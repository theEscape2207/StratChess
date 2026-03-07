# StratChess Engine Roadmap

**Last Updated**: March 3, 2026
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
- **Status**: In progress — `Position.h/cpp` created (not yet committed)
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
- **Status**: In progress — `AIPerplexParallel.h/cpp` created (not yet committed)
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

#### 🟡 Implement Late Move Reductions (LMR)
- **Estimate**: 3-5 days
- **Impact**: 2-3x speedup (most important optimization!)
- **Description**: Reduce search depth for late moves, re-search if they raise alpha
- **Algorithm**:
  ```cpp
  if (move_number > 3 && depth > 2 && !is_tactical(move)) {
      reduction = 1 + (move_number / 6);

      // Search at reduced depth
      score = -pvs(depth - 1 - reduction, -alpha-1, -alpha, ...);

      // Re-search at full depth if needed
      if (score > alpha && reduction > 0) {
          score = -pvs(depth - 1, -alpha-1, -alpha, ...);
      }

      // Final re-search with full window
      if (score > alpha && is_pv_node) {
          score = -pvs(depth - 1, -beta, -alpha, ...);
      }
  }
  ```
- **Tuning Parameters**:
  - Base reduction: 1 ply
  - Progressive factor: move_number / 6
  - Minimum move number: 4
  - Minimum depth: 3
- **Testing**:
  - Verify tactical positions aren't hurt
  - Measure depth increase (should reach depth+2 or more)
  - Compare against AIAgent baseline

#### 🟡 Implement Aspiration Windows
- **Estimate**: 2 days
- **Impact**: 10-15% speedup
- **Description**: Use narrow window around previous iteration's score
- **Implementation**:
  ```cpp
  int aspiration_window = 50;  // centipawns
  int alpha = state.best_score - aspiration_window;
  int beta = state.best_score + aspiration_window;

  int score = pvs(depth, alpha, beta, 0, true, tt, pv_table);

  // Re-search if outside window
  if (score <= alpha || score >= beta) {
      score = pvs(depth, -INF, +INF, 0, true, tt, pv_table);
  }
  ```
- **Tuning**: Start with 50cp window, measure fail rate
- **Note**: AIAgent already has this - can reference implementation

### Features

#### 🟡 Migrate Inline Move Scoring into MoveSorter
- **Estimate**: 2 days
- **Status**: In progress — `MoveOrdering.h/cpp` created as a redesign (not yet committed)
- **Impact**: Cleaner `pvs()` method; move ordering testable in isolation
- **Current State**: `MoveSorter` exists in `Sort.h/cpp` but only handles capture prioritization (recapture + MVV-LVA). The full scoring loop — PV move, hash move, killer slots, history table, MVV-LVA — is inline in `pvs()` with a `// TODO: Relocate MoveSorting to MoveSorter class` comment (AIPerplex.cpp line 346)
- **Remaining Work**: Move the scoring logic out of `pvs()` and into `MoveSorter` (or its redesigned successor `MoveOrdering`), passing killers and history as parameters
- **Move Ordering Priority** (already implemented inline, just needs relocation):
  1. PV move (2,000,000)
  2. Hash move (1,900,000)
  3. Winning captures — MVV-LVA > 0 (1,000,000 + value)
  4. Killer slot 0 (900,000)
  5. Killer slot 1 (800,000)
  6. Equal captures (700,000)
  7. History score (quiet moves)
  8. Losing captures (-100,000 + value)
- **Files**: `StratEngine/Sort.h/cpp` (extend or replace with `MoveOrdering.h/cpp`)

---

## 🟢 Medium Priority (Nice-to-Have)

### Performance

#### ✅ Expand PCH Coverage in StdAfx.h — COMPLETE
- **Added**: `<algorithm>`, `<array>`, `<cassert>`, `<cstdint>`, `<functional>`, `<memory>`, `<sstream>`, `<string>`, `<utility>` (9 headers)
- **Cleaned**: redundant per-TU includes removed from `AIPerplex.cpp`, `Sort.cpp`, `MoveGenerator.cpp`, `Move.cpp`, `MoveFormatter.cpp`, `PlayerHuman.cpp`, `Utils/FENParser.cpp`
- **Verified**: full rebuild + all 214 test assertions pass

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

#### ✅ Move class → 16-bit layout — COMPLETE (all 4 phases done)


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

#### 🟢 Extract Magic Numbers to Constants
- **Estimate**: 2 hours
- **Impact**: Better maintainability
- **Files**: `AIPerplex.cpp`, `quiescence()`
- **Current Issues**:
  ```cpp
  const bool barelySearched = (nodes < 1000);          // Magic!
  const bool probablyIncomplete = (ratio < 0.10);      // Magic!
  const bool pvTooShort = (pv_len < depth / 3);        // Magic!
  ```
- **Solution**: Move remaining magic numbers into `SearchTuning`

#### 🟢 Remove Dead Code
- **Estimate**: 1 hour
- **Files**: `AIPerplex.cpp` lines 114-142 (commented old Search method)
- **Check**: Search for `// TODO` comments and evaluate each

### Infrastructure

#### ✅ Fix `zobrist::initialize()` never called — COMPLETE
- `zobrist::initialize()` is called in the `Board` constructor (`Board.cpp` line 50); all castling, en-passant, and side-to-move keys are initialised before any Board method runs.
- Stale "never called" comment in `Board.h` removed.

#### 🟢 Phase 1 Test Infrastructure (add per-feature, see TestDesign.md)
- **Estimate**: Varies — add when touching the relevant component
- **Components** (details in `Docs/TestDesign.md`):
  - `[search]` — AIPerplex helper tests (`assess_iteration_quality`, `should_stop_early`, `handle_empty_move_emergency`) — add when LMR/aspiration windows lands
  - `[sort]` — Move ordering tests — add when `MoveOrdering` class is committed
  - `[board]` — `DoMove`/`UndoMove` completeness — add when Move layout Phases 3 & 4 land
  - `[bitboard]` — bitboard helper tests — add opportunistically
  - Full tactical suite (`StratChessEvolved.exe tactical test`) — add when evaluation is extended

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

#### ⚪ Time Management Improvements
- **Estimate**: 2-3 days
- **Description**: Better time allocation based on position complexity
- **Factors**: Material balance, move number, search stability

### Infrastructure

#### ⚪ UCI Protocol Support
- **Estimate**: 1-2 weeks
- **Prerequisite**: MoveFormatter (`ToUCI` / `FromUCI`)
- **Description**: Universal Chess Interface for GUI compatibility
- **GUIs**: Arena, ChessBase, Fritz
- **Commands**: `uci`, `isready`, `position`, `go`, `stop`

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
- **Depth reached**: 15 seconds should reach depth 10-12
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
3. **High-impact perf** - LMR, killer moves, aspiration windows
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
- `GameInfo` history moved from Player into `Board`; `Position.h/cpp` created
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

---

**Document Version**: 1.1
**Next Review**: June 2026
**Owner**: Thees
