# CLAUDE.md – StratChessEvolved

## Project Overview
A modern C++20 chess engine focused on improving playing strength (ELO) while maintaining clarity, efficiency, and robustness.

## Repository Structure
- `StratChessEvolved/` – Main application entry point and project files
- `StratEngine/` – Core engine source (search, evaluation, move generation, AI agents)
- `StratEngine/Utils/` – Cross-cutting utilities: `TimeManager` (soft/hard limits), `TimeUtils` (budget formula), `Logger`
- `StratEngine/Archived/` – Legacy algorithms kept for reference (AITrans, ABIterTrans, HashElement)
- `StratEngine/Tests/` – Perft and repetition test implementations
- `StratChessTests/` – Unit test project
- `Docs/` – Roadmap and design documents
- `StratChessEvolved.sln` – Visual Studio solution file

## Build
- Visual Studio solution (`.sln`) targeting C++20
- Supports both Debug and Release builds; prefer Release for performance benchmarking, especially for deeper Perft tests and self-play validation
- `Directory.Build.props` at repo root defines `$(DepsRoot)` for spdlog/nlohmann includes;
  copy `Directory.Build.user.props.example` → `Directory.Build.user.props` if your dependency
  layout differs from the default (sibling directories next to the repo)
- Adding files to Solution Explorer: headers use `ClInclude`, non-code files (`.md`, `.json`) use `None` — in both cases also add a matching `<Filter>` entry in `.vcxproj.filters`. Forgetting the filter entry leaves the file visible but unfiled (no folder).
- **Only `x64` builds work** — the x86/Win32 configuration is not maintained for C++20
- `StratEngine/StdAfx.h` is the PCH — 14 STL headers covered (alphabetical order within the `#pragma warning push/pop` block). Add new frequently-used headers there, not in individual `.cpp` files. Note: measuring PCH benefit via wall-clock is misleading (LTCG dominates); use `/Bt` or `/d1reportTime` for per-TU parse timing.

### Build script (preferred)
`build.ps1` at the repo root wraps MSBuild and discovers the correct executable automatically via `vswhere.exe` — no hard-coded VS paths:

**First commit in a fresh worktree**: the pre-commit hook rebuilds from scratch (no incremental cache) — pass a longer explicit timeout (5-10 min) to whatever runs `git commit`, not the 2-minute default, or it gets killed mid-build with the commit never happening.
```powershell
.\build.ps1               # build main + tests in parallel (Release|x64)
.\build.ps1 main          # main solution only
.\build.ps1 tests         # test project only
.\build.ps1 run-tests     # build tests then run fast tier only (~[slow])
.\build.ps1 extended-tests           # build tests then run ALL tiers including [slow]
.\build.ps1 run-tests "[formatter]"  # build tests then run a single tag
.\build.ps1 all                      # build main + tests in parallel (Release|x64)
.\build.ps1 all -Config Debug        # parallel debug build of both
```

### Validation scripts
Scripts in `StratChessEvolved/Scripts/` handle working-directory and MSBuild path resolution
internally. Invoke via the canonical pattern from any shell (bash, cmd, PowerShell):
```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\<name>.ps1"
```

| Script | When to use |
|---|---|
| `Scripts\Run-Tests.ps1 [tag]` | Any test verification — optional tag filter |
| `Scripts\Validate-PreCommit.ps1` | Before every commit — FEN check + fast tests |
| `Scripts\Validate-PrePR.ps1` | Before opening a PR — full build + extended tests + self-play |
| `Scripts\Run-EloMatch.ps1 [-Smoke]` | After search/eval/time-management changes — measure strength vs pinned reference build (see `Docs/EloLog.md`); ≈1 h unattended per 500-game batch. **Use `-Sprt NonRegression` / `-Sprt Gain` for anything expected to be worth less than ~25 Elo** — a fixed 500-game batch cannot resolve it, and recording the resulting "±26" row as a measurement is how false confidence accumulates |

Scripts must be invoked with `-File`, not dot-sourced (`$PSScriptRoot` is `$null` under dot-source).

### CI
`.github/workflows/build-and-test.yml` runs an independent build + fast-test check on every PR
into `main` and every push to `main` (the latter both validates post-merge and warms the deps
cache on the default branch, which `actions/cache` scopes per-branch — a PR from a differently
named branch can't restore a cache only ever saved under another branch), on the
`windows-2025-vs2026` runner image (pinned explicitly, not `windows-latest`, because this
project's `PlatformToolset` is `v145` — see `.claude/plans/full-build-test-ci-github-actions.md`
for the verification). It fetches spdlog/nlohmann-json/Catch2 at the same pinned versions used
locally via a generated `Directory.Build.user.props`, then runs `build.ps1 tests` (not `all`) —
CI never runs `StratChessEvolved.exe`, and the main app project has LTCG (`WholeProgramOptimization`)
enabled for `Release|x64` while the test project deliberately doesn't; building `all` wastes most
of the wall time linking an unused, LTCG'd binary. Extended `[slow]` tests and self-play remain
local-only (`Validate-PrePR.ps1`) — self-play's timeout-based nondeterminism isn't worth the CI
flakiness.

### Git hooks
`.githooks/pre-commit` (tracked, runs `Validate-PreCommit.ps1`) is the actual hook — `git`'s
default `.git/hooks/` is untracked and not used. `build.ps1` sets `core.hooksPath` to `.githooks`
automatically on first run, so any clone or worktree gets the hook the first time someone builds;
no manual setup step needed. If you skip `build.ps1` entirely, run `git config core.hooksPath .githooks` once yourself.

### MSBuild invocation (fallback / raw)
When calling MSBuild directly from a **Task (Bash subagent)** using `cmd.exe /c "…"`, use Windows backslash paths and single-slash flags:
```
cmd.exe /c "\"<MSBuild>\" \"StratChessEvolved.sln\" /p:Configuration=Release /p:Platform=x64 /m /v:minimal"
```
To discover `<MSBuild>` dynamically:
```
cmd.exe /c "\"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe\" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe"
```
**Shell notes**: The MSBuild install folder changes with every VS release (`2022`, `18`, …); never hard-code it. In **Git Bash** use `//p:` flags (double-slash) and the `/c/Program Files/…` path form. Direct `Bash` tool invocations are unreliable for Windows paths — prefer the Task (Bash subagent) + `cmd.exe` pattern.

**PowerShell from Bash tool**: The Bash tool runs Git Bash (bash), not PS7. PS7 syntax (`$var`, backtick escapes, piped cmdlets like `Where-Object`/`Select-String`/`Sort-Object`, and multi-line strings) all fail silently when inlined in bash. Rule: write any non-trivial PS logic to a `.ps1` file first, then invoke with `pwsh -ExecutionPolicy Bypass -File .\script.ps1`.

**Editing existing `.ps1` files**: multi-line `sed`/bash substitutions reliably mangle backslashes (Windows paths, PS line-continuation backticks) — write a small Python script to a temp file and run it (`python3 script.py`, not inline `-c`) for precise text replacement instead. Validate syntax without executing via `[System.Management.Automation.Language.Parser]::ParseInput($content, [ref]$tokens, [ref]$errors)`.

Use `/v:normal` instead of `/v:minimal` when diagnosing build errors.

## Engine Algorithm Summary
IDS + PVS + quiescence search; Zobrist-hashed transposition table; bitboard representation; killer moves (2/ply) + history heuristic; threefold/twofold repetition detection. Active priorities/backlog live in GitHub Issues (see `Docs/Roadmap.md` for the label taxonomy); `Docs/Changelog.md` has the implementation history.

## Key Source Files
- `Move::Output()` produces pseudo-LAN (`Pe2-e3`): piece prefix (uppercase=White, lowercase=Black) + from + `-` + to — not short algebraic notation
- `Move` is a pure 2-byte value (from/to/flags only); moving piece and captured piece are NOT stored — retrieve via `Board::GetEffectiveMovPiece(m)` (pre-move only) and `Board::GetCapturedPiece(m)`. After `DoMove`, use `board.GetPiece(m.to())` to identify the moved piece.
- `MoveHelper::Value(move, movPiece, content)` — material scoring for move ordering (not `Move::Value`). `IsMoveType`/`IsPawnMove`/`IsKingMove` take only an `ePiece` — no `Move&` parameter.
- `StratEngine/Board.cpp` / `Board.h` – Board state and move application
- `StratEngine/MoveGenerator.cpp` / `MoveGenerator.h` – Legal move generation
- `StratEngine/Eval.cpp` / `Eval.h` – Position evaluation
- `StratEngine/AIPerplex.cpp` / `AIPerplex.h` – Primary AI agent; `AIPerplex.h` holds `SearchTuning` and internal structs. Null-move pruning gated by `tuning_.null_move_enabled` (default `true`) via `should_try_null_move()` — covers zugzwang, mate-score contamination, consecutive-null, PV/in-check, min-depth
- `StratEngine/ThreadData.h` – Per-thread search state (thread-local `Board` copy, node counter, `PVTable`, `GameInfo` sequence, killers/history/null-flags + their maintenance methods). `ThreadData&` is always the FIRST parameter of every search method; the search runs on `td.board`, never on the game board (root game state is propagated back in `GetMove()`). The TT stays a separate parameter, shared across threads — Lazy SMP helper threads (`AIPerplex::helper_tds_`, spawned per-search via `SetThreads(N)`/UCI `Threads`/`game_settings.json` `"threads"`) each get their own `ThreadData`
- `StratEngine/TranspositionTable.cpp` / `TranspositionTable.h` – TT implementation
- `StratEngine/MoveHelper.h` – Move query utilities (`IsCapture`, `IsPawnMove`, `IsKingMove`, `Value`, etc.) — all take `ePiece`, no `Move&`
- `StratEngine/Sort.cpp` / `Sort.h` – Move ordering
- `StratEngine/Utils/TimeUtils.h/cpp` – `Engine::compute_budget(remaining, increment, moves_to_go)` → `TimeBudget{soft, hard}`; pure function, no clock dependency
- `StratEngine/Utils/TimeManager.h` – `chess::TimeManager`: `start(soft, hard)` / `start(allocated)` (delegates); `should_stop_iteration()` (soft) + `should_stop_search()` (hard)
- `StratEngine/SearchLimits.h/cpp` – `SearchLimits` (clock/movetime/depth/infinite, all optional) carries every per-call search constraint; `Engine::resolve_limits()` is the pure resolver (mirrors `compute_budget`'s pattern); `PlayerAiBase::ApplyLimits()` arms the timer from the resolved budget and returns the effective depth. Every `GetMove(info, limits)` call is self-contained — no more pre-call `SetClockInfo()`/`SetMaxDepth()` ordering contract (that method and its `clock_info_set_` flag were deleted in PR #80)
- `StratChessEvolved/game_settings.json` – Runtime player/AI configuration; per-player `"search_limits"` block (legacy `max_depth`/`time_limit` keys still work via a deprecation-warning fallback)
- `Docs/Roadmap.md` – General engineering principles only; active backlog lives in GitHub Issues (see its Overview for the label taxonomy)
- `Docs/Changelog.md` – Historical record of completed work, dated entries (Keep a Changelog-style)
- `Docs/TestDesign.md` – Test coverage map and phase plan; check before adding tests

## Development Guidelines
- Language: C++20; favor `constexpr`, RAII, move semantics, strong types
- **Compiler: Level4 + `/WX` enforced** on `x64` Debug and Release (both projects) — any new warning is a build error. Approved suppressions: `[[maybe_unused]]` for params only used in `assert()` macros; `static_cast<>` for intentional narrowing. Never use `#pragma warning(disable)` in source files.
- Naming and comments must be in English and unambiguous
- Approved external dependencies only: `spdlog` (logging), `nlohmann/json` (config/serialization)
- All changes must be thread-safe, especially around transposition tables
- No regressions in search accuracy or ELO without explicit justification
- Benchmark before and after any optimization

## Testing & Validation

### Unit tests (Catch2 v3)
The test binary lives under `StratChessTests/x64/Release/` (not `x64/Release/`). Run from any directory:
```bash
StratChessTests/x64/Release/StratChessTests.exe                    # all tests
StratChessTests/x64/Release/StratChessTests.exe [repetition]       # repetition detection
StratChessTests/x64/Release/StratChessTests.exe [perft]            # perft depth 1-4
StratChessTests/x64/Release/StratChessTests.exe [tt]               # TranspositionTable unit tests
StratChessTests/x64/Release/StratChessTests.exe [eval]             # evaluation position tests
StratChessTests/x64/Release/StratChessTests.exe [tactical]         # search regression tests (fast, depth 4)
StratChessTests/x64/Release/StratChessTests.exe [tactical_full]   # slow tactical tier (depth 6, ~2 s)
StratChessTests/x64/Release/StratChessTests.exe [sort]             # move ordering priority tests
StratChessTests/x64/Release/StratChessTests.exe [time_mgr]         # compute_budget formula + soft/hard timing
```
`run-tests` excludes `[slow]` tests by default; `extended-tests` includes them.
Building the `.sln` does not always rebuild the test project. To build it explicitly:
```powershell
.\build.ps1 tests
```
The test project uses `/Z7` debug format (debug info embedded in `.obj` files) so parallel `//m` compilation works without PDB write contention.
Sources: `StratChessTests/*.cpp` — Framework: Catch2 v3 amalgamated — `$(DepsRoot)Catch2/` (sibling of repo)

### Tactical test positions
`StratChessTests/TacticalTestHelpers.h` — shared `TacticalCase` struct + `make_tactical_engine()` factory used by all tactical test files. Use `GENERATE(from_range(kCases))` pattern (see `TacticalTests.cpp`).

When constructing FEN positions for tactical tests:
- **Always append ` w - - 0 1`** — omitting the side-to-move field silently makes the engine play as Black (bug #46)
- **Verify no White piece attacks the Black King** before adding a position — engine silently accepts illegal FENs and returns king-capture moves (bug #45)
- **Verify uniqueness against the engine**, not manually: `(printf "uci\nisready\nposition fen FEN w - - 0 1\ngo depth N\n" | ./x64/Release/StratChessEvolved.exe 2>/dev/null | grep "^bestmove")`
- Use parentheses `(cmd; cmd) | pipe` for multi-command UCI pipes in Git Bash — braces `{ }` fail

### AIPerplex in tests
Constructor re-enables verbose logging and leaves `Eval` null; follow this setup order:
```cpp
Board board(fen);                                // must be constructed before Create()
auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth, board);
AIPerplex::SetVerboseLogging(false);             // must be AFTER Create()
ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX); // must be before GetMove()
GameInfo info = board.GetGameInfo();
Move m = ai->GetMove(info);
```

### Deep perft tests
**Must be run from the `Tests/` directory** — the executable looks for `perft_test_cases.json` in the working directory:
```bash
cd Tests
../x64/Release/StratChessEvolved.exe perft test
```
Sources: `StratEngine/Tests/Perft.h/cpp` + `Tests/perft_test_cases.json`

### Self-play validation
- Run exe from `StratChessEvolved/` directory — reads `game_settings.json` from working directory
- AI vs AI is fully headless: `Game::Run()` terminates automatically on checkmate/stalemate; no stdin needed
- AIPerplex verbose logging is **on** by default in game mode; each move logs `GetMove complete: move=..., depth=..., time=...ms, nodes=..., stable=...` to stdout; default spdlog also writes `logs/multisink.txt` (relative to working dir)
- For subprocess validation: `Start-Process ..\x64\Release\StratChessEvolved.exe -ArgumentList "game" -PassThru -NoNewWindow -RedirectStandardOutput out.txt` then `$proc.Kill()` after N seconds for timed tests; `$proc.WaitForExit(msTimeout)` for games expected to complete naturally — **the `game` argument is required**: no args (or `uci`) routes into `UciHandler::run()` (`StratChessEvolved.cpp` `main()`), which blocks on stdin and never runs `Game::Run()`
- **Claude is expected to execute all validation plan steps autonomously** — explicitly flag any step that requires user assistance (e.g. interactive GUI, manual input) before skipping it
- Run AIPerplex self-play (`"type": 6` for both sides in `game_settings.json`) to verify search behaviour
- For changes to base classes PlayerAI/PlayerBase, verify through AIAgent self-play (`"type": 3`) as well
- `game_settings.json` uses C-style `/* */` comments (handled by nlohmann); when writing test configs programmatically use plain JSON strings (PowerShell `ConvertFrom-Json` rejects comments)

### Log and output files

Files written to disk at runtime; all paths are relative to the **working directory** (not the exe location):

| File | Created by | Build/runtime context | Level/gate | Notes |
|---|---|---|---|---|
| `logs/multisink.txt` | `Engine::Logger::InitDefault()` (`Logger.cpp`) | Game mode only; **not** in tests | Always — trace→file, info→console | All four files require `logs/` to pre-exist; spdlog silently swallows failures |
| `logs/aiperplex.log` | `AIPerplex::SetVerboseLogging(true)` → `ensure_logger_initialized()` (`AIPerplex.cpp`) | Created whenever AIPerplex is constructed; level=off (file stays empty) when `SetVerboseLogging(false)` is called afterward | debug level on `s_logger`; silenced via `spdlog::level::off` when verbose is disabled | gitignored; in tests the file is created but empty because ctor enables then test disables |
| `logs/SimplePerfStats.txt` | `Engine::Logger::EnsurePerfLogger()` (`Game.cpp`, `PlayerAI.cpp`) | Game mode only — created eagerly in `Game::Init()` | Always in game mode; written per AI move by `StopTimerAndAdjustVars()` | gitignored |
| `logs/gamelist.txt` | `Game::CreateGameMoveFile()` (`Game.cpp`) | Game mode only — created eagerly in `Game::Init()` | Always in game mode; one line per move via `MoveFormatter::ToShort` | gitignored |

All four files are gitignored and all land under `logs/`. The `logs/` subdirectory must pre-exist — spdlog silently swallows the `basic_file_sink` constructor failure, so missing the directory means no file output (no error message). Deprecated file `legalmoves.txt` (pre-spdlog global `std::ofstream`) was removed — board and root-move diagnostics now flow through the default spdlog logger at `debug` level.

**Working directory**: must run exe from `StratChessEvolved/` — both for `game_settings.json` resolution and so all log output lands in `StratChessEvolved/logs/`.

### General
- Maintain deterministic behavior for reproducibility
- `game_settings.json`: verify FEN is set to the starting position before committing — test sessions often leave a custom FEN in place

## Design Documents
For any non-trivial multi-file task, create `.claude/plans/<kebab-name>.md` before starting implementation. Include:
- **Goal** — what the task achieves and its scope limits
- **Design Decisions** — key choices and the reasoning behind them
- **Files Changed** — complete list
- **Step-by-Step Changes** — detailed enough to resume mid-task or review later
- **Validation Plan** — build command, test tags, self-play check
- **Key Correctness Properties** — invariants that must hold after the change

Commit the plan file — it lives under `.claude/plans/` (tracked by git) and serves as permanent reference even after the worktree is retired. **Name it after the content**, not an auto-generated string — e.g. `logging-spdlog-gate-and-outlegalmoves-removal.md`, not `ticklish-rolling-ripple.md`.

## Subagent Dispatch Guidelines
- **Do exploratory/verification work in the controller session first**, not inside the implementer subagent. Open-ended tasks (e.g. verifying 25 engine positions) should be completed by the controller and the results handed to the implementer as a closed list — not delegated as an exploration task.
- **Always provide the explicit worktree-relative binary path** in implementer prompts. Never use `..` relative paths pointing outside the worktree — the main repo's stale binary may silently satisfy the path check while producing wrong results. Correct pattern: `StratChessTests/x64/Release/StratChessEvolved.exe` from the worktree root, or build first with `.\build.ps1 main` and pass the explicit absolute path.
- **NEEDS_CONTEXT safety valves only catch "file not found"** — they don't detect a stale or wrong binary. If verification relies on binary output, ensure the binary was built from current worktree sources before the subagent is dispatched.
- **Background Bash tasks piping to long-running processes on Windows are unreliable** — they may be killed mid-stream. Use foreground calls for anything that pipes stdin to the engine and reads stdout.
- **A subagent's own long background wait (self-play, an ELO match) doesn't reliably auto-resume its turn when the wait completes.** Don't just wait for a notification — proactively check in (file/log timestamps, process list, or a direct status-check message) every 15-20 min on anything long-running.

## Commit & PR Conventions
- Development on shared work happens via per-task worktrees forked fresh from `origin/main`; PRs target `main`
- Local `master` in the main checkout is a personal/scratch branch, independent of any PR workflow — safe to commit to directly, safe to let drift. Never fork a worktree from it, and never fall back to it mid-session if a worktree's branch is already gone (fork a fresh worktree from `origin/main` instead). Run `Scripts\Sync-Master.ps1` any time it needs to catch up with `origin/main` — fast-forwards when possible, merges (preserving local-only commits) when master has diverged, never pushes anywhere. `origin/master` was retired: a leftover from an incomplete master→main rename, never actually relied on as a backup — nothing should reference it going forward.
- Keep PRs small and logically scoped
- Include motivation, design reasoning, and expected impact for non-trivial changes
- Claude worktrees live under `.claude/worktrees/` — build from a worktree works out of the box via `Directory.Build.props`
- **Worktree PR workflow**: if the worktree branch contains commits unrelated to the current task (e.g. personal WIP from `master` that bled into the worktree), do NOT PR the branch directly — cherry-pick only the relevant commit(s) onto a fresh branch from `origin/main`. If all commits in the worktree are intentional, reviewed, and part of the same plan, PR the branch directly.

### Resuming work in an existing worktree
Before continuing work in a worktree that's been idle, `git fetch origin main` and check `git log HEAD..origin/main --oneline`. A worktree can silently drift behind `main` indefinitely; catching divergence early avoids discovering a conflict for the first time at PR review (see PR #57, where NMP landed on `main` mid-session and collided with `Board.h`).

### Pre-PR checklist
Before running `gh pr create` (or pushing an update to an already-open PR):
1. **Sync** — `git fetch origin main`, then `git merge origin/main`. Resolve any conflicts before proceeding (keep both sides' additions when the conflict is just "two PRs added unrelated declarations at the same anchor line" — don't drop either). This step is cheap and applies to every PR, docs included: a doc-only change can still collide with another doc-only change.
2. **Validate — scoped to what actually changed**, via `git diff --name-only origin/main...HEAD`:
   - **Doc-only diff** (every changed file matches `*.md`, `Docs/**`, `.claude/plans/**`, or `CLAUDE.md` — no `.cpp`/`.h`/`.json`/`.ps1`/`.vcxproj*`/source files touched): the pre-commit hook's fast-test pass is sufficient. Do **not** run `Validate-PrePR.ps1` — a full build + extended-test + self-play cycle cannot catch anything a documentation edit could break, and running it anyway just burns minutes for a guaranteed pass (see PR #56, a one-line `CLAUDE.md` fix).
   - **Any code/script/config change**: run `Scripts\Validate-PrePR.ps1` (full build + extended `[slow]` tests + self-play) before opening the PR.
3. **Dispatch a specialized reviewer if the diff touches their domain** (check via `git diff --name-only origin/main...HEAD`):
   - `Eval.cpp` changed → dispatch the `eval-reviewer` subagent
   - `AIPerplex.cpp`/`AIPerplex.h` search internals (`pvs()`, `qsearch()`, move ordering, pruning conditions) changed → dispatch the `search-reviewer` subagent
   - Address findings before opening the PR
4. Only after steps 1-3 pass, create or update the PR.

### After a PR merges
Delete the remote branch, remove the local worktree, and delete the local branch — or invoke the `commit-commands:clean_gone` skill, which handles all three at once. Don't leave merged worktrees lying around as the default; treat cleanup as part of finishing the task, not a separate optional step the user has to ask for. If continuing directly in the main checkout afterward (not a new worktree), run `Scripts\Sync-Master.ps1` first so `master` reflects the merge.

**Worktree removal gotcha**: for a worktree created via `EnterWorktree` this session, prefer `ExitWorktree(action:"remove")` over manual git commands — clean, no leftovers. Manual `git worktree remove --force` run *from inside* the worktree being removed will deregister it from git but fail to delete the directory (can't rmdir its own cwd) — the leaf survives with no `.git`, and the shell's cwd can get stuck pointing at it while git commands silently resolve against the *outer* repo instead. If you hit this, use absolute paths / `git -C <path>` for everything until the stray directory is cleaned up, and don't trust `pwd` output.
