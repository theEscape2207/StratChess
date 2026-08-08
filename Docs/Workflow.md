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
| know whether I may spend 1% of nps | [Speed and nps](#speed-and-nps) |
| judge whether a hardening or mitigation is worth it | [Threat model](#threat-model) |
| know why Linux and Windows validate different things | [What validates what](#what-validates-what) |
| know what CI runs, and when | [`CI.md`](CI.md) |
| set up Visual Studio | [Working in Visual Studio](#working-in-visual-studio) |
| understand a first-build or network failure | [Dependency cache](#dependency-cache) |
| drive CMake directly | [Raw CMake invocation](#raw-cmake-invocation-fallback) |
| clean up a worktree that will not go away | [Worktree removal gotchas](#worktree-removal-gotchas) |
| reproduce an ASan/UBSan finding | [Reproducing a sanitizer finding](#reproducing-a-sanitizer-finding) |
| find out where a log file came from | [Runtime output files](#runtime-output-files) |

Four things are non-negotiable and are the reason most of this file exists:

- **Every task forks fresh from `origin/main`.** Never from `master`, never from the previous task.
- **A batch reporting a time loss, illegal move or disconnect is discarded, never reported.**
- **Never measure an MSVC build against a clang-cl one.** The compiler gap alone is worth tens of Elo.
- **The goal is measured positive Elo.** Speed serves that; it is not the objective in itself.

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

# Part 2 — Standing decisions

Direction rather than mechanics. These exist because each was decided once, deliberately, and would
otherwise drift back by accident — a platform added "for completeness", a percent of speed spent
without anyone noticing, a mitigation adopted because it sounded prudent. Mechanics for CI are in
[`CI.md`](CI.md).

## What validates what

**Linux Debug plus the sanitizers is the primary correctness gate.** ASan, UBSan and
`_GLIBCXX_DEBUG` — plus TSan (#184) and MSan (#189) as they land — catch strictly more, and more
precisely, than any Windows-side checking option. Cost is not a consideration: the repository is
public, so standard-runner minutes are free.

**Do not add a Windows Debug configuration to any gate.** Its unique checks are subsumed. MSVC's
`/RTC1` finds uninitialised locals and stack-frame corruption; ASan finds the latter better and MSan
the former far better. `windows-msvc-debug` exists for interactive debugging (Edit and Continue),
not for validation.

**Windows CI is not redundant, and does not become redundant when TSan and MSan land.** It covers a
different axis — *toolchain*, not bug class — and Linux cannot cover it by construction:

- clang-cl silently mistranslates or drops flags. Two of three spellings failed silently in #84, and
  `/RTC1` is a live example today: clang-cl accepts it, emits no diagnostic even under
  `-Wunused-command-line-argument`, and produces byte-identical object files. MSVC honours it
  (+53% object size on the same source).
- The MSVC standard library, the `_MSC_VER`/`_WIN32` sites, and the lld-link ThinLTO link of the
  binary that actually ships exist nowhere else.

So: Linux answers "is the code correct?", Windows answers "does the shipping toolchain build it?".

**The perftcheck corpus stays a local instrument and is not a CI leg (#196).** The sweep of all
142,953 positions found **zero disagreements on legally reachable input** (#198), so as a gate it
would be a regression tripwire rather than a discovery tool — and that role is already filled by
`perft test` in the Build tier and the depth-7/depth-6 perft legs in `nightly.yml`. Against that it
costs ~25 minutes and an 84 MB binary to fetch or cache per run. Run it from
`Scripts\Run-PerftCheck.ps1` after `MoveGenerator`, make/unmake or FEN-parser work, and when a
periodic exhaustive statement is wanted. What would change this: a defect that the committed suite
misses and the corpus catches, which would make the breadth worth paying for continuously.

## Speed and nps

**The goal is measured positive Elo, not nps.** Speed is a means; it is not the objective, and
trading a little of it for more strength is a good deal when the trade is measured.

**Use the right instrument for the size of the effect.** From this project's own data — the clang-cl
migration measured +23.32% nps → +40.28 Elo at 10+0.1 — roughly **1% nps ≈ 1.7 Elo**. That is
*below* the ±4 Elo the 20,000-game strength lab resolves, so:

| Effect size | Instrument |
|---|---|
| Under ~5% nps | `Run-Bench.ps1`. An Elo match cannot resolve it at any affordable game count |
| A strength change | `Run-EloMatch.ps1` locally, or the CI strength lab |

Trying to Elo-measure a 1% speed change is not diligence; it is a match that cannot answer the
question asked of it.

**Anything that adds per-node work gets a `Run-Bench.ps1` pass** — evaluation terms as much as
compiler flags. Eval terms are where nps actually goes; scrutinising a 1% flag while adding terms
unmeasured is the wrong emphasis. Take repeat runs: a single pass has already produced a 12% outlier
on this hardware, and per-config spread is normally 0.27-1.11%.

**A measured slowdown needs a stated benefit that outweighs it**, in correctness, robustness,
maintainability or clarity — written down in the PR, not assumed. A slowdown with no such statement
is a regression regardless of how small.

One caveat worth carrying: the +40.28 Elo result implies ~133 Elo per doubling against a textbook
~60, and the recorded explanation is the time control — speed is amplified at 10+0.1, where the
engine is often one iteration short. The 1.7 Elo/1% figure is therefore an upper bound tied to how
strength is measured here, not a universal constant.

## Threat model

**This engine is not network-facing and crosses no privilege boundary.** Input arrives from a chess
GUI the user launched, from `fastchess`, or from files in this repository. There is no attacker.

The goal for external input is therefore **robustness, not security**: malformed input produces a
clear diagnostic and a clean exit rather than a crash, a fail-fast, or a silently wrong answer.
Exploit mitigation is a cheap secondary at best.

Consequences, so this is not re-litigated each time:

- Hardening work is judged on whether it turns undefined behaviour into a diagnosable error. #178 is
  the worked example.
- **Exploit mitigations need a reason beyond "it sounds prudent."** `/GS`, ASLR and DEP are on by
  linker and compiler default, cost nothing to keep, and stay. Control Flow Guard was declined
  (#218): it defends against an attacker this project does not have, and would cost nps that buys
  Elo. A mitigation that is free is still not automatically worth adopting.
- Anything that *would* change this model — accepting input over a network, running untrusted
  engines in-process — reopens the question rather than being covered by it.

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

