# CI Reference

What each GitHub Actions workflow runs, when, and why. Split out of `Workflow.md` when it reached a
third of that file; the **design intent** behind this shape — which platform validates what, and why
Windows is not redundant — lives in `Workflow.md`'s "Standing decisions", not here. This document is
the mechanics.

| I want to… | Read |
|---|---|
| know what blocks my PR | [The per-PR gate](#the-per-pr-gate-build-and-testyml) |
| understand a nightly failure | [Nightly](#nightly-nightlyyml) |
| measure strength in CI | [Strength lab](#strength-lab-strengthyml) |
| know why validation is split across platforms | `Workflow.md` → Standing decisions |

---

## The per-PR gate (`build-and-test.yml`)

`.github/workflows/build-and-test.yml` runs an independent build + fast-test check on **Linux**,
tier-gated by `classify` on pull requests and on merges alike: Build and Engine changes only, so a
Docs or Tooling change skips it either way.

**How `classify` reads each event.** A pull request diffs against `origin/main`. A push cannot —
on a push to `main`, `origin/main` *is* `HEAD`, so the diff is empty and every merge classified as
Docs regardless of content (#185). Pushes therefore diff against `github.event.before`, the tip
`main` held before the push. If that ref is unreachable — a force push, or the all-zeros SHA on
branch creation — `Get-ChangeTier.ps1` fails closed to Engine tier. Leave that path alone.

`classify` also runs `Test-WorkflowTimeouts.ps1`, which fails the run if any job in any workflow
omits `timeout-minutes`. It lives here because `classify` is the only job with no tier condition, and
`Validate-PrePR.ps1` runs the same script so the answer is reachable before pushing.

Consequence for the deps cache: `main` now only builds on Build/Engine merges, and `actions/cache`
is branch-scoped so a PR can only restore a cache saved there. This is safe because the key is static
and changes only on a dependency bump, which is a `CMakeLists.txt` edit and so still Build tier — and
because eviction is seven days without *access*, which every PR restore refreshes.

**The four Linux jobs also run through ccache** (`build-linux` Release and Debug, `sanitize-linux`,
`tsan-linux`) — `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` on each job's configure line, when the install
below succeeds.

**`build-and-test (Release)` runs through it too; `build-and-test (Debug)` does not** (#377). ccache
cannot cache `/Zi`, which needs debug information embedded in the object instead, and Debug carries
`/Zi` on every translation unit. Release carries no debug-information flag at all, so it needs no
decision about one — making Debug cacheable would mean `/Z7`, a change to how the shipping toolchain
builds rather than an added cache. That leg reaches CMake differently from the Linux ones: it goes
through `build.ps1`, which drives a preset and takes no pass-through for cache variables, so the
launcher arrives as the `CMAKE_CXX_COMPILER_LAUNCHER` environment variable, which CMake reads at
first configure. Do not add a parameter to `build.ps1` for this.

**What caching Release alone can buy is bounded by the Debug leg.** `build-and-test` is a two-leg
matrix and both legs must finish, so the job's duration is the slower one. Across 27 merge runs on
`main` the Build-step medians were 183 s Release against 156 s Debug, and the prize is that gap.
#377's opening post costs the work against a "316 s → 175 s" ceiling that assumed Windows leaves the
critical path entirely; it does not, and that figure should not be quoted.

ccache is not on the `ubuntu-24.04` image, so it is installed from its upstream static release
tarball rather than apt, for the same reason the standing decision above rules out an apt install
anywhere else here: it would put an Ubuntu apt mirror on the critical path of every Linux job. The
Windows leg installs the `windows-x86_64` asset of the same release the same way, from the same
composite action, so one bump moves every configuration at once. A version bump carries **both**
pinned SHA-256 values forward — never drop a hash to make an upgrade easier —
and happens only after reading the release notes and open issues for correctness regressions, since a
compiler cache that silently serves a wrong object produces a wrong binary that tests may not catch.
The install degrades rather than fails the job on a download or verification miss: the required check
stays green and merely as slow as it is without ccache, with a `::warning::` annotation on the run so
the degradation is visible rather than silently eating the wall-clock saving for weeks.

Each caching job keeps its own `actions/cache` entry (`ccache-linux-release`, `-debug`,
`-asan-ubsan-stdlibdebug`, `-tsan`, `ccache-windows-clang-cl-release`), capped at
`CCACHE_MAXSIZE=400M` each — about 2 GB total against
the repository-wide 10 GB budget shared with the deps cache above. That sharing is the eviction risk:
`actions/cache` entries are immutable, so each ccache-carrying run writes a new entry per job, and if
that churn evicts the FetchContent entry, its miss costs a fresh dependency clone of about a minute —
which would make this change net negative while every job still reports green. The kill criterion:
if a `gh cache list` check one week after landing shows the FetchContent entry missing, or a
wall-clock reading of `sanitize-linux` merge runs on `main` shows no median improvement of at least
20 s, revert. Method and the measured reading: `Docs/Changelog.md` and `.claude/plans/ccache-linux-ci.md`.

**Checking a cached build produced the right thing is the same gate on both platforms: a
byte-identical binary, cold cache versus warm.** That holds on Windows only because
`strat_configure_target` passes `/Brepro` to the compiler and the linker. Without it the clang-cl
build is not reproducible at all — every COFF object carries a `TimeDateStamp` and every PE image a
header timestamp, and two clean builds of one commit differ in 61 of 90 objects and both executables,
with ccache nowhere in the picture.

**Compare rebuilds in the same build directory.** `/Z7` embeds each object's own path in its
`debug$S` record, so a Debug comparison across two differently-named build directories reports
differences that a rebuild in place never has. That is a property of the measurement, not of the
build.

**Windows runs on every Build- and Engine-tier change**, same trigger as Linux. It is the only job
that builds what ships — the clang-cl branch of `strat_configure_target`, the eight
`_MSC_VER`/`_WIN32` sites, the MSVC standard library, and the lld-link/ThinLTO link of
`StratChessEvolved.exe` itself. Two of the three clang-cl flag spellings fail *silently* when wrong
(#84), so Linux cannot stand in for those.

`Validate-PrePR.ps1` does **not** cover it. The script never passes `-Config`, so it builds Release
only — Windows Debug (MSVC's checked iterators, `assert()` live on the shipping compiler) is compiled
nowhere else, locally or in CI.

**`build-linux`** builds `all`, not just the test target: `StratChessEvolved.cpp` — `main()` and the
perft, tactical and eval runners — belongs to no other target, so a tests-only build never compiled it
with GCC. It then runs the fast tier, and on the **Release leg only**, **`perft test`** — the
131-position / 655-check suite behind `Tests/perft_test_cases.json`, which previously ran in no
automated gate at all. The Catch2 `[perft]` tests cover only seven hardcoded cases (startpos d1-4,
Kiwipete d1-3).

Release-only because perft is compute-bound: the suite takes **30 s** optimised, and the Debug leg
reached 4 of 131 positions in six minutes — roughly three hours extrapolated. Never put a perft suite
on a Debug leg.

**`sanitize-linux`** builds the test binary with `-fsanitize=address,undefined` and
`STRAT_STDLIB_DEBUG=ON` — libstdc++ debug mode, i.e. checked iterators and container preconditions,
which `_GLIBCXX_ASSERTIONS` (bounds only) misses and MSVC covers with `_ITERATOR_DEBUG_LEVEL=2` — and
runs the fast tier. It shares `build-linux`'s trigger exactly — Build and Engine tiers, on PRs and
merges alike — deliberately, rather than being narrowed to Engine: a Build-tier change to `CMakeLists.txt` is
precisely what can break the sanitizer wiring, and two conditions would eventually drift apart.
It is the only job that can catch a *silent* fault: an
out-of-bounds read of the magic tables, a PST, a killer/history table or the mailbox does not crash,
it returns a wrong evaluation. Debug rather than Release, so `assert()` and the `#ifndef NDEBUG`
tripwires stay live alongside the instrumentation. Linux-only — the GNU `-fsanitize=` spelling does
not survive the MSVC driver, and `CMakeLists.txt` raises a configure error rather than letting a
Windows build look instrumented when it is not.

**`tsan-linux`** builds `StratChessEvolved` with `-fsanitize=thread` and runs
`.github/scripts/tsan_smp_drive.py`, which drives six multi-threaded scenarios over UCI at
`Threads=4`, `8` and `16` — including a time-managed `movetime` abort, the one path where a search
ends on something other than its own depth limit. The job takes **197 s** (85 s build, 86 s drive),
under the `build-linux (Release)` critical path, so it adds no wall-clock time to a PR. Same trigger
as the two jobs above, for the same reason.

There is no `stop` scenario: a TSan-instrumented engine never answers `stop` with a `bestmove`, while
a clean build of the same commit answers in 0.00 s (#243). So the abort-on-request path is out of
reach here; `movetime` exercises the time-manager half of the same mechanism.

It does **not** run the Catch2 tier, and that is the point of the job's design. The `[smp]` tests only
check `SetThreads()` clamping and the rest of the tier is single-threaded, so a TSan run over it
spawns no helper threads and cannot fail for the reason the job exists — while a second instrumented
target plus 65 s of instrumented test execution would push the job past `build-linux (Release)`, the
current critical path, and slow every full-tier PR. `sanitize-linux` already runs that tier.

Two mechanics that are easy to get wrong, both of which produce a *falsely clean* run:
`setarch $(uname -m) -R` disables ASLR, without which TSan dies with `unexpected memory mapping`
before `main` on Ubuntu 24.04 and reports nothing; and the driver waits for `uciok`/`readyok`/
`bestmove` rather than piping commands, which would otherwise arrive mid-search and be refused by the
UCI guards. TSan cannot be combined with ASan, hence a separate job. Survey, positive control, cost
and contention analysis: `.claude/plans/tsan-lazy-smp.md`.

**`lint-linux`** runs the shared `Run-Lint.ps1` entry point over files the PR touches, on the same
tier condition as the jobs above:

| Tool | Scope | Effect |
|---|---|---|
| clang-format | changed `.cpp` and `.h` | **Blocking** |
| clang-tidy Gate | changed `.cpp` only | **Blocking** for findings and infrastructure failures |

`Validate-PrePR.ps1` calls the same Gate runner after its shipping clang-cl build, so local and CI
validation share file selection, normalization, checks, worker behavior, and failure rules. Direct
local invocations are:

```powershell
pwsh -File StratChessEvolved/Scripts/Run-Lint.ps1 -Check Tidy -Profile Gate
pwsh -File StratChessEvolved/Scripts/Run-Lint.ps1 -Check Tidy -Profile Gate -All
pwsh -File StratChessEvolved/Scripts/Run-Lint.ps1 -Check Tidy -Profile Deep -All
```

### clang-tidy profiles

| Profile | Checks | Source scope | Where it blocks | Workers |
|---|---|---|---|---:|
| Gate | `bugprone-*`, `performance-*` | Engine, application, and tests | PrePR and required PR CI; whole tree Nightly | 4 |
| Deep | `clang-analyzer-*`, `bugprone-exception-escape` | Shipping Engine/application only | Nightly Linux and Windows | 2 |

Gate excludes `bugprone-throwing-static-initialization` (Catch2 registration),
`bugprone-easily-swappable-parameters` (the move API intentionally has adjacent same-typed values),
and the checks assigned to Deep. `StratChessTests/.clang-tidy` additionally disables
`bugprone-unchecked-optional-access`, because clang-tidy does not model Catch2 `REQUIRE`, and
`performance-*`, because test code favors clarity over micro-optimization. Deep does not analyze
test translation units.

Both profiles set `WarningsAsErrors: '*'`. A finding, non-zero worker, missing worker result,
malformed/missing database, failed diff, or normalization ambiguity fails the invocation. A changed
scope with no `.cpp` is the only valid zero-TU result; whole-tree lint selecting zero TUs fails.

### Compilation database and workers

`New-TidyCompileDatabase.ps1` canonicalizes source paths and writes a separate database for each
profile. CMake builds every Engine source for both `StratChessEvolved` and `StratChessTests`; the
normalizer retains the shipping `StratChessEvolved` command and rejects ambiguous or missing shipping
candidates. The Windows database currently reports **74 inputs, 50 canonical sources, 50 selected
commands, and 24 duplicate target entries removed**. The original build database is never changed.

The runner starts one clang-tidy process per selected TU through a bounded pool. Gate defaults to four
workers and Deep to two; required/Nightly CI passes those values explicitly. `-Jobs 1` is the serial
diagnostic baseline, and any positive `-Jobs` value is accepted. Each worker's stdout, stderr, and exit
code are captured separately, then non-empty results are printed in canonical source order. The summary
includes tool/version, profile/config, normalized and selected counts, requested/effective workers,
completed invocations, elapsed time, and findings grouped by check.

**LLVM is pinned to major 22**, installed from apt.llvm.org rather than using the image's
`clang-tidy-18`. The check inventory differs between clang-tidy majors, so an unpinned runner
silently gains and loses checks when the image moves. Major 22 is what Visual Studio 18 ships, so
developers already have it; the CI patch level is 22.1.8 against the VS toolchain's 22.1.3, and
clang-format output was verified byte-identical between them across the source tree — which is what
makes a blocking format check safe. `Run-Lint.ps1` warns when the local major differs.

The lint database is configured with **clang, not the default GCC**, and this is load-bearing rather
than cosmetic. `strat_configure_target` emits `-fconstexpr-ops-limit=` for GCC and
`-fconstexpr-steps=` for Clang; clang-tidy consumes the database through the clang driver, which
rejects the GNU spelling as an unknown argument. Against a GCC database every translation unit fails
to parse. The runner fails that infrastructure error and reports completed invocation counts as a
positive control.

A header is not a translation unit, so a PR touching only `.h` files analyses nothing here. That gap
is closed by the nightly `lint-tree` job rather than by header-to-TU mapping, which a change to
`defines.h` or `StdAfx.h` would expand to a whole-tree run — the one shape capable of becoming the
critical path. Changes to a lint config, `Run-Lint.ps1`, or the database normalizer deliberately
expand to the whole tree so the gate machinery validates itself.

Local Windows/clang-cl whole-tree measurements on 2026-08-13 show why the bounded defaults matter:

| Profile | TUs | Workers | Findings | Elapsed |
|---|---:|---:|---:|---:|
| Gate | 49 | 1 | 0 | 86.0 s |
| Gate | 49 | 4 | 0 | 24.7 s |
| Deep | 25 | 1 | 0 | 154.8 s |
| Deep | 25 | 2 | 0 | 71.8 s |

Required PR CI analyzes only changed TUs, so it normally stays below the existing build critical path.
Whole-tree Gate and both Deep platform runs belong to Nightly.

**CI is a gate.** `build-and-test-result` is a required check on `main`, so a red run blocks the
merge. A SKIPPED leg reports success deliberately: a Docs-tier PR runs none of the build jobs, and a
required check that never ran would block it forever.

Dependencies come from `FetchContent` at the versions pinned in `CMakeLists.txt`; each job caches
`build/_deps`. `build-linux` builds `all`, then runs the fast Catch2 tier and, in Release, the
executable's `perft test` suite. Windows builds `all` in Release and Debug, then runs the fast tier.

Runner image is pinned to `windows-2025-vs2026`, not `windows-latest`, so the toolchain moves only
when it is changed deliberately — see `.claude/plans/full-build-test-ci-github-actions.md`.

`Check starting FEN` is path-filtered to `StratChessEvolved/game_settings.json` and does not run
otherwise.

Self-play stays local-only (`Validate-PrePR.ps1`); its timeout-based nondeterminism is not worth CI
flakiness. The `[slow]` Catch2 tier runs in `extended-tests` and `sanitize-extended` nightly jobs.

---

## Nightly (`nightly.yml`)

**`nightly.yml`** runs at 03:00 UTC and on `workflow_dispatch`, and gates nothing — it answers "is
`main` still correct?", not "may this land?".

| Job | What it adds over the per-PR gate |
|---|---|
| `deep-perft` | `perft(7)` from the start position (3,195,901,860 nodes) and `perft(6)` from Kiwipete (8,031,647,685), each compared against the known count. The fast tier stops at depth 4 |
| `extended-tests` | The `[slow]` tier, Release and Debug |
| `sanitize-extended` | That tier under ASan+UBSan plus `_GLIBCXX_DEBUG` |
| `tactical-stability` | `tactical stability 100`, against the local run's 10 |
| `lint-tree` | Failing clang-format and fast Gate over the whole tree, closing the per-PR job's header gap |
| `lint-deep-linux` | Failing Deep profile over normalized shipping sources with Linux Clang |
| `lint-deep-windows` | Failing Deep profile over normalized shipping sources with Windows clang-cl |

`perft run <depth> [fen]` prints a count but does not verify it, so the workflow does the comparison.
Runners measure **~22.5 Mnps** (startpos depth 7 in 140 s, Kiwipete depth 6 in 364 s — 2.2× slower
than a local build), so neither needs sharding across a matrix.

**Deeper perft was measured and declined.** startpos(8) is ~62 min and Kiwipete(7) ~4.7 h at that
rate, the latter being 78% of GitHub's 6-hour job cap — it would need a 48-way root-move shard to be
safe. Neither buys coverage: startpos(7) already returns 3,195,901,860, so the 32-bit boundary is
already crossed, and perft allocates nothing per node, so a longer run stresses nothing. Move
generation is exercised by **breadth**, which is what `perft test` provides. Do not re-propose depth
without a reason that survives those numbers.

**The `[slow]` tier is thin.** The fast tier is 250 test cases; everything is 253 — two deep tactical
searches and the null-move guards. `extended-tests` and `sanitize-extended` are worth their (free)
minutes, but a green run there is weak evidence, and "extended tier" oversells what exists. Growing
it is #156's territory, not the schedule's.

---

## Strength lab (`strength.yml`)

**`strength.yml`** is the CI strength lab: `workflow_dispatch` only, candidate against a reference
ref with both sides built from source by the same GCC. It reports pooled Elo to the job summary and
uploads every shard's PGN; it gates nothing and is triggered by nobody automatically.

Dispatch-only is not a stepping stone to be skipped past. A measurement harness that is wrong is
worse than none, because its output looks exactly like a measurement — so it stays manual, and its
numbers are only trusted because the null test and the known-sign control were run first. Both are
in `Docs/EloLog.md`'s Linux ledger.

**Four jobs.** `setup` turns the requested game count into a shard plan and self-tests the pooling
formula before anything expensive runs. `build` compiles both engines and stages them with fastchess
and the book as **one artifact**, so every shard provably plays the same two binaries against the
same book. `match` is the shard matrix. `aggregate` pools them.

| Input | Meaning |
|---|---|
| `reference_ref` | Reference side, default `merge-base` — the commit this ref forked from `main`, so the result is attributable to this change alone. A tag such as `elo-reference-v2` measures cumulative strength instead; the candidate's own SHA is a null test. Resolved and verified in `setup`, so a bad ref fails in seconds |
| `games` | Total games across all shards, two per opening pair. Rounded down so each shard gets whole pairs |
| `shards` | Parallel match jobs, default 18. 18×1110 games is ~3 h and leaves 2 of the 20 concurrent-job slots free, so a run no longer blocks every other PR; 20 consumes the whole allowance for the duration. Below ~16 a shard can exceed the 340-minute job timeout |
| `candidate_tc` / `reference_tc` | Per-side time control. Halve the **base** for a handicap run — an increment under 0.1 s makes the engine play near-instantly at the bottom of its clock |
| `concurrency` | Concurrent games **per shard**. Validated at 3; raising it causes contention, adding shards does not |

**One run at a time, repository-wide.** The workflow's concurrency group is the constant
`strength-lab`, not one keyed on the ref: a run takes 18 of the 20 concurrent jobs, so two runs on
different refs would both start with nine shards each and starve normal CI for longer than either
needs. A running batch is never cancelled. Only one run can wait, though — GitHub keeps a single
pending run per group and cancels the previously pending one, so a third dispatch supersedes the
second.

**Shard slices are disjoint by arithmetic.** With `order=sequential`, shard *i* playing *R* pairs
starts at opening `i*R + 1`. `aggregate` re-checks this every run by comparing the first FEN of each
shard's PGN, and fails if two match.

**The error bar is pentanomial** — pooled over colour-swapped *pairs*, which are the independent
unit, by `.github/scripts/pool_pentanomial.py`. Pooling raw W/L/D would understate the variance and
produce an interval that is wrong in the direction of looking more precise. That script's
`--self-test` reproduces fastchess's own Elo and interval on six real matches from this project;
`setup` runs it before any build.

A batch reporting a time loss, an illegal move or a disconnect is **discarded, never reported** —
same rule as `Run-EloMatch.ps1`, and on a shared runner a time loss most likely means the box was
oversubscribed, which invalidates the whole batch rather than the one game. **A failed shard
discards the whole batch**, not just itself: the survivors are the ones that happened to avoid
whatever went wrong, so pooling them would be a biased subset wearing a full batch's error bar.
Numbers land in `Docs/EloLog.md`'s **Linux ledger**, which must never be compared against the local
clang-cl rows.

At the default 18 shards, a strength run occupies 18 of the 20 concurrent-job slots for ~3 hours.
It can delay other CI, but leaves two slots; choosing 20 shards consumes the allowance. This is why
the lab is not wired to trigger automatically.
