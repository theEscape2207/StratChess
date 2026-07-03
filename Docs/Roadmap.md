# StratChess Engine Roadmap

**Last Updated**: July 2, 2026
**Timeframe**: Next 6 months (through parallel search implementation)
**Current Version**: AIPerplex 2.0

---

## Overview

This roadmap organizes development tasks by priority and category. Items are drawn from recent debugging sessions, 
performance analysis, and architectural reviews conducted February–March 2026.

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

## 📌 Near-Term Sequence (decided July 2026)

Agreed ordering after the issue #66 post-mortem (see Completed Work). The #66 process gap
(tactical suite not run by any gate) is closed; the other motivator was coverage *breadth*
(was: 8 gated positions), since closed by step 1 below.

1. **Tactical suite expansion** (~1 day) — the scoped near-term slice of "Create Automated
   Test Suite" below: added a WAC subset + mate-in-2/3 set to
   `Tests/tactical_test_cases.json`. Runner, JSON format, and Pre-PR gate already existed —
   pure content work at 25–35 ms/position. Endgame/ECMGCP components stay deferred.
   — ✅ done (July 2026): suite grew 8 → 31 positions (WAC mate-in-2/3/4 + non-mate
   tactical wins), 31/31 passing; mate categories now require 100% pass (unit-tested);
   `tactical test [filename]` staging support added.
2. **Extract ThreadData Structure** (🔴 Critical) — validate the PR #67 way: perft
   equivalence + byte-identical self-play node counts. For deterministic refactors that is a
   *stronger* check than any tactical suite (detects any behavioral drift, not just drift
   that flips a test position), so ThreadData did not need to wait for broader suites.
   — ✅ done (July 2026): `ThreadData` struct passed through the whole search call chain,
   search runs on a thread-local `Board` copy; byte-identical node counts vs. pre-refactor
   baseline across a full fixed-depth self-play game (see Completed Work below).
3. **Before Lazy SMP**: establish the ELO baseline (below) while the engine is still
   deterministic, and revisit the deferred automated-suite scope. Once threads race on a
   shared TT, node-count equivalence stops being available — tactical suites + strength
   measurement become the primary correctness signal.
   — ELO baseline half ✅ done (July 2026): `Run-EloMatch.ps1` + `Docs/EloLog.md`, sanity
   baseline at ±0 (identical builds, pooled 1000 games). The deferred-suite scope revisit
   (endgame tablebase positions, BT2630/ECMGCP) remains open.

---

## 🔴 Critical Priority (Before Parallel Search)

### Refactoring

None open — **Extract ThreadData Structure** completed July 2026 (see Completed Work below).
With De-Singleton Board (PR #67) also done, both refactoring blockers for parallel search are
cleared; what remains before Lazy SMP is step 3 of the Near-Term Sequence above (ELO baseline
+ deferred-suite scope revisit).

---

## 🟡 High Priority (Significant Wins)

### Performance

#### 🟡 Magic Bitboards (PEXT) for Sliding-Piece Attacks
- **Estimate**: 2-3 days
- **Plan**: `.claude/plans/magic-bitboards-sliding-piece-attacks.md`
- **Impact**: Removes the `ROTATED90`/`ROTATED45R`/`ROTATED45L` auxiliary bitboards that
  `Board::add_piece`/`remove_piece` must keep in sync on every `DoMove`/`UndoMove` (3 extra
  writes + mutual-exclusion asserts per piece placement), and collapses `GetTowerBitboard`/
  `GetBishopBitboard` from 2 table lookups + OR to a single `_pext_u64`-indexed lookup each.
- **Approach**: BMI2 PEXT ("fancy magic") attack tables, generated `constexpr` at compile
  time (same style as the existing rotated-bitboard tables in `defines.h`) — no runtime
  init step. Requires enabling `/arch:AVX2` codegen project-wide (both `.vcxproj` files),
  which raises the binary's minimum CPU baseline to Haswell+ (2013+) or Zen3+ (2020+); PEXT
  is emulated in slow microcode on older AMD (Zen/Zen+/Zen2) — confirmed acceptable since
  this is a personal dev-only x64 build, not distributed to unknown hardware.
- **Also benefits**: Shrinks `Board`'s bitboard array by 3 elements and removes one class of
  `test_bitboards()` invariant failure — relevant context for the completed **De-Singleton Board**
  work (see Completed Work below) and the upcoming **Extract ThreadData Structure** item
  (fewer bitboards to carry per thread-local `Board` copy once Lazy SMP lands).
- **Validation**: perft equivalence (depth 1-4 fast tier + full `perft_test_cases.json`),
  `[tactical]`/`[tactical_full]`, nodes-per-second benchmark before/after (see plan for
  full validation plan).

### Infrastructure

#### 🟡 ELO Baseline Measurement — ✅ done (July 2026)
- **Purpose** (unchanged): tactical suites verify correctness only — a change can pass 100%
  of tactical tests and still lose 30 ELO; this item catches that class of regression
- **Delivered**: `Scripts\Run-EloMatch.ps1` (fastchess v1.8.0 match runner, committed
  250-opening book at `Tests/openings/openings-250.pgn`, pinned reference tag
  `elo-reference-v1`, 10+0.1 TC, color-swapped pairs, adjudication) + `Docs/EloLog.md`
  (pinned setup record, interpretation guide, append-only measurement history)
- **Sanity baseline**: identical builds measure −1.4 ELO pooled over 1000 games — the
  instrument has no directional bias; per-500-game batch noise is ±25 ELO at this draw ratio
- **To re-measure after a search/eval change**: build the candidate, run the script
  (≈1 h unattended per 500-game batch), read `Docs/EloLog.md` before interpreting
- **Found along the way**: >MAX_PLY UCI replay crash (fixed + `[uci]` regression test;
  see Completed Work)

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
- **Dependency**: Extract ThreadData Structure — ✅ satisfied (July 2026), so the `std::mdspan`
  history-table slice is actionable any time (after the separate `stdcpplatest` bump commit)
- **Secondary dependency**: De-Singleton Board — ✅ satisfied (PR #67), so the `std::expected`
  slice is actionable any time
- **Plan**: `.claude/plans/cpp23-upgrade.md`
- **Not sequenced**: deliberately kept out of the Near-Term Sequence — ride-along slices by
  design, and nothing in Lazy SMP needs C++23
- **Language Standard Change**: `stdcpp20` → `stdcpplatest` in both `.vcxproj` files (x64 configurations)
- **Validation hygiene**: land the `stdcpplatest` bump as its own commit (bump + verify
  byte-identical self-play node counts), separate from the ThreadData refactor PR — a global
  compiler-standard change inside a node-count-validated refactor would make any drift
  unattributable
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
- **Estimate**: 2-3 days full scope; ~1 day for the near-term slice
- **Status**: Partially in place — exe tactical suite is gated in `Validate-PrePR.ps1`
  Step 3 since PR #69; QFORK-001 mirrored as Catch2 `[tactical]` case
- ✅ **Near-term slice done (July 2026)**: WAC mate-in-2/3/4 + non-mate tactical-win
  batches added to `Tests/tactical_test_cases.json`, 8 → 31 positions, 31/31 passing.
  Candidates verified via a staging file (`Tests/tactical_staging.json`, transient) +
  `Scripts/verify_mate_key.py` (ground-truth mate confirmation) before merging into the
  gated file. See `Docs/TestDesign.md` for the full position list and drop list.
- **Deferred**: BT2630/ECMGCP tactical sets, endgame tablebase positions — revisit before
  Lazy SMP and/or after evaluation work (king safety, mobility) gives them something to catch
- **Ongoing**: fold in regression positions from each bug fix as they occur
- **Acceptance**: Pass 90%+ tactical tests overall, 100% mate tests (now enforced by
  `evaluate_results()` + `[suite_policy]` unit tests, not just a manual target)

---

## ⚪ Long-term Ideas (Research & Experimental)

### Parallel Search

#### ⚪ Implement Lazy SMP
- **Estimate**: 3-4 weeks
- **Prerequisite**: ThreadData refactoring complete — ✅ satisfied (July 2026); remaining
  pre-work is the ELO baseline (step 3 of the Near-Term Sequence)
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

### Refactoring: De-Singleton Board (PR #67, July 2026)
- `Board::Instance()` singleton accessor removed entirely; `Board` is now an ordinary constructible, copyable value type (public default ctor, `explicit Board(const std::string& fen)` convenience ctor, defaulted copy/move)
- `MoveGenerator` (`ComputeLegalMoves`, `ComputeCaptures`, `GetAttackBoard` + private helpers) and `EvalManager::Evaluate` now take an explicit `const Board&` parameter instead of reaching for the singleton
- `PlayerBase::Create()` and every player constructor (`PlayerAiBase`, `AIPerplex`, `AIBasic`, `AIAgent`, `ABIterative`, `PlayerHuman`) take `Board&` by injection; `PlayerAiBase`'s now-meaningless default constructor removed
- `Game` and `UciHandler` each own their own `Board board_` member (declared before the players/AI that hold a reference into it); `Config`'s FEN/board-setup methods take an explicit `Board&`
- All Catch2 test files construct their own local `Board` — no more shared global board state between `TEST_CASE`s; three dead legacy test headers retired (`Unittests.h`, `RepetitionTests.h`, `Perft_unittests.h`, unused since the March 2026 Catch2 migration)
- `zobrist::initialize()` changed from "runs on every Board construction" to a thread-safe run-once magic static, so two boards built from the same FEN are guaranteed to hash identically — required for a future shared TT across per-thread boards
- `GetBitBoards()` changed to a `const` accessor returning `std::span<const BITBOARD>` (every caller was already read-only), which is what unlocked `const Board&` parameters on `MoveGenerator`/`EvalManager`
- Validated throughout: perft 640/640 with identical node counts at every phase, self-play reproducing byte-identical node counts vs. the pre-refactor baseline (fully deterministic — no behavioral change), self-play through both `PlayerAI`/`PlayerBase`-derived hierarchies (AIPerplex and AIAgent) after the base-class changes in the player-injection phase
- New `[board_instance]` test tag (`BoardInstanceTests.cpp`) covers instance independence, copy-then-diverge semantics, and cross-instance zobrist identity
- Plan: `.claude/plans/de-singleton-board.md` (7 phases, each independently built/tested/committed)
- **Unblocks**: Extract ThreadData Structure (see Critical Priority above), and the "with De-Singleton Board" C++23 item (`std::expected` in `FENParser::ParseFEN`)

### Correctness: NMP Single-Piece Zugzwang Guard (issue #66, July 2026)
- QFORK-001 (`8/8/8/3r4/4k3/8/8/3QK3 w`, KQ vs KR) regressed to 7/8 on the exe tactical suite when null-move pruning landed (PR #55): the zugzwang guard only refused NMP for a side with *zero* non-pawn material, so the side with a lone rook could "pass", hiding the domination/zugzwang rook win
- `should_try_null_move()` now requires ≥ 2 non-pawn pieces for the side to move (`std::popcount`); single-piece endgames (K+R, K+Q, K+minor ± pawns) are exactly the zugzwang-prone class and their subtrees are cheap, so the lost pruning is negligible
- Verification gap closed: the exe tactical suite (never run by any automated gate — how the 7/8 went unnoticed) now runs in `Validate-PrePR.ps1` Step 3, and QFORK-001 is mirrored as a Catch2 `[tactical]` case (pre-commit + CI); new `[search]` unit tests cover the guard branch
- Future NMP enhancement (not needed for this fix): verification search — on a null-move fail-high at high depth, re-search at reduced depth without the null to confirm; generalizes zugzwang safety beyond material heuristics
- Plan: `.claude/plans/nmp-single-piece-zugzwang-guard.md`

### Refactoring: Extract ThreadData Structure (July 2026)
- All per-search mutable state used by `AIPerplex` now lives in a single `ThreadData` struct
  (`StratEngine/ThreadData.h`): thread-local `Board` copy, node counter, `PVTable`, `GameInfo`
  sequence, killers, history, null-move flags — plus the maintenance methods that operate on them
- `ThreadData&` is passed explicitly as the **first** parameter through the whole search call
  chain (`iterative_deepening` → `search_with_aspiration` → `pvs` → `quiescence` and helpers);
  the `TranspositionTable` stays a separate explicit parameter because it remains *shared*
  across threads under Lazy SMP — every call site documents the shared-vs-local split
- The search runs entirely on `td.board` (copy-assigned from the game board per `GetMove()`);
  the only remaining side effect on the real board — root game state (mate/stalemate/draw) —
  is propagated back explicitly after the search returns
- `PlayerAiBase::StopTimerAndAdjustVars(size_t node_count)` takes the node count explicitly;
  legacy AIs (`AIBasic`/`AIAgent`/`ABIterative`) pass `m_SearchCount` and are otherwise untouched
- Time control (`time_manager_`, stop flags), `SearchTuning`, and the TT stay OUT of ThreadData
  by design (control plane / read-only / shared); noted for Lazy SMP: `nodes_since_check_` and
  the static `m_TotalTime`/`m_TotalCount` perf stats will need per-thread treatment
- Validated: byte-identical move/score/depth/nodes sequence across a full 137-move fixed-depth
  self-play game vs. the pre-refactor baseline; deep perft 640/640; all Catch2 tiers +
  exe tactical suite 31/31 (`Validate-PrePR.ps1` full gate)
- Plan: `.claude/plans/extract-threaddata-structure.md` (3 steps, each independently
  built/tested/committed)
- **Unblocks**: Lazy SMP (helper thread = another `ThreadData` + same search functions), and
  the "with ThreadData extraction" C++23 slice (`std::mdspan` history table)

### Infrastructure: ELO Baseline Measurement (July 2026)
- `Scripts\Run-EloMatch.ps1`: one-command differential strength measurement — candidate build
  vs. pinned reference (`elo-reference-v1` tag), fastchess v1.8.0, 250 committed openings
  (color-swapped pairs), 10+0.1 TC, adjudication, per-engine working dirs; auto-rebuilds the
  cached reference exe from its tag via a temp worktree on cache miss; appends every result
  to `Docs/EloLog.md` and exits non-zero on illegal-move/disconnect/stall losses (harness
  failures, never strength data)
- Sanity baseline established: identical builds (SHA256-verified) pooled −1.4 ELO over
  2×500 games — no instrument bias; measured per-batch noise ±25 ELO at this draw ratio
  (both batches individually hit opposite 2σ edges — pool before acting on edge results)
- **The smoke match found a real crash on first contact with a match runner**: games longer
  than MAX_PLY (256) plies overflowed the ply-indexed history arrays during UCI `position`
  replay (Release access violation) — `cmd_position` reset the undo cursor only after the
  whole replay loop. Fixed by per-move `ResetSearchDepth()` (matching `Game.cpp`); issue #53
  follow-up; 300-ply regression test via new `UciHandlerTestFixture` (`[uci]`)
- Plan: `.claude/plans/elo-baseline-measurement.md`; full setup/interpretation: `Docs/EloLog.md`
- Deliberately out of scope: SPRT gating (fastchess supports it — adopt when wanted),
  absolute ELO vs third-party engines, CI integration

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

**Document Version**: 1.3
**Next Review**: October 2026
**Owner**: Thees
