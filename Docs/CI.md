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

Consequence for the deps cache: `main` now only builds on Build/Engine merges, and `actions/cache`
is branch-scoped so a PR can only restore a cache saved there. This is safe because the key is static
and changes only on a dependency bump, which is a `CMakeLists.txt` edit and so still Build tier — and
because eviction is seven days without *access*, which every PR restore refreshes.

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
`.github/scripts/tsan_smp_drive.py`, which drives four multi-threaded searches over UCI at
`Threads=4` and `Threads=8`. Same trigger as the two jobs above, for the same reason.

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

**CI is a gate.** `build-and-test-result` is a required check on `main`, so a red run blocks the
merge. A SKIPPED leg reports success deliberately: a Docs-tier PR runs none of the build jobs, and a
required check that never ran would block it forever.

Dependencies come from `FetchContent` at the versions pinned in `CMakeLists.txt`, cached as
`build/_deps` and shared by both matrix legs. The job runs `build.ps1 tests` (not `all`): CI never
runs `StratChessEvolved.exe`, and only the engine target carries `INTERPROCEDURAL_OPTIMIZATION` for
Release, so building `all` would spend most of the wall time on an LTO link of an unused binary.

Runner image is pinned to `windows-2025-vs2026`, not `windows-latest`, so the toolchain moves only
when it is changed deliberately — see `.claude/plans/full-build-test-ci-github-actions.md`.

`Check starting FEN` is path-filtered to `StratChessEvolved/game_settings.json` and does not run
otherwise.

Extended `[slow]` tests and self-play stay local-only (`Validate-PrePR.ps1`) — self-play's
timeout-based nondeterminism is not worth the CI flakiness.

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

**A full dispatch consumes the entire 20-job concurrent allowance for ~3 hours**, and
`build-and-test` is a required check — so a strength run in flight will queue everyone else's merges.
Worth knowing before starting one, and the reason the lab is not wired to trigger automatically.
