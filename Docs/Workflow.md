# Workflow Reference

Detail that `CLAUDE.md` points at rather than carries. `CLAUDE.md` holds the rules that change
what you do; this file holds the background you consult when something is unexpected.

## Find what you came for

| I want to… | Read |
|---|---|
| know what validation my change needs | [Validation tiers](#validation-tiers) |
| decide whether to dispatch `search-reviewer` | [When `search-reviewer` may be skipped](#when-search-reviewer-may-be-skipped) |
| start a task, or clean one up afterwards | [Two ways to run a task](#two-ways-to-run-a-task) |
| run an AI-vs-AI game by hand | [Self-play validation](#self-play-validation) |
| know what CI runs, and when | [The per-PR gate](#the-per-pr-gate-build-and-testyml) |
| understand a nightly failure | [Nightly](#nightly-nightlyyml) |
| measure strength in CI | [Strength lab](#strength-lab-strengthyml) |
| set up Visual Studio | [Working in Visual Studio](#working-in-visual-studio) |
| understand a first-build or network failure | [Dependency cache](#dependency-cache) |
| drive CMake directly | [Raw CMake invocation](#raw-cmake-invocation-fallback) |
| clean up a worktree that will not go away | [Worktree removal gotchas](#worktree-removal-gotchas) |
| reproduce an ASan/UBSan finding | [Reproducing a sanitizer finding](#reproducing-a-sanitizer-finding) |
| find out where a log file came from | [Runtime output files](#runtime-output-files) |

Three things are non-negotiable and are the reason most of this file exists:

- **Every task forks fresh from `origin/main`.** Never from `master`, never from the previous task.
- **A batch reporting a time loss, illegal move or disconnect is discarded, never reported.**
- **Never measure an MSVC build against a clang-cl one.** The compiler gap alone is worth tens of Elo.

---

# Part 1 — Doing the work

## Validation tiers

`Scripts\Validate-PrePR.ps1` scopes itself to what changed, so there is no judgement call to make.
`Scripts\Get-ChangeTier.ps1` is the single source of truth and is shared with CI
(`.github/workflows/build-and-test.yml`), so the two cannot drift.

| Tier | Matches | What runs |
|---|---|---|
| `Docs` | `*.md`, `Docs/**`, `.claude/plans/**` | Nothing — the pre-commit hook's fast tests already cover it |
| `Tooling` | `Scripts\Run-EloMatch.ps1`, `Run-Tests.ps1`, `Sync-Master.ps1`, `verify_mate_key.py`, `build_corpus.py`, `New-Worktree.ps1`, `Remove-Worktree.ps1`, `Get-Worktrees.ps1` | PowerShell syntax parse only — never compiled, never invoked by the engine |
| `Build` | `build.ps1`, `Scripts\Validate-*.ps1`, `New-PullRequest.ps1`, `Get-ChangeTier.ps1`, `.githooks/**`, `.github/**`, `CMakeLists.txt`, `*.cmake`, `CMakePresets.json` | Full: build + extended `[slow]` tests + tactical suite + self-play |
| `Engine` | `*.cpp`, `*.h`, `*.json`, **and anything unrecognised** | Full |

A mixed diff takes the **strictest** tier present. Two properties are deliberate and asserted by
`Get-ChangeTier.ps1 -SelfTest`: it **fails closed** (an unrecognised path gets the full run, never a
skip), and the validation machinery is itself `Build` tier — a change to `Validate-*.ps1` or to the
classifier can never take its own shortcut, since a classifier bug would otherwise be
self-concealing. `-Force` runs every gate regardless.

Background: PR #56 (a one-line `CLAUDE.md` fix) and PR #133 (a measurement-script change) both paid
a full build + extended-test + self-play cycle for a guaranteed pass — issue #124.

---

## When `search-reviewer` may be skipped

**Default is to dispatch.** Self-certification is permitted only when **every** condition below
holds; failing any one means dispatch.

Do not judge by "it's only logging". The question is whether the edit changes what the search
computes *or how fast it computes it* — a log call added inside a per-node path can cut NPS, and
under time control that means a shallower completed depth and a genuinely different move, which
neither fixed-depth tests nor a self-play PASS would catch.

1. Every changed line lies inside a `log_*` function, or is an argument expression to an existing
   `s_logger->…` / `log.…` / `spdlog::…` call.
2. **No log call is added or removed** — only the arguments of existing calls change. Adding a call
   site is the risky act, not editing one.
3. Every new argument expression is a pure read of already-computed values: no `td.`/`board.`
   mutation, no counter increment, no lazy initialisation, and no board query whose result depends
   on `DoMove`/`UndoMove` state. (`MoveFormatter::ToShort(move, board)` reads `GetPiece()`/
   `InCheck()` — never call it after a failed or unpaired `DoMove`; use the board-free `ToCoord` in
   search diagnostics.)
4. No changed line lies inside `pvs`, `quiescence`, `search_with_aspiration`, `iterative_deepening`,
   `assess_iteration_quality`, `adjustScoreForGameState`, `should_stop_early`, or
   `should_try_null_move`.
5. No numeric literal, comparison operator, or control-flow keyword changed anywhere in the file.
6. `SearchTuning` in `AIPerplex.h` is untouched. A one-character constant change there is the
   highest-Elo-density edit in the repo and the least alarming-looking diff in it — it never
   self-certifies.

When skipping, say so in the PR body: *"search-reviewer skipped: logging-only, criteria 1-6."*
That makes the skip an auditable claim rather than a silent omission.

`New-PullRequest.ps1` reminds on **any** touch to the reviewed files rather than detecting the above
automatically. The asymmetry is the point: a false positive costs one subagent dispatch, a false
negative merges an unreviewed search change that surfaces weeks later in an Elo match, if at all.
Never teach the script to suppress the reminder — escalating it is fine.

---

## Two ways to run a task

Both fork every task fresh from `origin/main`. They differ only in whether the task gets its own
directory.

| | Per-task worktree | Task branch in place |
|---|---|---|
| Start | `New-Worktree.ps1 -Name x` | `New-TaskBranch.ps1 -Name x` |
| Finish | `Remove-Worktree.ps1 -Name x -SyncMaster` | `Remove-MergedBranches.ps1 -SyncMaster` |
| Parallel tasks | Yes — park one, switch to another | No, sequential only |
| Build directory | Cold per worktree; first build needs network | Stays warm across tasks |
| Cleanup failure mode | Orphaned directories, unregistered worktrees | `git branch -D` |

Use a worktree when work must be parked half-finished or run alongside another task. Use in-place for
a run of small sequential PRs, where directory churn buys nothing and the warm build directory is
worth real minutes.

**The worktree model enforces two invariants structurally; in-place mode needs scripts to enforce
them.** This is the whole reason those two scripts exist:

| Invariant | Worktree | In-place |
|---|---|---|
| Every task forks from `origin/main` | `New-Worktree.ps1` never reads the current branch | `New-TaskBranch.ps1` does the same — a hand-typed `git checkout -b` would fork off the previous task and drag its commits into the next PR |
| Uncommitted work cannot cross tasks | Separate working trees | `New-TaskBranch.ps1` refuses on tracked modifications — `git checkout` otherwise carries a dirty tree onto the new branch |

Untracked files never count as dirty in either script: they survive a checkout unchanged, cannot
enter a commit on their own, and tool caches would otherwise block every run.

`Remove-MergedBranches.ps1` deletes only what `git merge-base --is-ancestor <branch> origin/main`
proves is contained in `origin/main` — **names are never evidence**. `git branch -d` is not a
substitute: it asks whether a branch merged into the *current* branch, a different question. It skips
`master`, `main`, the branch you are on, and anything checked out in another worktree; if the branch
you are on is itself merged it says so rather than moving your HEAD.

**`Sync-Master.ps1` runs from anywhere.** `master` can be checked out in only one worktree, so the
script finds that worktree and syncs there rather than requiring you to be standing in it. When that
is not the tree you invoked from, it refuses on uncommitted tracked changes instead of stashing — the
stash stack is shared across worktrees, so another session could pop entries it pushed. This matters
most in in-place mode, where nothing ever removes a worktree and so nothing would otherwise trigger a
sync: `master` drifts silently until someone notices it is ten commits behind.

---

## Self-play validation

- AI vs AI is fully headless: `Game::Run()` terminates on checkmate/stalemate; no stdin needed.
- **The `game` argument is required.** No argument (or `uci`) routes into `UciHandler::run()`, which
  blocks on stdin and never runs `Game::Run()`.
- Subprocess pattern: `Start-Process ..\build\windows-clang-cl\StratChessEvolved.exe -ArgumentList "game"
  -PassThru -NoNewWindow -RedirectStandardOutput out.txt`, then `$proc.Kill()` after N seconds for a
  timed test, or `$proc.WaitForExit(msTimeout)` for a game expected to finish naturally.
- Verbose logging is on by default in game mode; each move logs `GetMove complete: move=…, depth=…,
  time=…ms, nodes=…, stable=…` to stdout.
- Use `"type": 6` for both sides to exercise AIPerplex. For changes to `PlayerAI`/`PlayerBase`,
  also verify with `"type": 3` (AIAgent).
- `game_settings.json` accepts C-style `/* */` comments via nlohmann, but PowerShell's
  `ConvertFrom-Json` rejects them — write plain JSON when generating configs programmatically.

---

# Part 2 — What CI does

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

**`strength.yml`** is the CI strength lab: `workflow_dispatch` only, one job, candidate against a
reference ref with both sides built from source in that job by the same GCC. It reports Elo to the
job summary and uploads the PGN; it gates nothing and is triggered by nobody automatically.

Dispatch-only is not a stepping stone to be skipped past. A measurement harness that is wrong is
worse than none, because its output looks exactly like a measurement — so it stays manual until the
null test and the known-sign control in M4 of `.claude/plans/public-repo-and-strength-lab.md` have
both come back right, and no result is recorded before then.

| Input | Meaning |
|---|---|
| `reference_ref` | Tag, branch or SHA for the reference side. Pass the candidate's own SHA for a null test |
| `games` | Total games, two per opening pair |
| `candidate_tc` / `reference_tc` | Per-side time control. Halve the **base** for a handicap run — an increment under 0.1 s sits below `compute_budget()`'s 100 ms per-move floor and forfeits games |
| `concurrency` | Concurrent games. A standard runner has 4 vCPU and only one engine per game thinks at a time |

A batch reporting a time loss, an illegal move or a disconnect is **discarded, never reported** —
same rule as `Run-EloMatch.ps1`, and on a shared runner a time loss most likely means the box was
oversubscribed, which invalidates the whole batch rather than the one game. Numbers land in
`Docs/EloLog.md`'s **Linux ledger**, which must never be compared against the local clang-cl rows.

---

# Part 3 — The environment

## Working in Visual Studio

Open the repo as a **folder**, not a solution — VS reads `CMakePresets.json` and offers the presets
in its configuration dropdown.

Debugger arguments and working directory live in `.vs/launch.vs.json` (gitignored). Set
`"args": ["game"]` for game mode, and **`"currentDir": "${workspaceRoot}\\StratChessEvolved"`** —
`game_settings.json`, `logs/` and the `Tests/` lookup all resolve against the working directory, and
`TacticalTestRunner` takes its *parent*, so only that directory satisfies all three. VS defaults to
the executable's own folder, which satisfies none of them.

---

## Dependency cache

`FetchContent` clones spdlog, nlohmann/json and Catch2 on the first configure of each build tree, so
**a fresh worktree's first build needs network**. The committed presets place them in
`${sourceDir}/build/_deps` — per worktree, and the path CI caches.

`build.ps1` overrides that at configure time with `-D FETCHCONTENT_BASE_DIR=<repos>/StratChessDeps`,
a single cache beside the main checkout — so only the first worktree on a machine ever clones and the
rest need no network. It is skipped when `GITHUB_ACTIONS` is set, leaving CI on `build/_deps` where
its cache key expects them.

Done via `-D` rather than a preset because `CMakeUserPresets.json` cannot redefine a preset that
`CMakePresets.json` already declares — duplicate names are a hard error — and inheriting under a new
name would change `binaryDir` too, which `Get-BuildArtifact.ps1` depends on. `CMakeUserPresets.json`
is gitignored regardless: it is CMake's per-developer override file and is machine-specific by
definition.

---

## Two CMake settings that are load-bearing

- **`CMAKE_RC_COMPILER=llvm-rc`** — the Windows SDK `rc.exe` is the only tool in this chain that is
  not long-path aware, and fails with `RC1109` under a deep build path.
- **`/clang:` prefixes** on the warning and constexpr flags. clang-cl silently mistranslates the
  plain GNU spellings (`-Wall` is read as `/Wall` → `-Weverything`; `-fconstexpr-steps=` is dropped
  before reaching the frontend), and MSVC ignores them with a D9002 warning while still producing a
  binary. A clean compile does not prove a flag arrived — check `cmake --build --verbose`.

---

## Raw CMake invocation (fallback)

`build.ps1` is the documented path: it imports the VS developer environment via `vswhere` and then
drives the presets. Use these only when driving CMake directly, from a **VS Developer PowerShell**
so the compiler, `ninja` and `cmake` are on `PATH`.

```
cmake --preset windows-clang-cl            # configure (once per build tree)
cmake --build --preset windows-clang-cl    # build every target
cmake --build --preset windows-clang-cl --target StratChessTests
```

Presets: `windows-clang-cl`, `windows-clang-cl-debug`, `windows-msvc`, `windows-msvc-debug`.
clang-cl is what ships; MSVC is for development and debugging only, never for measurement.

Configuring is only needed when the build tree does not exist — Ninja re-runs CMake by itself when
`CMakeLists.txt` or `CMakePresets.json` change. Add `--verbose` to `cmake --build` to see the real
compiler command lines, which is the only reliable way to confirm a flag actually reached the
compiler rather than being silently dropped.

---

# Part 4 — When something breaks

## Worktree removal gotchas

All three are handled by `Remove-Worktree.ps1`; they still apply when doing it by hand.

- **Never remove a worktree from inside it.** git deregisters it but cannot rmdir its own cwd, so
  the leaf survives with no `.git`, and the shell's cwd gets stuck pointing at it while git commands
  silently resolve against the *outer* repo. If you hit this, use absolute paths / `git -C <path>`
  and don't trust `pwd`.
- **A locked directory is not a failure.** On Windows git often deletes every file but cannot rmdir
  the folder while a process holds it open. Deregister and carry on to the branch deletion —
  stopping there is how orphaned branches accumulate.
- **Detached worktrees keep a sibling branch.** Claude Code auto-mode worktrees are detached with a
  `claude/<dir-name>` branch parked at the same commit; removing the directory alone leaves that
  branch behind. For a worktree created via `EnterWorktree`, `ExitWorktree(action:"remove")` is
  cleanest.

`Remove-Worktree.ps1` verifies the branch is an ancestor of `origin/main` before deleting anything,
so a **squash-merged** PR is reported rather than deleted — its commits are not ancestors even
though the content landed. Confirm with `git diff origin/main <branch> --stat` (empty means safe)
and re-run with `-Force`.

`Get-Worktrees.ps1` also reports directories under `.claude\worktrees` that are absent from
`git worktree list` — the residue of a half-succeeded removal, which no other cleanup path can see.

---

## Reproducing a sanitizer finding

There is no preset and no `build.ps1` verb: `-fsanitize=` is refused on MSVC and clang-cl, so this
only ever runs on Linux. On WSL, the export-and-extract route in the CI job's image:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTRAT_SANITIZE=address,undefined
cmake --build build --target StratChessTests --parallel
./build/StratChessTests '~[slow]'
```

`STRAT_SANITIZE_RECOVER=ON` is the one deliberate exception to that invocation. It drops
`-fno-sanitize-recover=all`, so the run reports everything it finds instead of aborting on the first
hit — useful when surveying, and useless as a gate, because a recovering build exits 0 with the
findings only in the log. Pair it with `ASAN_OPTIONS=halt_on_error=0`. CI leaves it off.

Building over `/mnt/c` does not work — `FetchContent` fails with `configure_file: Operation not
permitted`, because DrvFs cannot perform the permission operations CMake asks for. Extract onto
native ext4.

---

## Runtime output files

All paths are relative to the **working directory**, not the exe location. Run the exe from
`StratChessEvolved/` so `game_settings.json` resolves and output lands in `StratChessEvolved/logs/`.

| File | Created by | Context | Notes |
|---|---|---|---|
| `logs/multisink.txt` | `Logger::InitDefault()` (`Logger.cpp`) | Game mode only; **not** in tests | trace→file, info→console |
| `logs/aiperplex.log` | `AIPerplex::SetVerboseLogging(true)` (`AIPerplex.cpp`) | Whenever AIPerplex is constructed | Level `off` (file stays empty) when verbose is disabled afterwards, which is what tests do |
| `logs/SimplePerfStats.txt` | `Logger::EnsurePerfLogger()` (`Game.cpp` only) | Game mode only — `Game::Init()` is the sole creator | Written per AI move by `StopTimerAndAdjustVars()`, which only writes if a logger already exists; no file in tests, the tactical runner or UCI mode |
| `logs/gamelist.txt` | `Game::CreateGameMoveFile()` (`Game.cpp`) | Game mode only | One line per move via `MoveFormatter::ToShort` |

All four are gitignored. `logs/` does **not** need to pre-exist — spdlog's `file_helper::open` calls
`os::create_dir()` on the parent path. spdlog *does* swallow a genuine `basic_file_sink` constructor
failure silently, so a permissions problem produces no file and no error message.
