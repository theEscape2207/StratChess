# StratChess Engine Roadmap

**Last Updated**: February 28, 2026
**Timeframe**: Next 6 months (through parallel search implementation)  
**Current Version**: AIPerplex 2.0

---

## Overview

This roadmap organizes development tasks by priority and category. Items are drawn from recent debugging sessions, performance analysis, and architectural reviews conducted February 8-11, 2026.

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

#### ✅ COMPLETED: Refactor iterative_deepening
- **Status**: ✅ Done (Feb 11, 2026)
- **Impact**: Fixed timeout bugs, improved maintainability
- **Changes**: 
  - Extracted helper methods (assess_quality, should_stop_early, handle_emergency)
  - Introduced SearchResult struct
  - Added tunable SearchTuning parameters
  - Fixed mate-found iteration continuation bug

#### 🔴 Extract ThreadData Structure
- **Estimate**: 2-3 days
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

#### ✅ COMPLETED: Verify TranspositionTable Thread-Safety
- **Status**: ✅ Done
- **Implementation**: Per-bucket `shared_mutex` locks in `TranspositionTable.h`
- **Details**:
  - Probes use `shared_lock` for concurrent reads
  - Stores use `unique_lock` for exclusive writes
  - Global `shared_mutex` for resize/clear operations
  - Atomic counters for O(1) diagnostics
- **Next**: Benchmark with parallel search to measure locking overhead

#### 🔴 Archive Broken Algorithms
- **Estimate**: 1 hour
- **Blocking**: Codebase clarity
- **Actions**:
  1. Move `ABIterTrans.cpp/h` to `Archived/` directory
  2. Move `AITrans.cpp/h` to `Archived/`
  3. Create `Archived/README.md` explaining historical context
  4. Remove from build system
- **Rationale**: Both have TT bugs, cluttering active codebase

---

## 🟡 High Priority (Significant Wins)

### Performance

#### ✅ COMPLETED: Re-enable thread_local Move Sorting Buffer
- **Status**: ✅ Done — superseded by stack-allocated `std::array`
- **Outcome**: `pvs()` already uses `std::array<std::pair<int,int>, MoveList::MAX_MOVES>` on the
  stack, giving zero heap allocation per call. A `static thread_local` buffer would require an
  extra copy step to be recursion-safe (the array is still read on each loop iteration after
  the recursive call returns), so the stack array is the correct final form.

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

#### ✅ COMPLETED: Add Delta Pruning to Quiescence
- **Status**: ✅ Done (Feb 26, 2026)
- **Impact**: Fewer qsearch nodes, consistently deeper search — mate at depth 14 observed in a
  standard 15s/move game where it would not have been reached before
- **Implementation**:
  - `tuning_.delta_pruning_margin = 200` added to `SearchTuning` struct (`AIPerplex.h`)
  - `const bool in_check = m_Board.InCheck()` computed once before the capture loop
  - Guard at top of loop: `if (!in_check && stand_pat + Move::Value(move) + tuning_.delta_pruning_margin < alpha) continue;`
  - Uses `Move::Value()` (MVV-LVA, conservatively ≤ raw capture value) — pruning is never over-aggressive
  - Promotions always score ≥ 800cp and are never pruned; in-check positions always fully searched
- **Verified**: Unit tests ✅, Perft ✅, self-play ✅ — no functional regressions

#### 🟡 Extract Move Ordering to MoveSorter Class
- **Estimate**: 2 days
- **Impact**: Better code organization, enables future enhancements
- **Current State**: Move ordering logic scattered in `pvs()` method
- **Target Design**:
  ```cpp
  class MoveSorter {
  public:
      static void SortMovesForPVS(
          MoveList& moves,
          Move pv_move,
          Move hash_move,
          int ply,
          const KillerMoves& killers = {}    // Future
          const HistoryTable& history = {}   // Future
      );
  };
  
  // Usage in pvs():
  MoveSorter::SortMovesForPVS(moveList, pv_move, hash_move, ply);
  ```
- **Files**: Create new `MoveSorter.cpp`, update `MoveSorter.h`
- **Benefits**: 
  - Cleaner `pvs()` method
  - Easier to add killer moves and history
  - Testable in isolation

### Features

#### 🟡 Add Killer Moves + History Heuristic
- **Estimate**: 3-4 days
- **Impact**: 15-20% speedup
- **Prerequisite**: Move ordering extracted to MoveSorter
- **Components**:
  1. **Killer Moves**: 2 killers per ply
     ```cpp
     struct KillerMoves {
         std::array<std::array<Move, 2>, MAX_PLY> killers;
         void update(int ply, Move move);
     };
     ```
  2. **History Table**: Score moves by success
     ```cpp
     struct HistoryTable {
         std::array<std::array<int, 64>, 64> scores;  // [from][to]
         void update(Move move, int depth, bool caused_cutoff);
         int get_score(Move move) const;
     };
     ```
- **Integration**: Add to ThreadData for parallel-readiness
- **Move Ordering Priority**:
  1. PV move (200,000)
  2. Hash move (150,000)
  3. Captures (MVV-LVA)
  4. Killer moves (10,000 + slot)
  5. History score (0-10,000)
  6. Quiet moves (tiebreaker)

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

#### 🟢 Introduce MoveFormatter — centralise move presentation
- **Status**: Not started
- **Motivation**: Removing `Move::IsCheck` (Phase 1 of the 16-bit layout work) exposed a
  deeper structural problem: display logic is scattered across three locations, none of which
  have the board context needed to produce correct output:
  - `Move::Output()` — pseudo-LAN notation (`Pe2-e3`, uppercase = White / lowercase = Black);
    no `+`, no `#`, no disambiguation; used for PV logging and the "Last move:" console line
  - `Move::operator<<` — verbose English description (`White pawn moves e2-e3`); no board
    access; check currently requires
    string surgery in `PrintBoardAndMove` (searching for `\n` offsets to inject `+` /
    ` and checks!`) — fragile, breaks silently if the format changes
  - `Game::PrintBoardAndMove` / `PrintGameMoves` — ad-hoc annotation at the call site;
    **`gamelist.txt` currently receives no check annotation at all** because `PrintGameMoves`
    calls `operator<<` on a stored `Move` without access to the board state at that moment
- **Design**: Introduce a stateless `MoveFormatter` class (static methods, no instance):
  ```cpp
  // Informal short notation with +/# — replaces Move::Output() + manual injection
  static std::string ToShort(const Move&, const Board&);

  // Verbose / Long Algebraic Notation — replaces operator<< + manual injection
  static std::string ToVerbose(const Move&, const Board&);

  // UCI wire format (e.g. "e2e4", "e7e8q") — prerequisite for UCI protocol
  static std::string ToUCI(const Move&);
  static Move        FromUCI(std::string_view, const Board&); // inverse, for UCI input

  // Standard Algebraic Notation (e.g. "Nf3+", "exd5#", "O-O")
  // Requires board for piece disambiguation and checkmate detection
  static std::string ToSAN(const Move&, const Board&);
  ```
  `Game::PrintBoardAndMove` and `PrintGameMoves` delegate to `ToVerbose`/`ToShort`,
  receiving `board.InCheck()` context naturally. `Move::Output()` and `operator<<`
  become thin context-free wrappers or are deprecated in favour of the formatter.
- **Gaps this directly fixes**:
  - `+` and `#` missing from `gamelist.txt`
  - Fragile `\n`-offset injection hack in `PrintBoardAndMove` removed
  - Single authoritative place for disambiguation, promotion suffix (`=Q`), and `#`
- **Enables**: UCI protocol (`ToUCI` / `FromUCI`), PGN export (`ToSAN`), any future GUI
  or web integration
- **Suggested order**: after Move layout Phases 3 & 4 (so piece type is obtained via
  `Board::GetEffectiveMovPiece()` rather than `Move::MovPiece` directly); before UCI

#### 🟢 Move class → 16-bit layout (Phases 3 & 4 deferred)
- **Status**: Phases 1 (IsCheck) and 2 ([from|to]IsNoSquare) complete — see commit history
- **Design document**: `.claude/plans/prancy-prancing-trinket.md`
- **Remaining work**:
  - **Phase 3 — Remove `MovPiece`**: Add `Board::GetEffectiveMovPiece()`, thread `ePiece movPiece`
    param through `MoveHelper`, `GameState`, `Sort`, `AIPerplex`, factory callers.
    See plan §Phase 3 for full file list and code sketches.
  - **Phase 4 — Remove `Content`**: Add `Board::captured_history_[]` undo stack;
    update `DoMove`/`UndoMove`; update `Move::Value(move, movPiece, content)` signature;
    strip `captured` param from `MoveFactory`; add `static_assert(sizeof(Move) == 2)`.
- **Prerequisite**: Phase 3 must complete before Phase 4
- **Final validation**: perft suite + repetition tests + self-play with AIPerplex & AIAgent

#### Architecture Refactor - State Management

**Current Issue:** GameInfo history is in Player, but Board/Position need it

**Steps:**
1. [ ] Quick fix: Add state save/restore to Perft (temporary)
2. [ ] Enable Position.cpp and Position.h
3. [ ] Move GameInfo history from Player to Position
4. [ ] Update Board to work with Position
5. [ ] Update search algorithms to use Position
6. [ ] Remove state management from Player classes
7. [ ] Update Perft to use Position instead of Board directly
8. [ ] Verify all tests still pass

**Benefits:**
- Clean separation of concerns
- Thread-safe for parallel search
- Matches industry standard pattern
- Easier to test and maintain

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
- **Solution**: Already in SearchTuning, document others

#### 🟢 Add Unit Tests for Helper Methods
- **Estimate**: 3-4 days
- **Impact**: Confidence in refactoring
- **Target Methods**:
  - `assess_iteration_quality()` - Mock metrics, verify decisions
  - `should_stop_early()` - Mate detection, forced lines
  - `handle_empty_move_emergency()` - Mate vs true emergency
- **Framework**: Google Test or Catch2
- **Files**: Create `Tests/AIPerplex_Tests.cpp`

#### 🟢 Remove Dead Code
- **Estimate**: 1 hour
- **Files**: `AIPerplex.cpp` lines 114-142 (commented old Search method)
- **Check**: Search for `// TODO` comments and evaluate each

### Infrastructure

#### 🟢 Migrate test board setup from piece-by-piece API to FEN
- **Estimate**: 1-2 hours
- **Impact**: Allows `ClearBoard`, `SetInitialColor`, `AddPieceToBoard` to become private, shrinking the public interface further
- **Motivation**: `MoveGeneratorPromotionTests.h` still uses the low-level triple-call pattern (`ClearBoard` + `SetInitialColor` + `AddPieceToBoard`) rather than `SetupFromFEN`, which is already used in `RepetitionTests.h` and is easier to read and maintain
- **Actions**:
  1. Convert each test case in `StratEngine/Tests/MoveGeneratorPromotionTests.h` to use `SetupFromFEN`
  2. Remove `SetInitialColor` from the public interface (no other callers)
  3. Move `ClearBoard` and `AddPieceToBoard` to `private:` in `Board.h`
- **Files**: `StratEngine/Tests/MoveGeneratorPromotionTests.h`, `StratEngine/Board.h`

#### 🟢 Fix `zobrist::initialize()` never called
- **Estimate**: 2-3 hours
- **Impact**: Correctness — castling rights, en-passant square, and side-to-move changes currently contribute zero to the Zobrist hash (XOR with 0 is a no-op)
- **Root cause**: `zobrist::initialize()` is defined in `Board.cpp` and declared in `Board.h` but is never called. The keys in `zobrist::castling_keys`, `zobrist::ep_keys`, and `zobrist::side_key` remain zero.
- **Note**: The board still hashes correctly for piece placement (handled by `allHashKeys` / `InitHashkey()`), so this is not causing visible incorrectness today; repetition detection works because the same-state hash collisions cancel out. But it is a latent bug.
- **Actions**:
  1. Call `zobrist::initialize()` early in program startup (e.g., in `main()` or `Config` initialisation)
  2. Evaluate whether `allHashKeys` (the `Board`-internal, non-deterministic table) should be unified with `zobrist::piece_keys` for a single consistent Zobrist scheme
- **Files**: `StratChessEvolved/StratChessEvolved.cpp` or `StratEngine/Config.cpp`, `StratEngine/Board.h`, `StratEngine/Board.cpp`

#### 🟢 Migrate to Google Test or Catch2
- **Estimate**: 2-3 days
- **Impact**: Proper test isolation, fixtures, parameterised tests, IDE integration
- **Current State**: Custom `TestFramework.h` with hand-rolled `TEST_ASSERT` macros and global counters; no test isolation between test functions (shared singleton Board)
- **Target**:
  - Replace `TestFramework.h` macros with Google Test (`TEST`, `TEST_F`) or Catch2 (`TEST_CASE`, `SECTION`)
  - Wrap `Board::Instance()` setup/teardown in a test fixture so every test starts with a clean board
  - Integrate with Visual Studio Test Explorer or CTest for CI
- **Files**: `StratEngine/Tests/TestFramework.h`, all `*Tests.h` files
- **Note**: Deliberately deferred — larger undertaking than individual test additions

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
- **Implementation**: Track N best moves per depth

#### ⚪ Singular Extensions
- **Estimate**: 2-3 weeks
- **Description**: Extend search when one move is much better
- **Impact**: Better tactical play, 5-10% improvement
- **Complexity**: Requires re-search verification

#### ⚪ Internal Iterative Deepening (IID)
- **Estimate**: 1 week
- **Description**: Search shallower when no hash move available
- **Impact**: Better move ordering, 3-5% improvement

### Evaluation

#### ⚪ Add King Safety Evaluation
- **Estimate**: 2-3 weeks
- **Components**:
  - Pawn shield scoring
  - King tropism (enemy pieces near king)
  - Open files near king
  - Attack squares around king
- **Tuning**: Requires large test suite

#### ⚪ Add Mobility Evaluation
- **Estimate**: 1-2 weeks
- **Description**: Score based on legal moves available
- **Note**: Expensive to compute, maybe cache

#### ⚪ Tapered Evaluation (Midgame → Endgame)
- **Estimate**: 1 week
- **Description**: Interpolate between midgame and endgame scores
- **Implementation**: Based on material count

#### ⚪ Neural Network Evaluation (NNUE)
- **Estimate**: 3-6 months
- **Impact**: Potentially +300 Elo
- **Complexity**: Very high - requires training infrastructure
- **Reference**: Stockfish NNUE implementation
- **Prerequisite**: Strong classical evaluation baseline

### Advanced Features

#### ⚪ Syzygy Tablebase Support
- **Estimate**: 2-3 weeks
- **Description**: Perfect play in 7-piece endgames
- **Library**: [Fathom](https://github.com/jdart1/Fathom)
- **Impact**: Better endgame play, draw avoidance

#### ⚪ Opening Book
- **Estimate**: 1 week (integration), varies (book creation)
- **Format**: Polyglot or custom
- **Source**: Master games database
- **Features**: Win/draw/loss statistics, popularity weighting

#### ⚪ Time Management
- **Estimate**: 2-3 days
- **Description**: Better time allocation based on position complexity
- **Factors**:
  - Material balance (more time when behind)
  - Move number (more time in middle game)
  - Search stability (more time when scores jump)

### Infrastructure

#### ⚪ UCI Protocol Support
- **Estimate**: 1-2 weeks
- **Prerequisite**: MoveFormatter (`ToUCI` serialisation + `FromUCI` parsing for the
  `position ... moves` command)
- **Description**: Universal Chess Interface for GUI compatibility
- **GUIs**: Arena, ChessBase, Fritz
- **Commands**: `uci`, `isready`, `position`, `go`, `stop`

#### ⚪ Cross-Platform Build System
- **Estimate**: 3-5 days
- **Current**: MSVC on Windows
- **Target**: CMake for Windows/Linux/Mac
- **CI**: GitHub Actions for automated builds

---

## Completed Work (Feb 2026)

### ✅ Major Fixes
- Fixed iterative deepening timeout move selection bug
- Fixed score=0 acceptance on interrupted searches  
- Fixed PV table/move mismatch issue
- Fixed mate emergency infinite loop
- Fixed mate-found not stopping iteration
- Removed false-positive score-swing rejections (Case 5b)

### ✅ Refactoring
- Extracted `assess_iteration_quality()` helper method
- Extracted `should_stop_early()` helper method
- Extracted `handle_empty_move_emergency()` helper method
- Introduced `SearchResult` struct for clean interface
- Added `SearchTuning` struct for runtime parameters
- Improved log levels (debug/info/critical)

### ✅ Code Quality
- Added comprehensive diagnostic logging
- Improved method organization (80-line main loop)
- Documented all helper methods
- Added detailed inline comments

### ✅ Correctness Fixes (Feb 22, 2026)
- **Threefold / Twofold Repetition Rule** — implemented and fully debugged:
  - BUG-1: `push_position()` now called after `ChangePlayer()` in both `DoMove()` branches (hash ordering)
  - BUG-2: `is_repetition()` loop start corrected from `history_size-4` to `history_size-3` (parity)
  - BUG-3: Twofold-in-search branch was unreachable; replaced dead `history_ply >= root_ply` condition with `i >= history_size - ply`
  - BUG-4: Castling rights and en-passant square changes now included in Zobrist hash (`update_zobrist_castling`, `update_zobrist_ep`)
  - `zobrist_hash_` widened from `unsigned int` (32-bit) to `uint64_t`
  - `updateThreefoldRep` made private and renamed to `update_threefold_rep`
  - Dead `mark_irreversible()` method removed
  - 8 regression tests in `StratEngine/Tests/RepetitionTests.h` (TC1–TC7, TC9) — all passing

### ✅ Infrastructure
- **Transposition Table Thread-Safety**: Per-bucket `shared_mutex` locks for concurrent access
  - Probes use `shared_lock` for concurrent reads
  - Stores use `unique_lock` for exclusive writes  
  - Global `shared_mutex` for resize/clear operations
  - Atomic counters for O(1) diagnostics
- **Optimized TT Clearing**: Only clears on new game (line 83-84 in `AIPerplex.cpp`)
  - Preserves TT across moves for better performance
  - Improves self-play and tournament play by 10-15%
- **Perft Testing Framework**: Complete implementation for move generation verification
  - Implementation: `StratEngine/Tests/Perft.h/cpp`
  - Test runner: `StratEngine/Tests/PerftRunner.cpp`
  - Test cases: `Tests/perft_test_cases.json` (green/passing)
  - Integrated into main: See `StratChessEvolved.cpp main()`
  - Verifies move generation correctness against known values

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
2. **Low-hanging fruit** - High ROI, low effort (thread_local buffer)
3. **Blocking items** - Required for parallel search (ThreadData)
4. **High-impact perf** - LMR, delta pruning, killer moves
5. **Infrastructure** - Testing, profiling (enables confidence)
6. **Advanced features** - NNUE, tablebases (after solid baseline)

### When to Skip
Avoid these traps:
- ❌ Premature optimization (profile first!)
- ❌ Feature creep (parallel search is the goal)
- ❌ Perfect-is-enemy-of-good (working > elegant)
- ❌ Over-engineering (YAGNI principle)

---

## Next Steps (Recommended Order)

### Week 1: Quick Wins
1. ✅ Re-enable thread_local buffer (30 min) → +20-30%
2. ✅ Archive broken algorithms (1 hour)
3. ✅ Add delta pruning (1 day) → +20-40% qsearch

### Week 2-3: Major Performance
4. ✅ Implement LMR (3-5 days) → +200-300% effective depth
5. ✅ Extract move ordering to MoveSorter (2 days)

### Week 4-5: Parallel Prep
6. ✅ Extract ThreadData structure (2-3 days)
7. ✅ Verify TT thread-safety (1-2 days)
8. ✅ Add killer moves + history (3-4 days) → +15-20%

### Week 6-8: Advanced Features
9. ✅ Implement aspiration windows (2 days) → +10-15%
10. ✅ Add SEE to quiescence (4-5 days) → +10-15%

### Month 3-6: Parallel Search
11. ✅ Implement Lazy SMP (3-4 weeks) → +200-400% with 4-8 threads
12. ✅ Tune and optimize parallel scaling
13. ✅ Extensive testing and regression prevention

---

## Notes

### Prioritization Philosophy
- **Correctness > Performance > Features**
- Fix bugs immediately
- Profile before optimizing
- Maintain clean architecture
- Parallel search is the north star

### Risk Management
- Test extensively after each change
- Keep git history clean for easy rollback
- Save positions from bug reports
- Document decisions in commit messages

### Communication
- Update this roadmap quarterly
- Document completed items in CHANGELOG (TODO)
- Share performance results in commit messages

---

**Document Version**: 1.0  
**Next Review**: May 2026  
**Owner**: Thees
