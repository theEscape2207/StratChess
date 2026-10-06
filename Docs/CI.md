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
| know what else scans the repo | [Repository services](#repository-services) |

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

`classify` also runs three guards, each failing the run when:

- `Test-WorkflowTimeouts.ps1`: a job in any workflow omits `timeout-minutes`;
- `Test-WorkflowCcachePaths.ps1`: a workflow or composite action sets ccache's `base_dir` or
  `hash_dir` (see below);
- `Test-ScriptBinding.ps1`: a script with a `param()` block lacks `[CmdletBinding()]`.

They live here because `classify` is the only job with no tier condition. `Validate-PrePR.ps1` runs
the same scripts on Build and Engine tiers, so the answer is reachable before pushing.

Consequence for the deps cache: `main` now only builds on Build/Engine merges, and `actions/cache`
is branch-scoped so a PR can only restore a cache saved there. This is safe because the key is static
and changes only on a dependency bump, which is a `CMakeLists.txt` edit and so still Build tier — and
because eviction is seven days without *access*, which every PR restore refreshes.

**ccache fronts the compiler on all six build jobs**: the four Linux ones (`build-linux` Release and
Debug, `sanitize-linux`, `tsan-linux`) via `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` on the configure
line, and both `build-and-test` legs via the `CMAKE_CXX_COMPILER_LAUNCHER` environment variable —
those go through `build.ps1`, which takes no pass-through for cache variables, and CMake reads the
variable at first configure. **Do not add a parameter to `build.ps1` for this.** Every call site must
drop the launcher when the install reports unavailable: CMake does not check that a launcher exists,
so it configures happily and then fails at the first Ninja edge.

Caching the Windows Debug leg is why `CMakeLists.txt` sets `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` to
`Embedded` (`/Z7`) and requires CMake 3.25 for CMP0141 — ccache cannot cache `/Zi`. Both legs matter
because this is a two-leg matrix: the job takes the duration of the slower leg, so caching one alone
moved it ~5 s (#377/#380).

**Debug is the slower leg, and its Build step is not why** (#514, 14 paired `main` merge runs). Means
per leg: Build 75 s Release against 70 s Debug — indistinguishable beside a per-run spread of 33-148 s
that tracks cache warmth, not configuration — while `Run fast tests` costs 34 s in Debug against 14 s
in Release. That 20 s is most of the 17 s median gap in job total. So the lever on this job is test
execution in Debug, not the compile; ccache already caches the compile, and there is no
Release-specific attribution left to chase.

ccache is not on the Ubuntu runner images and is installed from the upstream release archive rather
than apt — an apt mirror on the critical path of every Linux job is what the standing decision above
rules out. Both platforms install from the same composite action, so one bump moves every
configuration. **A bump carries both pinned SHA-256 values forward** — never drop a hash to make an
upgrade easier — and follows a read of the release notes for correctness regressions: a cache that
serves a wrong object produces a wrong binary tests may not catch. A download or verification miss
degrades rather than fails, with a `::warning::` annotation so it is visible. Both halves of that
fallback have been exercised against a broken download — Linux at #375, Windows at #385 — and the
proof it worked is the **stats steps reporting skipped**, since they are the only thing gated on
`available` reaching the caller. A test that breaks the download URL must bust the `ccache-bin-…`
cache key too, or the binary restores, the download is never attempted, and the test passes vacuously.

Each caching job keeps its own entry (`ccache-linux-gcc15-release`, `-debug`,
`ccache-linux-gcc15-asan-ubsan-stdlibdebug`, `ccache-linux-gcc15-tsan`, `ccache-windows-clang-cl-release`,
`-debug`) at `CCACHE_MAXSIZE=400M` (`sanitize-linux`: 600M). `actions/cache`
entries are immutable, so **every run writes six new ones** and the store carries a generation per
run until LRU trims it — an order of magnitude more than one generation, against a budget shared with
the FetchContent deps cache. That sharing was the risk this change was gated on: churn evicting a
deps entry costs a fresh clone, turning ccache net negative while every job still reports green.

**No CI job sets `base_dir` or `hash_dir`, and `Test-WorkflowCcachePaths.ps1` fails the run if one
ever does.** Both are local-workflow levers for cross-*worktree* hits (#510, #511); CI has no such
problem, because every job builds at a stable path and restores an entry produced at that same path,
so the compile command lines — and the paths embedded in the objects they produce — match the
consuming tree by construction. Setting either to chase a cross-job hit rate would trade that
guarantee for a hazard whose failure mode is a stale artifact on a green build — which is why the
tripwire is an assertion rather than this paragraph.

Cache **scope** makes much of that churn avoidable. A run on a PR writes its six entries to
`refs/pull/N/merge`, and `actions/cache` reads only from the run's own ref or from the default
branch — so no other PR, and no run on `main`, can ever restore them. `pr-closed-cleanup.yml`
deletes them when the PR closes. What accumulates on `refs/heads/main` is the part `restore-keys`
can still reach.

**The gate was read a week after landing and the change kept** (#385). LRU does evict, and it evicts
the right entries — an object cache is read once by the next run through `restore-keys` and never
again, while the deps entries are touched by every Build-tier run, which puts them at the top of the
LRU list rather than the bottom. Both survived. Build-step medians on `main` merge runs fell from
235 s to 20 s on `sanitize-linux`, and from 198 s and 167 s to 49 s and 37 s on the `build-and-test`
Release and Debug legs, against a 20 s revert threshold. Roughly one merge run in six is genuinely
cold: a wide `.cpp` or header change drops the hit rate under 40% and the build back to its uncached
duration, so **read `--show-stats` beside a duration** before blaming the cache for a slow run.
Readings: `Docs/Changelog.md`.

**The gate on a cached build is a byte-identical binary, cold cache versus warm, on both platforms.**
On Windows that works only because `strat_configure_target` passes `/Brepro` to compiler and linker;
without it COFF and PE timestamps make two clean builds of one commit differ (#381). **Compare
rebuilds in the same build directory** — `/Z7` embeds each object's path in its `debug$S` record, so
comparing differently-named build directories reports differences a rebuild in place never has.

`Scripts/Test-ReleaseReproducibility.ps1` is that comparison, in both modes: `-Mode Determinism`
(two cold builds) and `-Mode Cache` (an uncached reference, then a build served from a freshly
populated cache — **the reference is uncached on purpose**, since comparing the cold cached build
with the warm one compares a cache entry against the copy it was made from and holds whatever the
cache returns). It runs on demand — two full builds, three under `-Mode Cache`, and nothing invokes
it automatically — so run it when a change touches the build configuration or the toolchain.

**Release rests on a different basis from Debug's, not a weaker one:** the engine target
links with ThinLTO, where the compiler emits bitcode rather than COFF, so the compile-side `/Brepro`
is inert there and identity comes from frontend determinism, while the linker-side `/Brepro` still
settles the PE header. Both halves stay — the ~89 non-LTO compile edges do emit COFF and do need the
compile flag. Measured 2026-09-19: 102 of 102 project artifacts byte-identical, Release, both modes.
Out of scope, and not by accident: CMake's own configure probes, which see no `/Brepro` and are
reproducible in neither configuration, and the dependency `.lib`s, which are built outside the preset
tree and never receive it.

**The Release leg also asserts hot-code alignment** (`Test-CodeAlignment.ps1`, #513). It reads the
linker map the build just emitted and requires `pvs` and `quiescence` at `%64 == 0`, so it costs a
file read. It runs here as well as in `Validate-PrePR.ps1` because the failure it guards needs no
diff: `-falign-functions=64` works only while clang-cl keeps translating the spelling and link-time
codegen keeps honouring `align 64`, and a toolchain upgrade that ended either would leave a green
build with layout variance quietly back to what #555 measured.

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

Every Linux job in `build-and-test.yml`, `nightly.yml` and `strength.yml` runs on `ubuntu-26.04`,
the GCC ones with its default GCC 15. The ccache keys carry `-gcc15`, so a job on another GCC major
never shares, and overwrites, one cache.
26.04 runs transparent huge pages in `madvise` mode, not 24.04's `always`; the engine advises its
TT allocation itself, so a full-size table gets huge pages on either image.

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
ends on something other than its own depth limit. The drive, not the build, is what the job costs;
it runs under the longest Build-tier job either way, so it adds no wall-clock time to a PR. Same
trigger as the two jobs above, for the same reason.

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
before `main` on the runner kernel and reports nothing; and the driver waits for `uciok`/`readyok`/
`bestmove` rather than piping commands, which would otherwise arrive mid-search and be refused by the
UCI guards. TSan cannot be combined with ASan, hence a separate job. Survey, positive control, cost
and contention analysis: `.claude/plans/retained/tsan-lazy-smp.md`.

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
pwsh -File Scripts/Run-Lint.ps1 -Check Tidy -Profile Gate
pwsh -File Scripts/Run-Lint.ps1 -Check Tidy -Profile Gate -All
pwsh -File Scripts/Run-Lint.ps1 -Check Tidy -Profile Deep -All
```

### clang-tidy profiles

| Profile | Checks | Source scope | Where it blocks | Workers |
|---|---|---|---|---:|
| Gate | `bugprone-*`, `performance-*`, `misc-const-correctness` | Engine, application, and tests | PrePR and required PR CI; whole tree Nightly | 4 |
| Deep | `clang-analyzer-*`, `bugprone-exception-escape` | Shipping Engine/application only | Nightly Linux and Windows | 2 |

Gate excludes `bugprone-throwing-static-initialization` (Catch2 registration),
`bugprone-easily-swappable-parameters` (the move API intentionally has adjacent same-typed values),
and the checks assigned to Deep. `StratChessTests/.clang-tidy` additionally disables
`bugprone-unchecked-optional-access`, because clang-tidy does not model Catch2 `REQUIRE`,
`performance-*`, because test code favors clarity over micro-optimization, and
`misc-const-correctness`, because `const` on a local a test never reassigns is noise rather than
intent. Deep does not analyze test translation units.

`misc-const-correctness` is the one Gate check with a mass `--fix`, and it has two traps. Run it
**one translation unit at a time**: parallel `--fix` processes rewrite a shared header from their
own buffers, applying the same edit several times and dropping most of the others — observed as
`MoveType const const const type` and an insertion landing mid-identifier. Its fix-its are also
east-const, and it places `const` wrongly for arrays of pointers
(`const char* x[]` becomes `const char const* x[]`, which does not compile).

Both profiles set `WarningsAsErrors: '*'`. A finding, non-zero worker, missing worker result,
malformed/missing database, failed diff, or normalization ambiguity fails the invocation. A changed
scope with no `.cpp` is the only valid zero-TU result; whole-tree lint selecting zero TUs fails.

### Compilation database

`New-TidyCompileDatabase.ps1` writes a normalized database per profile and never modifies the build's
own. CMake compiles every Engine source for both `StratChessEvolved` and `StratChessTests`; the
normalizer retains the **shipping** command, so lint sees Release engine flags rather than test ones,
and fails on an ambiguous or missing candidate.

**LLVM is pinned to major 22**, called as `clang-tidy-22`/`clang-format-22` from the `ubuntu-26.04`
image, which also ships 20 and 21. The check inventory differs between clang-tidy majors, so an
unpinned runner silently gains and loses checks when the image moves. Major 22 is what Visual
Studio 18 ships, so developers already have it; clang-format output was verified byte-identical
across the source tree between the VS toolchain's 22.1.3 and the image's 22.1.2 — which is what
makes a blocking format check safe. `Run-Lint.ps1`
warns when the local major differs.

The lint database is configured with **clang, not the default GCC**, and this is load-bearing rather
than cosmetic. `strat_configure_target` emits `-fconstexpr-ops-limit=` for GCC and
`-fconstexpr-steps=` for Clang; clang-tidy consumes the database through the clang driver, which
rejects the GNU spelling as an unknown argument. Against a GCC database every translation unit fails
to parse. The runner fails that infrastructure error and reports completed invocation counts as a
positive control.

A header is not a translation unit, so a changed `.h` is analysed through **one** translation unit
that includes it, chosen from an include graph over the tracked sources. clang-tidy already reports
findings inside a header through `HeaderFilterRegex`; what it needs is an includer to reach them
from, and the header text is the same in every one, so the first is enough.

One rather than all, because the dependent set of a widely-included header is most of the tree, and a
whole-tree run is the one shape capable of making lint the critical path. Engine units are preferred
over test units, which carry Catch2 and analyse more slowly. A header no unit includes is reported as
uncovered and left to `lint-tree`.

Changes to a lint config, `Run-Lint.ps1`, or the database normalizer deliberately expand to the
whole tree so the gate machinery validates itself.

**CI is a gate.** `build-and-test-result` is a required check on `main`, so a red run blocks the
merge. A SKIPPED leg reports success deliberately: a Docs-tier PR runs none of the build jobs, and a
required check that never ran would block it forever.

Runner image is pinned to `windows-2025-vs2026`, not `windows-latest`, so the toolchain moves only
when it is changed deliberately — see `.claude/plans/retained/full-build-test-ci-github-actions.md`.

`check-starting-fen.yml` is path-filtered to `StratChessEvolved/game_settings.json` and does not run
otherwise. `pr-closed-cleanup.yml` fires once per closed PR — deleting same-repository head branches
and caches scoped to the PR ref whether the PR merged or was cancelled. Both gate nothing.

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
| `lint-tree` | Failing clang-format and fast Gate over the whole tree, covering what the per-PR job's one-unit-per-header cover does not reach |
| `lint-deep-linux` | Failing Deep profile over normalized shipping sources with Linux Clang |
| `lint-deep-windows` | Failing Deep profile over normalized shipping sources with Windows clang-cl, and `Validate-PrePR.ps1 -AllSelfTests` — every script self-test, against the PR gate's "only the ones the diff touched" |
| `agent-docs` | `Test-AgentDocs.ps1`: citations in skills, subagents, `CLAUDE.md` and `AGENTS.md` that no longer resolve. Not in PrePR |

`perft run <depth> [fen]` prints a count but does not verify it, so the workflow does the comparison.
Runners measure **~22.5 Mnps** (startpos depth 7 in 140 s, Kiwipete depth 6 in 364 s — 2.2× slower
than a local build), so neither needs sharding across a matrix.

**Deeper perft was measured and declined.** startpos(8) is ~62 min and Kiwipete(7) ~4.7 h at that
rate, the latter being 78% of GitHub's 6-hour job cap — it would need a 48-way root-move shard to be
safe. Neither buys coverage: startpos(7) already returns 3,195,901,860, so the 32-bit boundary is
already crossed, and perft allocates nothing per node, so a longer run stresses nothing. Move
generation is exercised by **breadth**, which is what `perft test` provides. Do not re-propose depth
without a reason that survives those numbers.

**The `[slow]` tier is thin**: a handful of deep tactical, null-move and endgame-conversion cases
beside the whole fast tier. `extended-tests` and `sanitize-extended` are worth their (free)
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
in `Measurements/ci-calibration.md`.

**Four jobs.** `setup` turns the requested game count into a shard plan and self-tests the pooling
formula, comparison preflight and shard verifier before anything expensive runs. `build` compiles
both engines, resolves the intended comparison, and stages them with fastchess and the book
as **one artifact**, so every shard provably plays the same two binaries against the same book.
`match` is the shard matrix. `aggregate` verifies the whole batch before pooling any arm.
The normal PR gate also runs these Python self-tests and workflow boundary fixtures in its Linux
Release leg, so harness failures can be checked without dispatching a strength match.

`.github/scripts/test_strength_workflow.py` runs the workflow's Bash blocks against complete and
incomplete fixture batches and models directory-upload relative paths. This protects the evidence
layout as well as the Python helpers: a misplaced retained book must fail locally before a match.

| Input | Meaning |
|---|---|
| `reference_ref` | Reference side, default `merge-base` — the commit this ref forked from `main`, so the result is attributable to this change alone. A tag such as `elo-reference-v2` measures cumulative strength instead; the candidate's own SHA is a null test only when build configuration, effective options and playing conditions also match. Resolved and verified in `setup`, so a bad ref fails in seconds |
| `cmake_defines` | Optional whitespace-separated `-DNAME=VALUE` arguments. The setup job rejects any other shape and reserves `CMAKE_*` so the fixed toolchain cannot be overridden; accepted arguments are applied identically to both builds and recorded in the run summary |
| `candidate_uci_options` / `reference_uci_options` | Optional whitespace-separated `Name=Value` UCI options, set on that side only — how a default-off runtime option is measured without a probe branch. `build` checks each against that engine's own advertised option table and fails the run on an unknown name, a wrong type or an out-of-range value, because the engine ignores all three in silence and the batch would otherwise report a null result. `Threads` is reserved for the `threads` input, a repeated name is rejected, and a value equal to the engine's default warns. Spin values must be **unsigned decimal digits** — the engine's UCI parser refuses a sign or a plus whatever the advertised minimum says, so an option with a negative minimum is not settable over UCI until that parser learns to read one. Both strings are recorded in the run summary |
| `candidate_arms` | Optional multi-arm screen: `;`-separated candidate option sets, each in `candidate_uci_options` syntax. Shard *i* plays arm *i* mod K, and each arm is pooled on its own shards against the shared reference, so an arm is a smaller batch: K arms of a 20k-game run get about √K times the full run's error bar each. `setup` rejects empty, malformed or duplicate arms, a `shards` count not divisible by K, and use together with `candidate_uci_options`; `build` checks every arm against the candidate as above. Routing lives in `.github/scripts/plan_arms.py`, which `setup` self-tests. One failed shard still discards every arm |
| `calibration` | Boolean, default false. Required for an intentionally identical arm. Declaring it on a known-sign control labels the report; different time controls already permit that comparison. It bypasses only the identical-comparison refusal |
| `opening_offset` | Openings to skip before shard 0, default 0. A multi-run experiment gives each run its own range, so a confirmation never replays the openings a screen selected its winner on. The book-size check counts it, and a non-zero offset is recorded in the run summary |
| `games` | Total games across all shards, two per opening pair. Rounded down so each shard gets whole pairs |
| `shards` | Parallel match jobs, default 18. 18×1110 games is ~3 h and leaves 2 of the 20 concurrent-job slots free, so a run no longer blocks every other PR; 20 consumes the whole allowance for the duration. Below ~16 a shard can exceed the 340-minute job timeout |
| `candidate_tc` / `reference_tc` | Per-side time control, finite decimal `seconds+increment`, base > 0 and increment >= 0; normalized so spelling cannot create a difference. Halve the **base** for a handicap run — an increment under 0.1 s makes the engine play near-instantly at the bottom of its clock |
| `concurrency` | Concurrent games **per shard**. Validated at 3; raising it causes contention, adding shards does not |
| `threads` | UCI `Threads` on **both** engines, default 1. `setup` refuses `threads` × `concurrency` above the runner's 4 vCPU, so `Threads=4` needs `concurrency` 1 — a third of the game rate, so cut `games` to keep a shard inside the job timeout. Unvalidated for time losses above 1 |

**One run at a time, repository-wide.** The workflow's concurrency group is the constant
`strength-lab`, not one keyed on the ref: a run takes 18 of the 20 concurrent jobs, so two runs on
different refs would both start with nine shards each and starve normal CI for longer than either
needs. A running batch is never cancelled. Only one run can wait, though — GitHub keeps a single
pending run per group and cancels the previously pending one, so a third dispatch supersedes the
second.

**The comparison is resolved before shards start.** `.github/scripts/compare_lab_configs.py`
queries both staged binaries even when overrides are empty. It requires a successful, complete
`uciok` reply and valid nonempty spin/check advertisements; unsupported types and duplicate names
fail. Defaults plus validated overrides become integer/boolean maps, with the harness-owned `threads` value.
Older references may lack newer options: their own table is used, and explicit missing overrides
fail. Redundant defaults, zero padding and option order do not distinguish conditions. Two candidate
arms resolving to the same map are refused even during calibration.

An arm is refused without `calibration` when its resolved settings and normalized time control
equal the reference's, and either their engine build inputs or staged binary bytes are identical.
Input identity compares tracked Git paths, modes and objects under `CMakeLists.txt`, `cmake/`,
`StratEngine/` and `StratChessEvolved/` at both revisions. Thus a docs-only commit cannot evade the
check; same source with different options remains a valid comparison. Shared CMake definitions,
toolchain/Release recipe and the shared `threads` value are recorded but cannot distinguish the sides.
Future per-side build inputs or generated engine inputs must extend this rule. Unequal inputs
permit a code comparison; they do not prove different chess behaviour.

The readable `comparison.md` records full revisions, input identity, binary SHA-256 hashes, build
definitions, time controls and every resolved arm with its differences. It is retained in the build
summary even after null refusal, then copied into the aggregate summary and PR comment. The
`strength-<run>-comparison` artifact retains it and the pinned book for 90 days, so an aggregate
rerun does not depend on the one-day executable toolkit. Missing evidence prevents a result.
These are **intended settings**, validated by advertisement, syntax and domain, not runtime readback
or proof that `setoption` took effect. Cross-field engine constraints remain outside this check;
`readyok` would not acknowledge individual settings. New measurement rows link this evidence.

**Shard slices are disjoint by arithmetic.** With `order=sequential`, shard *i* playing *R* pairs
starts at opening `offset + i*R + 1` (one-based nonblank EPD lines). The verifier requires exactly
one canonical artifact directory per index 0..S-1 with both log and PGN, including single-arm runs.
Every game's names must identify that shard's assigned arm and the shared reference. It requires
exactly 2R games and rounds 1..R each twice with opposite colours, and checks both round-one FENs'
four position fields against the assigned book entry. Shard starts must also differ. Concurrent
games finish out of order, so the first physical PGN game is not the opening-start evidence.
This establishes routing, completion and assigned starts, not every opening or move legality.

**The error bar is pentanomial** — pooled over colour-swapped *pairs*, which are the independent
unit, by `.github/scripts/pool_pentanomial.py`. Pooling raw W/L/D would understate the variance and
produce an interval that is wrong in the direction of looking more precise. That script's
`--self-test` reproduces fastchess's own Elo and interval on seven real matches from this project;
`setup` runs it before any build.

Every workflow pooling call supplies `--expect-pairs-per-shard R`. The last pentanomial counts of
each log must sum to R, even if missing pairs in one log would offset excess pairs in another.
Standalone historical pooling may omit the flag. All batch checks pass before pooling, and all
arm pools succeed before any result is published; failures report DISCARDED with no partial Elo.
The checks establish internal consistency, not authenticated provenance. Local fixtures exercise
the comparison and completion failures; a fresh strength run is unnecessary for harness changes.

A batch reporting a time loss, an illegal move or a disconnect is **discarded, never reported** —
same rule as `Run-EloMatch.ps1`, and on a shared runner a time loss most likely means the box was
oversubscribed, which invalidates the whole batch rather than the one game. **A failed shard
discards the whole batch**, not just itself: the survivors are the ones that happened to avoid
whatever went wrong, so pooling them would be a biased subset wearing a full batch's error bar.
Numbers land in `Measurements/ci-per-change.md` or `ci-anchor.md`, which must never be compared
against the local clang-cl rows. **Ratios transfer between the two instruments; absolute values
do not.**

**Two deliberate differences from the local setup.** The book is `UHO_4060_v3.epd` (242,201
openings — 4060 names the evaluation band the positions were selected from, not their count),
downloaded per run from a pinned commit of `official-stockfish/books` rather than committed. And
the lab runs a **fixed N with no SPRT**: a sequential test does not shard across runners, so it
buys resolution with games instead, which is affordable because the minutes are free.

Every run uploads the annotated PGN of every game it played, retained 90 days.
[`MoveQuality.md`](MoveQuality.md) is the method for reading them: the pooled Elo says whether a
change helped, that scan says where.

At the default 18 shards, a strength run occupies 18 of the 20 concurrent-job slots for ~3 hours.
It can delay other CI, but leaves two slots; choosing 20 shards consumes the allowance. This is why
the lab is not wired to trigger automatically.

---

## Repository services

These are configured outside the workflows, and none of them uses a runner slot.

- **Dependabot** (`.github/dependabot.yml`) opens one grouped PR a month that bumps the workflow
  actions. It covers nothing else, because it can't read the C++ dependencies pinned by
  `FetchContent`. Bump those by hand.
- **Secret scanning and push protection** are enabled in the repo settings.
- **CodeQL is off.** Its default setup took runner slots ahead of `classify`, which delayed the
  whole gate, and it found nothing the threat model cares about.
