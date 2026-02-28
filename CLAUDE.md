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
- Adding a header to `.vcxproj` (`ClInclude`) makes it build-visible; also add a matching `<Filter>` entry in the `.vcxproj.filters` file to place it in the correct Solution Explorer folder
- **Only `x64` builds work** — the x86/Win32 configuration is not maintained for C++20

### MSBuild invocation (from bash / Git Bash)
Use double-slash flags (`//p:`) — bash converts single `/` to a path separator and breaks MSBuild switches:
```bash
"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" \
  "StratChessEvolved.sln" //p:Configuration=Release //p:Platform=x64 //m //v:minimal
```
The `//m` flag enables parallel builds. Use `//v:minimal` to suppress noise; change to `//v:normal` when diagnosing build errors.

## Engine Features
- Iterative Deepening Search (IDS)
- Principal Variation Search (PVS)
- Quiescence Search with delta pruning
- Transposition Tables (Zobrist hashing)
- Bitboard-based board representation
- Killer move heuristic (2 killers per ply)
- History heuristic (indexed by side, from-square, to-square)
- Threefold / twofold repetition detection (Zobrist hashing)

## Key Source Files
- `Move::Output()` produces pseudo-LAN (`Pe2-e3`): piece prefix (uppercase=White, lowercase=Black) + from + `-` + to — not short algebraic notation
- `StratEngine/Board.cpp` / `Board.h` – Board state and move application
- `StratEngine/MoveGenerator.cpp` / `MoveGenerator.h` – Legal move generation
- `StratEngine/Eval.cpp` / `Eval.h` – Position evaluation
- `StratEngine/AIPerplex.cpp` / `AIPerplex.h` – Primary AI agent; `AIPerplex.h` holds `SearchTuning` and internal structs
- `StratEngine/TranspositionTable.cpp` / `TranspositionTable.h` – TT implementation
- `StratEngine/Sort.cpp` / `Sort.h` – Move ordering
- `StratChessEvolved/game_settings.json` – Runtime player/AI configuration
- `Docs/Roadmap.md` – Living development plan; check before starting any new work

## Development Guidelines
- Language: C++20; favor `constexpr`, RAII, move semantics, strong types
- Naming and comments must be in English and unambiguous
- Approved external dependencies only: `spdlog` (logging), `nlohmann/json` (config/serialization)
- All changes must be thread-safe, especially around transposition tables
- No regressions in search accuracy or ELO without explicit justification
- Benchmark before and after any optimization

## Testing & Validation

All tests run through `StratChessEvolved.exe` (built in `x64/Release/` or `x64/Debug/`).

### Unit tests (includes repetition tests TC1–TC7, TC9)
Run from any directory:
```bash
x64/Release/StratChessEvolved.exe unittest
```
Sources: `StratEngine/Tests/Unittests.h`, `RepetitionTests.h`, `MoveFieldTests.h`

### Perft tests
**Must be run from the `Tests/` directory** — the executable looks for `perft_test_cases.json` in the working directory:
```bash
cd Tests
../x64/Release/StratChessEvolved.exe perft test
```
Sources: `StratEngine/Tests/Perft.h/cpp` + `Tests/perft_test_cases.json`

### Self-play validation
- Run AIPerplex self-play (`"type": 6` for both sides in `game_settings.json`) to verify search behaviour
- For changes to base classes PlayerAI/PlayerBase, verify through AIAgent self-play (`"type": 3`) as well

### General
- Maintain deterministic behavior for reproducibility
- `game_settings.json`: verify FEN is set to the starting position before committing — test sessions often leave a custom FEN in place

## Commit & PR Conventions
- Development happens on `master`; PRs target `main`
- Keep PRs small and logically scoped
- Include motivation, design reasoning, and expected impact for non-trivial changes
- Seek review for changes affecting evaluation, move ordering, or search algorithms
- Claude worktrees live under `.claude/worktrees/` — build from a worktree works out of the box via `Directory.Build.props`
