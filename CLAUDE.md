# CLAUDE.md – StratChessEvolved

## Project Overview
A modern C++20 chess engine focused on improving playing strength (ELO) while maintaining clarity, efficiency, and robustness.

## Repository Structure
- `StratChessEvolved/` – Main application entry point and project files
- `StratEngine/` – Core engine source (search, evaluation, move generation, AI agents)
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
```powershell
.\build.ps1               # build main solution + test project (Release|x64)
.\build.ps1 main          # main solution only
.\build.ps1 tests         # test project only
.\build.ps1 run-tests     # build tests then run all of them
.\build.ps1 run-tests "[formatter]"  # build tests then run a single tag
.\build.ps1 all -Config Debug        # debug build of both
```

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

**PowerShell from Bash tool**: Variable expansion (`$var`), backtick escapes, piped cmdlets (`Where-Object`, `Select-String`, `Sort-Object`), and multi-line strings all silently fail or corrupt when PowerShell code is inlined in the Bash tool. Rule: write any non-trivial PS logic to a `.ps1` file first, then invoke with `powershell -ExecutionPolicy Bypass -File .\script.ps1`.

Use `/v:normal` instead of `/v:minimal` when diagnosing build errors.

## Engine Algorithm Summary
IDS + PVS + quiescence search; Zobrist-hashed transposition table; bitboard representation; killer moves (2/ply) + history heuristic; threefold/twofold repetition detection. See `Docs/Roadmap.md` for current priorities and planned enhancements.

## Key Source Files
- `Move::Output()` produces pseudo-LAN (`Pe2-e3`): piece prefix (uppercase=White, lowercase=Black) + from + `-` + to — not short algebraic notation
- `Move` is a pure 2-byte value (from/to/flags only); moving piece and captured piece are NOT stored — retrieve via `Board::GetEffectiveMovPiece(m)` (pre-move only) and `Board::GetCapturedPiece(m)`. After `DoMove`, use `board.GetPiece(m.to())` to identify the moved piece.
- `MoveHelper::Value(move, movPiece, content)` — material scoring for move ordering (not `Move::Value`). `IsMoveType`/`IsPawnMove`/`IsKingMove` take only an `ePiece` — no `Move&` parameter.
- `StratEngine/Board.cpp` / `Board.h` – Board state and move application
- `StratEngine/MoveGenerator.cpp` / `MoveGenerator.h` – Legal move generation
- `StratEngine/Eval.cpp` / `Eval.h` – Position evaluation
- `StratEngine/AIPerplex.cpp` / `AIPerplex.h` – Primary AI agent; `AIPerplex.h` holds `SearchTuning` and internal structs
- `StratEngine/TranspositionTable.cpp` / `TranspositionTable.h` – TT implementation
- `StratEngine/MoveHelper.h` – Move query utilities (`IsCapture`, `IsPawnMove`, `IsKingMove`, `Value`, etc.) — all take `ePiece`, no `Move&`
- `StratEngine/Sort.cpp` / `Sort.h` – Move ordering
- `StratChessEvolved/game_settings.json` – Runtime player/AI configuration
- `Docs/Roadmap.md` – Living development plan; check before starting any new work
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
StratChessTests/x64/Release/StratChessTests.exe [tactical]         # search regression tests
```
Building the `.sln` does not always rebuild the test project. To build it explicitly:
```powershell
.\build.ps1 tests
```
The test project uses `/Z7` debug format (debug info embedded in `.obj` files) so parallel `//m` compilation works without PDB write contention.
Sources: `StratChessTests/*.cpp` — Framework: Catch2 v3 amalgamated — `$(DepsRoot)Catch2/` (sibling of repo)

### AIPerplex in tests
Constructor re-enables verbose logging and leaves `Eval` null; follow this setup order:
```cpp
auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth);
AIPerplex::SetVerboseLogging(false);             // must be AFTER Create()
ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX); // must be before GetMove()
Board::Instance().SetupFromFEN(fen);             // must be before GetGameInfo()
GameInfo info = Board::Instance().GetGameInfo();
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
- For subprocess validation: `Start-Process ..\x64\Release\StratChessEvolved.exe -PassThru -NoNewWindow -RedirectStandardOutput out.txt` then `$proc.Kill()` after N seconds for timed tests; `$proc.WaitForExit(msTimeout)` for games expected to complete naturally
- **Claude is expected to execute all validation plan steps autonomously** — explicitly flag any step that requires user assistance (e.g. interactive GUI, manual input) before skipping it
- Run AIPerplex self-play (`"type": 6` for both sides in `game_settings.json`) to verify search behaviour
- For changes to base classes PlayerAI/PlayerBase, verify through AIAgent self-play (`"type": 3`) as well
- `game_settings.json` uses C-style `/* */` comments (handled by nlohmann); when writing test configs programmatically use plain JSON strings (PowerShell `ConvertFrom-Json` rejects comments)

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

Commit the plan file — it lives under `.claude/plans/` (tracked by git) and serves as permanent reference even after the worktree is retired.

## Commit & PR Conventions
- Development happens on `master`; PRs target `main`
- Keep PRs small and logically scoped
- Include motivation, design reasoning, and expected impact for non-trivial changes
- Seek review for changes affecting evaluation, move ordering, or search algorithms
- Claude worktrees live under `.claude/worktrees/` — build from a worktree works out of the box via `Directory.Build.props`
