# CLAUDE.md – StratChessEvolved

## Project Overview
A modern C++20 chess engine focused on improving playing strength (ELO) while maintaining clarity, efficiency, and robustness.

## Repository Structure
- `StratChessEvolved/` – Main application entry point and project files
- `StratEngine/` – Core engine source (search, evaluation, move generation, AI agents)
- `StratChessEvolved.sln` – Visual Studio solution file

## Build
- Visual Studio solution (`.sln`) targeting C++20
- Supports both Debug and Release builds; prefer Release for performance benchmarking

## Engine Features
- Iterative Deepening Search (IDS)
- Principal Variation Search (PVS)
- Quiescence Search
- Transposition Tables (Zobrist hashing)
- Bitboard-based board representation

## Key Source Files
- `StratEngine/Board.cpp` / `Board.h` – Board state and move application
- `StratEngine/MoveGenerator.cpp` / `MoveGenerator.h` – Legal move generation
- `StratEngine/Eval.cpp` / `Eval.h` – Position evaluation
- `StratEngine/ABIterTrans.cpp` / `ABIterTrans.h` – Alpha-beta with transposition tables
- `StratEngine/TranspositionTable.cpp` / `TranspositionTable.h` – TT implementation
- `StratEngine/AIPerplex.cpp` – Primary AI agent
- `StratEngine/Sort.cpp` / `Sort.h` – Move ordering

## Development Guidelines
- Language: C++20; favor `constexpr`, RAII, move semantics, strong types
- Naming and comments must be in English and unambiguous
- Approved external dependencies only: `spdlog` (logging), `nlohmann/json` (config/serialization)
- All changes must be thread-safe, especially around transposition tables
- No regressions in search accuracy or ELO without explicit justification
- Benchmark before and after any optimization

## Testing & Validation
- Use Perft tests to validate move generation correctness
- Run self-play or controlled matches to verify ELO impact
- Maintain deterministic behavior for reproducibility

## Commit & PR Conventions
- Keep PRs small and logically scoped
- Include motivation, design reasoning, and expected impact for non-trivial changes
- Seek review for changes affecting evaluation, move ordering, or search algorithms
