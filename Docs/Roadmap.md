# StratChess Engine Roadmap

**Last Updated**: June 20, 2026
**Timeframe**: Next 6 months (through parallel search implementation)
**Current Version**: AIPerplex 2.0

---

## Overview

This roadmap organizes development tasks by priority and category. Items are drawn from recent debugging sessions, performance analysis, and architectural reviews conducted February–March 2026.

### Priority Levels
- 🔴 **Critical** - Must complete before parallel search
- 🟡 **High** - Significant performance or quality wins
- 🟢 **Medium** - Nice-to-have improvements
- ⚪ **Long-term** - Future research and experimental features

### Categories
- **Performance** - Speed and efficiency improvements
- **Features** - New search enhancements
- **Refactoring** - Code quality and maintainability
- **Infrastructure** - Testing and tooling

---

## 🔴 Critical Priority (Before Parallel Search)

### Refactoring

#### 🔴 De-Singleton Board
- **Estimate**: 3-4 days
- **Status**: Not started — none of the 4 approach steps below have been started
- **Blocking**: ThreadData extraction, parallel search (Lazy SMP), and clean unit testing of Eval/MoveGenerator
- **Rationale**: `Board::Instance()` is a global singleton referenced by `MoveGenerator`, `EvalManager`, and `AIPerplex`. This prevents:
  - Thread-local board copies required by Lazy SMP
  - Isolated unit testing of Eval and MoveGenerator without global state side-effects
  - The `ThreadData` struct (`Board board` member per thread)
- **Approach**:
  1. Remove `static` from `Board::Instance()`; make Board constructable directly (from FEN or default)
  2. Pass `Board&` through `MoveGenerator`, `EvalManager`, and `AIPerplex` call sites
  3. `ThreadData` holds its own `Board` copy (natural fit — see ThreadData item below)
  4. Update all test fixtures to construct `Board` directly (see `Docs/TestDesign.md` Phase 2)
- **Testing**: Existing perft + repetition tests serve as regression baseline; update fixtures to non-singleton pattern as part of this task
- **Test design note**: Tracked as Phase 2 in `Docs/TestDesign.md`

#### 🔴 Extract ThreadData Structure
- **Estimate**: 2-3 days
- **Status**: Not started
- **Blocking**: Parallel search implementation
- **Description**: Create thread-local state container to eliminate shared mutable state
- **Implementation**:
  ```cpp
  struct ThreadData {
      Board board;                    // Thread-local board copy
      int64_t nodes_searched;         // Thread-local counter
      PVTable pv_table;               // Thread-local PV
      std::vector<GameInfo> info_seq; // Thread-local game state
      int thread_id;                  // For debugging

      // Future: Thread-local move ordering
      KillerMoves killers;
      HistoryTable history;
  };
  ```
- **Files Affected**: `AIPerplex.h/cpp`, `PlayerAiIterBase.h`
- **Testing**: Verify single-threaded performance unchanged

---

## 🟡 High Priority (Significant Wins)

### Performance


---

## 🟢 Medium Priority (Nice-to-Have)

### Performance

#### 🟢 Add SEE (Static Exchange Evaluation) to Quiescence
- **Estimate**: 4-5 days
- **Impact**: 10-15% quiescence speedup
- **Description**: Evaluate capture sequences statically to prune losing captures
- **Implementation**:
  ```cpp
  int SEE(Move capture) {
      // Simulate capture sequence
      // Return material balance
  }

  // In quiescence:
  if (SEE(move) < 0 && !in_check) {
      continue;  // Skip losing capture
  }
  ```
- **Complexity**: Requires careful bitboard manipulation
- **Testing**: Verify tactical positions work correctly

#### 🟢 Implement Futility Pruning
- **Estimate**: 2-3 days
- **Impact**: 5-10% speedup
- **Description**: Skip nodes where evaluation + margin can't reach alpha
- **Types**:
  1. **Reverse Futility Pruning**: Near leaf nodes
  2. **Futility Pruning**: At frontier nodes
  3. **Extended Futility Pruning**: Multiple plies from leaf

### Refactoring

#### 🟢 GetMove TimeControl Refactor
- **Estimate**: 2-3 days
- **Dependency**: UCI protocol (#6) — do immediately after UCI lands
- **Description**: Pass a `TimeControl` struct directly into `GetMove()` instead of
  calling `SetClockInfo()` as a pre-call side-effect.  Eliminates temporal coupling,
  makes each call self-contained, and simplifies threading in Lazy SMP.
  ```cpp
  struct TimeControl {
      std::chrono::milliseconds remaining;
      std::chrono::milliseconds increment;
      int moves_to_go = 0;
  };
  // GameInfo gains an optional<TimeControl> field;
  // GetMove() reads it instead of relying on SetClockInfo() being called first.
  ```
- **Files affected**: `IPlayer.h`, `PlayerBase.h`, `PlayerAiBase.h/cpp`, `GameInfo.h`,
  all `GetMove()` overrides (PlayerHuman, AIBasic, AIAgent, ABIterative, AIPerplex),
  UCI handler, `Game.cpp::SetPlayerParams`
- **Note**: `SetClockInfo()` introduced by the Time Management PR (#5) becomes
  an internal implementation detail or is removed entirely once this refactor lands.

#### 🟢 Migrate `Move::Output()` callers to `MoveFormatter`
- **Estimate**: 1 hour
- **Impact**: Removes the last direct uses of `Move::Output()` / `Move::Output(ePiece)`,
  enabling those methods to be deleted
- **Remaining callers**: `Move::operator<<` (Move.cpp), `PVLine::operator<<` (Move.cpp),
  and ~10 `move.Output()` calls in `AIPerplex.cpp` (search diagnostic logging)
- **Note**: AIPerplex logging is coordinate-only (`Output()` no-arg) — acceptable today
  but inconsistent once ToUCI/ToShort are the canonical APIs
- **When**: when AIPerplex logging is refactored, or when `Move::Output` starts causing
  maintenance friction

### Infrastructure

#### 🟢 Phase 1 Test Infrastructure (add per-feature, see TestDesign.md)
- **Estimate**: Varies — add when touching the relevant component
- **Components** (details in `Docs/TestDesign.md`):
  - `[board]` / `[board_moves]` / `[board_state]` / `[board_api]` — DoMove/UndoMove completeness + full move-type, GameInfo state, and API coverage — ✅ done (`BoardTests.cpp`, `BoardMoveTests.cpp`, `BoardStateTests.cpp`, `BoardApiTests.cpp`)
  - `[sort]` — ✅ done — `MoveSorter::ScoreMoves()` extraction + 5 test cases (PR #38)
  - `[search]` — ✅ done — 10 test cases via `AIPerlexTestFixture`; landed with LMR (PR #38)
  - Full tactical suite (`StratChessEvolved.exe tactical test`) — add **before UCI** (engine readiness check; does not need to wait for evaluation extension)
  - `[bitboard]` — bitboard helper tests — add opportunistically

#### 🟢 Upgrade to C++23
- **Estimate**: 4-6 hours total
- **Dependency**: Extract ThreadData Structure (primary motivator — `std::mdspan` for history table)
- **Secondary dependency**: De-Singleton Board (for `std::expected` in FENParser)
- **Plan**: `.claude/plans/cpp23-upgrade.md`
- **Language Standard Change**: `stdcpp20` → `stdcpplatest` in both `.vcxproj` files (x64 configurations)
- **Items** (in dependency order):
  1. **With ThreadData extraction**: `stdcpplatest` bump + `std::mdspan<int32_t, std::extents<int,2,64,64>>` for `history_[2][64][64]`; `std::flat_multimap` for TT diagnostics
  2. **With De-Singleton Board**: `std::expected<ParsedFEN, std::string>` for `FENParser::ParseFEN`
  3. **With MoveOrdering refactor**: `std::views::enumerate` in `pvs()` move scoring loop
- **Already done** (March 2026): C++20 items `std::countr_zero` (replaces `_tzcnt_u64` #ifdef in `GetFirstPiece`) and `std::format` (replaces `stringstream` in `Move::Output()`) — see plan for details
- **Note**: MSVC VS 2022 17.8+ already satisfies all C++23 library requirements; no toolchain upgrade needed for this item

#### 🟢 Add Performance Profiling Scripts
- **Estimate**: 1 day
- **Tools**:
  - VTune or perf for CPU profiling
  - Custom benchmark suite
- **Metrics to Track**:
  - Nodes per second at different depths
  - TT hit rate
  - Move ordering effectiveness (beta cutoff position)
  - Quiescence search percentage
- **Output**: CSV files for regression tracking

#### 🟢 Create Automated Test Suite
- **Estimate**: 2-3 days
- **Components**:
  1. **Tactical Tests**: WAC (Win At Chess), BT2630, ECMGCP
  2. **Mate Tests**: Forced mate positions (mate in 2, 3, 4)
  3. **Endgame Tests**: Known tablebase positions
  4. **Regression Tests**: Positions from bug fixes
- **Acceptance**: Pass 80%+ tactical tests, 100% mate tests

---

## ⚪ Long-term Ideas (Research & Experimental)

### Parallel Search

#### ⚪ Implement Lazy SMP
- **Estimate**: 3-4 weeks
- **Prerequisite**: ThreadData refactoring complete
- **Description**: Multiple threads search same position with shared TT
- **Algorithm**:
  ```
  Main thread: Normal iterative deepening
  Helper threads: Start at depth d-3, d-2, etc.
  All share transposition table
  Best move propagates through TT
  ```
- **Challenges**:
  - Thread-safe TT access
  - Load balancing
  - Move ordering divergence
- **Target Speedup**: 2-3x on 4 cores, 3-5x on 8 cores

#### ⚪ Implement YBWC (Young Brothers Wait Concept)
- **Estimate**: 4-5 weeks
- **Description**: More sophisticated parallel search
- **Complexity**: Higher than Lazy SMP
- **ROI**: Questionable vs Lazy SMP simplicity

### Advanced Search

#### ⚪ Multi-PV Search
- **Estimate**: 1-2 weeks
- **Description**: Return top N moves with evaluations
- **Use Case**: Analysis mode, opening book generation

#### ⚪ Singular Extensions
- **Estimate**: 2-3 weeks
- **Description**: Extend search when one move is much better
- **Impact**: Better tactical play, 5-10% improvement

#### ⚪ Internal Iterative Deepening (IID)
- **Estimate**: 1 week
- **Description**: Search shallower when no hash move available
- **Impact**: Better move ordering, 3-5% improvement

### Evaluation

#### ⚪ Add King Safety Evaluation
- **Estimate**: 2-3 weeks
- **Components**: Pawn shield scoring, king tropism, open files near king, attack squares

#### ⚪ Add Mobility Evaluation
- **Estimate**: 1-2 weeks
- **Description**: Score based on legal moves available
- **Note**: Expensive to compute, maybe cache

#### ⚪ Tapered Evaluation (Midgame → Endgame)
- **Estimate**: 1 week
- **Description**: Interpolate between midgame and endgame scores based on material count

#### ⚪ Neural Network Evaluation (NNUE)
- **Estimate**: 3-6 months
- **Impact**: Potentially +300 Elo
- **Complexity**: Very high — requires training infrastructure
- **Reference**: Stockfish NNUE implementation
- **Prerequisite**: Strong classical evaluation baseline

### Advanced Features

#### ⚪ Syzygy Tablebase Support
- **Estimate**: 2-3 weeks
- **Library**: [Fathom](https://github.com/jdart1/Fathom)
- **Impact**: Perfect play in 7-piece endgames, draw avoidance

#### ⚪ Opening Book
- **Estimate**: 1 week (integration), varies (book creation)
- **Format**: Polyglot or custom
- **Source**: Master games database

#### ⚪ Time Management — Phase 2 (complexity-aware)
- **Estimate**: 1-2 days
- **Dependency**: Time Management Phase 1 (soft/hard limits, PR #5) ✅ done — do after UCI
- **Description**: Extend allocation formula with position-complexity signals
- **Factors**: Search instability (move changed last N iterations), material imbalance,
  number of legal moves (few moves → spend more time), score variance across depths
- **Note**: Phase 1 (clock-aware allocation, soft/hard limits) is implemented in PR #5.
  This item adds the *adaptive* layer on top of the static formula.

### Infrastructure

#### ⚪ Extract StratEngine Static Library
- **Estimate**: 1–2 days
- **Impact**: Engine compiled once instead of twice on clean builds (~50% reduction in compilation work)
- **Risk**: LTCG across a `.lib` boundary requires `/GL` on the library and `/LTCG` on the consuming linker; misconfiguration silently drops cross-TU inlining (5–15% NPS regression). Must be verified with a NPS benchmark.
- **Better fit**: Natural as a CMake `add_library(StratEngine STATIC …)` — combine with CMake migration rather than doing it in isolation

#### ⚪ Add sccache Compilation Caching
- **Estimate**: Half a day
- **Impact**: Near-instant subsequent clean builds when source is unchanged (branch-switch and switch-back). Zero benefit on first clean build.
- **Approach**: Install `sccache`; set `<CLToolExe>` in vcxproj or CMake `CMAKE_C_COMPILER_LAUNCHER`
- **Best fit**: Combined with a CI pipeline (GitHub Actions); less useful without CI

#### ⚪ Cross-Platform Build System
- **Estimate**: 3-5 days
- **Current**: MSVC on Windows; MSBuild `.vcxproj` / `.sln`; `build.ps1` script wraps invocation
- **Target**: CMake for Windows/Linux/Mac; Ninja back-end; `cmake --build . --target StratChessTests -j8`
- **CI**: GitHub Actions for automated builds
- **Note**: When migrating, preserve `INTERPROCEDURAL_OPTIMIZATION` (`/GL` + `/LTCG` equivalent) on the main Release target to avoid NPS regression; verify with a before/after NPS benchmark

#### ⚪ Migrate to Clang/LLVM Compiler
- **Estimate**: 1-2 days (on top of CMake migration effort)
- **Prerequisite**: Cross-Platform Build System (CMake) — **do both together**; migrating MSBuild + Clang in isolation is high cost, low reward
- **Plan**: `.claude/plans/cpp23-upgrade.md` (toolchain section)
- **Rationale**:
  - `clang-tidy` and `clang-format` enforce code quality in CI
  - Leading-edge C++23/26 language conformance (Clang typically ahead of MSVC on proposals)
  - Linux/Mac builds for free once on CMake
  - `std::countr_zero` still compiles to `TZCNT` — no NPS regression expected; verify with before/after benchmark
- **Note**: MSVC's C++23 standard **library** support is already complete for all features on this roadmap, so Clang is not needed to unlock C++23. This item is purely about toolchain quality and portability.

---

## Measurement & Success Criteria

### Performance Metrics
- **Nodes per second**: Track baseline, target +20% with optimizations
- **Depth reached**: 15 seconds reaches depth 13-15 (with LMR; was 8-9 before)
- **Win rate**: Maintain or improve vs AIAgent baseline

### Code Quality Metrics
- **Test coverage**: Target 80% for search algorithms
- **Build warnings**: Zero with `/W4` or `-Wall -Wextra`
- **Static analysis**: Zero critical issues (PVS-Studio)

### Regression Prevention
- Save positions from each bug fix as test cases
- Run full test suite before each release
- Track performance on standard benchmark suite

---

## Decision Framework

### When to Implement
Consider this order:
1. **Bug fixes** - Always first
2. **Blocking items** - Required for parallel search (De-Singleton Board, ThreadData)
3. **High-impact perf** - LMR, killer moves
4. **Infrastructure** - Testing, profiling (enables confidence)
5. **Advanced features** - NNUE, tablebases (after solid baseline)

### When to Skip
Avoid these traps:
- ❌ Premature optimization (profile first!)
- ❌ Feature creep (parallel search is the north star)
- ❌ Perfect-is-enemy-of-good (working > elegant)
- ❌ Over-engineering (YAGNI principle)

---

## ✅ Completed Work

### Search Algorithm Fixes (Feb 11, 2026)
- Fixed iterative deepening timeout move selection bug
- Fixed score=0 acceptance on interrupted searches
- Fixed PV table/move mismatch issue
- Fixed mate emergency infinite loop
- Fixed mate-found not stopping iteration
- Removed false-positive score-swing rejections (Case 5b)

### Refactor iterative_deepening (Feb 11, 2026)
- Extracted helper methods: `assess_iteration_quality`, `should_stop_early`, `handle_empty_move_emergency`
- Introduced `SearchResult` struct for clean interface
- Added `SearchTuning` struct for runtime parameters
- Improved log levels (debug/info/critical)
- Fixed mate-found iteration continuation bug

### Correctness: Threefold / Twofold Repetition (Feb 22, 2026)
- BUG-1: `push_position()` now called after `ChangePlayer()` in both `DoMove()` branches
- BUG-2: `is_repetition()` loop start corrected (parity fix)
- BUG-3: Twofold-in-search branch was unreachable; fixed dead condition
- BUG-4: Castling rights and en-passant square changes now included in Zobrist hash
- `zobrist_hash_` widened from `unsigned int` to `uint64_t`
- Dead `mark_irreversible()` removed; `updateThreefoldRep` made private and renamed

### Performance: Delta Pruning in Quiescence (Feb 26, 2026)
- `tuning_.delta_pruning_margin = 200` added to `SearchTuning`
- Guard: skip captures where `stand_pat + piece_value + margin < alpha` (not in check, not promotion)
- Consistently deeper search — mate at depth 14 observed where it wasn't reached before
- Verified: unit tests, perft, self-play — no regressions

### Infrastructure: TranspositionTable Thread-Safety
- Per-bucket `shared_mutex` locks: probes use `shared_lock`, stores use `unique_lock`
- Global `shared_mutex` for resize/clear operations
- Atomic counters for O(1) diagnostics
- TT only cleared on new game (preserves TT across moves for 10-15% self-play improvement)

### Infrastructure: Perft Testing Framework
- Implementation: `StratEngine/Tests/Perft.h/cpp`
- Test runner: `StratEngine/Tests/PerftRunner.cpp`
- Test cases: `Tests/perft_test_cases.json` — all passing
- Integrated into main binary: `StratChessEvolved.exe perft test`

### Infrastructure: Migrate to Catch2 v3 (March 2026)
- Catch2 v3 amalgamated (2-file drop-in, no pre-build step required)
- Test project `StratChessTests/StratChessTests.vcxproj` rebuilt from empty placeholder
- Tests migrated: `RepetitionTests.cpp` (TC1-TC7, TC9), `MoveFieldTests.cpp`, `PerftTests.cpp`
- Retired: `TestFramework.h`, `Unittests.h`, `Perft_unittests.h`
- Tags: `[repetition]`, `[moves]`, `[perft]`

### Infrastructure: Phase 0 Test Coverage (March 2026)
- `[tt]` — TranspositionTable unit tests (store/probe, mate normalization, replacement, counters)
- `[eval]` — Evaluation position tests (symmetry, material advantage, doubled pawns, rook bonus)
- `[tactical]` — Fast search regression tests (mate-in-1, hanging piece capture via AIPerplex depth 4)
- `STRAT_ENABLE_TEST_ACCESS` friend stub added to `AIPerplex.h` for future Phase 1 search tests

### Refactoring: Archive Broken Algorithms
- `ABIterTrans.cpp/h` and `AITrans.cpp/h` moved to `StratEngine/Archived/`
- `Archived/README.md` explains historical context
- Both removed from build system

### Refactoring: Restrict Board Piece-Setup API to Private (commit d4b1db6)
- `ClearBoard`, `SetInitialColor`, and `AddPieceToBoard` moved to `private:`
- `SetupFromFEN` is now the sole public API for board configuration
- Motivated by retirement of `MoveGeneratorPromotionTests.h` (PR #20)

### Features: Killer Moves + History Heuristic
- Fully implemented in `AIPerplex`: `killers_[MAX_PLY][MAX_KILLERS]`, `history_[2][64][64]`
- Methods: `clear_killers`, `store_killer`, `clear_history`, `age_history`, `update_history`
- Killers cleared at search start; history aged each iteration (halved to prevent overflow)
- Scoring integrated inline in `pvs()` — relocation to `MoveSorter` is a separate task

### Refactoring: State Management — GameInfo History in Board
- `GameInfo` history moved from Player into `Board`: `capturedHistory_`, `gameInfoHistory_`,
  `irreversiblePlyHistory_`, `zobrist_history_` ply-indexed arrays added directly to `Board.h/cpp`
- Clean separation of concerns; prerequisite data for De-Singleton Board work

### Refactoring: Introduce MoveFormatter — centralise move presentation (March 2026)
- **Stateless class** with four static methods in `StratEngine/MoveFormatter.h/cpp`:
  - `ToShort(Move, Board)` — pseudo-LAN + `+` check annotation (e.g. `"Rc1xc7+"`)
  - `ToVerbose(Move, Board)` — verbose English (e.g. `"White rook captures on c7 and checks!"`)
  - `ToUCI(Move)` — UCI wire format (e.g. `"e2e4"`, `"b7b8q"`) — no board context needed
  - `FromUCI(string_view, Board)` — parse UCI move from board pre-DoMove state
- **Gaps fixed**: verbose line restored; `+` now in `gamelist.txt`; fragile `\n`-surgery
  in `PrintBoardAndMove` removed; promotion-captures now get suffix in perft divide output
- **`Move::Output()` / `Move::Output(ePiece)`**: kept as-is (12 callers in AIPerplex
  use coordinate-only output for search logging; migration deferred — see `Migrate Move::Output()` item)
- **`ToSAN`** (Standard Algebraic Notation): omitted — deferred until PGN export is needed
- **Tests**: `StratChessTests/MoveFormatterTests.cpp` tag `[formatter]` — 65 assertions, 6 test cases
- **Plan**: `.claude/plans/move-formatter.md`

### Move class → 16-bit layout (Phases 1–4, March 2026)
- **Phase 1**: Removed `Move::IsCheck` field
- **Phase 2**: Removed `[from|to]IsNoSquare` fields
- **Phase 3**: Removed `MovPiece` field — `Board::GetEffectiveMovPiece()` added; `MoveFactory`
  drops movPiece param; `MoveHelper`/`GameState`/`Sort` thread explicit movPiece; Zobrist hash
  corruption in `UndoMove` fixed; `BoardTests.cpp` added (6 `[board]` test cases; all 47 tests pass)
- **Phase 4**: Removed `Content` (captured-piece) field — `sizeof(Move) == 2` enforced via
  `static_assert`; 4 `PROMOTION_*_CAPTURE` MoveType variants added (capture bit 2 + promotion bit 3);
  `Board::GetCapturedPiece()` public API added; `MoveFactory` drops captured param;
  `Move::Value` / `MoveHelper::IsValid` / `IsPieceCapturedAt` gain explicit `content` param;
  `IsCapture` / `IsPromote` simplified to pure flag-bit tests; all 47 tests pass

### Move sorting: Stack-allocated sort buffer
- `pvs()` uses `std::array<std::pair<int,int>, MoveList::MAX_MOVES>` on the stack
- Zero heap allocation per call; `thread_local` buffer approach ruled out (extra copy required for recursion safety)

### Performance: Aspiration Windows in Iterative Deepening (PR #30, March 2026)
- Narrow alpha/beta window around previous depth's score; gradual widening (25cp → 75cp → full) on fail-high/fail-low
- Kill-switch: `tuning_.aspiration_enabled` — depth 1 always uses full window regardless
- `search_with_aspiration()` extracted into its own method in `AIPerplex.cpp`
- Verified: no search regressions; self-play shows stable score progression across iterations

### Infrastructure: Expand PCH Coverage in StdAfx.h (March 2026)
- Added 9 STL headers: `<algorithm>`, `<array>`, `<cassert>`, `<cstdint>`, `<functional>`, `<memory>`, `<sstream>`, `<string>`, `<utility>`
- Removed redundant per-TU includes from `AIPerplex.cpp`, `Sort.cpp`, `MoveGenerator.cpp`, `Move.cpp`, `MoveFormatter.cpp`, `PlayerHuman.cpp`, `Utils/FENParser.cpp`
- Verified: full rebuild + all 214 test assertions pass

### Refactoring: Extract Magic Numbers into SearchTuning (March 2026)
- `barelySearched` threshold → `tuning_.min_nodes_threshold = 1000`
- `probablyIncomplete` threshold → `tuning_.min_completion_ratio = 0.10`
- `pvTooShort` threshold → `tuning_.min_pv_ratio = 0.33`
- All three values exposed via `game_settings.json` for runtime tuning

### Refactoring: Remove Dead Code (March 2026)
- Commented-out old `Search()` method body removed from `AIPerplex.cpp`
- Remaining `// TODO: Relocate MoveSorting to MoveSorter class` is a live tracked item (see "Migrate Inline Move Scoring" in active section)

### Infrastructure: Fix zobrist::initialize() Never Called (March 2026)
- `zobrist::initialize()` now called in `Board` constructor (`Board.cpp` line 50)
- All castling, en-passant, and side-to-move Zobrist keys initialised before any Board method runs
- Stale "never called" comment in `Board.h` removed

### Logging: spdlog Level Gate + outLegalMoves Removal (PR #34, March 2026)
- 3-line per-call logging boilerplate in `AIPerplex` replaced with spdlog level gate (`s_logger->set_level(...)`)
- Global `outLegalMoves` stream removed; board/root-move diagnostics now flow through default spdlog logger at `debug` level
- `Board::test_bitboards` signature simplified
- All four runtime log files documented in `CLAUDE.md`
- Plan: `.claude/plans/logging-spdlog-gate-and-outlegalmoves-removal.md`

### Time Management: Clock-Aware Soft/Hard Limits (PR #5, March 2026)
- `Engine::compute_budget(remaining, increment, moves_to_go)` free function in `TimeUtils.h/cpp`
  — pure math, independently testable; formula: `soft = usable/horizon + inc*80%`, `hard = min(soft*3, usable/2)`
- `TimeManager` gains two-arg `start(soft, hard)` + `should_stop_iteration()` (soft limit check)
- `PlayerAiBase::SetClockInfo()` public method: computes budget and arms timer; `clock_info_set_`
  flag prevents `StartTimer()` from overwriting the clock-aware budgets
- `AIPerplex::iterative_deepening()` soft-limit gate: stop after depth if `should_stop_iteration()`
  and best move was stable; allow one extra depth if best move just changed (verify the new move)
- Node-based time polling in `pvs()`: check every 1,024 nodes instead of every call
  (amortises `chrono::now()` overhead on deep searches)
- `[time_mgr]` test tag: 10 assertions (6 formula, 4 timing), total < 300 ms
- Plan: `.claude/plans/time-management-clock-aware.md`

### Move Sorting: Extract ScoreMoves + [sort] Tests (PR #38, March 2026)
- Inline move scoring loop extracted from `pvs()` into `MoveSorter::ScoreMoves()` static method
- Precondition asserts + `isKiller1` short-circuit for fast killer detection
- 5 `[sort]` test cases, 14 assertions — full priority order locked in
- Plan: `.claude/plans/move-scoring-extraction-and-sort-tests.md`

### Performance: Late Move Reductions (PR #38, March 2026)
- sqrt formula: `R = min(max(1, sqrt(depth-1) * sqrt(si-1)), depth-1)`
- Applied to quiet, non-killer, non-evasion, non-PV-node moves (si≥3, depth≥3); 2-step re-search
- Kill-switch: `tuning_.lmr_enabled` (parallels `aspiration_enabled`)
- Observed: depth 13-15 vs 8-9 (without LMR) in same 15-second budget; ~31M vs ~36M nodes/move
- 10 `[search]` test cases via `AIPerlexTestFixture` friend class; `STRAT_ENABLE_TEST_ACCESS` gate
- Plan: `.claude/plans/lmr-and-search-tests.md`

### Refactoring: C++20 Adoption — `<bit>` and `<format>` (PR #32, March 2026)
- `std::countr_zero` replaces `_tzcnt_u64` `#ifdef` block in `Board::GetFirstPiece`
- `std::format` replaces `std::stringstream` in `Move::Output()`
- `<bit>` and `<format>` added to `StdAfx.h` PCH
- C++23 upgrade path documented in `.claude/plans/cpp23-upgrade.md`

### Performance: Null-Move Pruning (PR #55, June 2026)
- `tuning_.null_move_enabled` defaults to `true`; guard helper `should_try_null_move()` centralises every condition: zugzwang (no non-pawn material), mate-score contamination, consecutive-null, PV/in-check, min-depth
- Two real bugs fixed along the way: `Board::DoNullMove()` wasn't forfeiting en-passant rights (zobrist/EP desync); `PlayerAiBase::m_infoSeq` wasn't sized for null-move plies (out-of-bounds crash the first time NMP recursed in a real self-play game, not caught by unit tests alone)
- Plan: `.claude/plans/null-move-pruning.md`

### Correctness: Decouple `Board::currentPly_` from Game Length (PR #57, issue #53, June 2026)
- `currentPly_` indexes four fixed `MAX_PLY=256` ply-history arrays but was never reset after a permanently-committed move, so it grew with total game length while search recursion added depth on top — overflowed the arrays after ~250 real moves (self-play access violation around move 247-249)
- `Board::ResetSearchDepth()` added, called after every permanent move commit (`Game::Run`'s real move, `UCIHandler`'s position-moves replay), so `currentPly_` only ever spans in-flight search recursion depth, never game length
- `assert(currentPly_ < MAX_PLY)` guard added in `DoMove`/`UndoMove` as defense in depth
- Regression tests: undo-stack depth doesn't accumulate across 320 simulated real moves; full search-recursion headroom remains after a 260-move game

### Infrastructure: UCI Protocol (March 2026)
- `UCIHandler` class: synchronous command loop with search on `std::thread`
- Commands: `uci`, `isready`, `ucinewgame`, `position` (startpos + fen + moves), `go`, `stop`, `quit`
- Time control: `movetime`, `wtime`/`btime`/`winc`/`binc`/`movestogo`, `depth`, `infinite`
- `AIPerplex::GetLastResult()` exposes `SearchResult` for post-search `info` line
- `PlayerAiBase::StopSearch()` public API calls `time_manager_.stop()` (thread-safe)
- All spdlog output silenced via `spdlog::set_level(off)` — stdout is UCI protocol only
- Invoked via: `StratChessEvolved.exe uci`; game mode unchanged (no args)
- Validated: pipe-based functional smoke test (uci → isready → position → go movetime → quit)

---

**Document Version**: 1.2
**Next Review**: June 2026
**Owner**: Thees
