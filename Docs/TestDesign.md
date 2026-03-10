# StratChessEvolved — Test Design

**Status**: Phase 0 bootstrapped (March 2026)
**Owner**: Thees
**Related**: `Docs/Roadmap.md` — consult before adding tests for a new area

---

## Philosophy

Tests are **protection tools for the roadmap**, not bureaucratic overhead.

- **Fast feedback above all**: the full Catch2 suite must run in < 10 seconds (Release build). Slow tests do not get run.
- **Tests protect the next change, not the last one**: write tests just before touching a component, especially for high-risk roadmap items (LMR, MoveSorter, parallel search).
- **Direction over precision**: for evaluation and search, verify the *direction* and *relative magnitude* of results. Exact centipawn values are fragile and mislead.
- **Perft is the ground truth for move generation**: do not replace it with unit tests — extend it with more positions.

---

## Test Tiers

| Tier | Executable | Tag / command | Max time | Trigger |
|------|-----------|---------------|----------|---------|
| Unit & fast integration | `StratChessTests.exe` | (all), or `[tag]` | < 10 s | Every build |
| Deep perft | `StratChessEvolved.exe perft test` | — | Minutes | Pre-merge |
| Full tactical suite | `StratChessEvolved.exe tactical test` | — | Minutes | Pre-merge |

Run all fast tests at once:
```bash
x64/Release/StratChessTests.exe
```

Run by tag:
```bash
x64/Release/StratChessTests.exe [tt]
x64/Release/StratChessTests.exe [eval]
x64/Release/StratChessTests.exe [tactical]
x64/Release/StratChessTests.exe [repetition]
x64/Release/StratChessTests.exe [moves]
x64/Release/StratChessTests.exe [perft]
x64/Release/StratChessTests.exe [search]
x64/Release/StratChessTests.exe [sort]
```

---

## Coverage Map

| Component | Tag | Status | File |
|-----------|-----|--------|------|
| Move structure & sentinels | `[moves]` | ✅ done | `MoveFieldTests.cpp` |
| Repetition detection | `[repetition]` | ✅ done | `RepetitionTests.cpp` |
| Move generation (perft d1–d4) | `[perft]` | ✅ done | `PerftTests.cpp` |
| Move generation (deep perft d5+) | — | ✅ done | `StratChessEvolved.exe perft test` |
| **MoveFormatter** | `[formatter]` | ✅ Phase 0 | `MoveFormatterTests.cpp` |
| **TranspositionTable** | `[tt]` | ✅ Phase 0 | `TTTests.cpp` |
| **Evaluation (EvalSimple/Complex)** | `[eval]` | ✅ Phase 0 | `EvalTests.cpp` |
| **Search regression (tactical)** | `[tactical]` | ✅ Phase 0 | `TacticalTests.cpp` |
| Search helpers (assess_quality etc.) | `[search]` | ✅ Phase 1 | `SearchTests.cpp` |
| Move ordering (Sort) | `[sort]` | ✅ Phase 1 | `SortTests.cpp` |
| Board DoMove/UndoMove completeness | `[board]` | ⏳ Phase 1 | `BoardTests.cpp` (future) |
| Bitboard helpers | `[bitboard]` | ⏳ Phase 1 | `BitboardTests.cpp` (future) |
| Full tactical suite (WAC/mate-in-N) | — | ⏳ Phase 1 | `StratChessEvolved.exe tactical test` |
| Position class (after Board refactor) | `[position]` | ⏳ Phase 2 | — |
| NPS / performance regression | — | ⏳ Phase 2 | — |

---

## Phase 0 — Bootstrap (March 2026)

### TranspositionTable Tests (`[tt]`)

**File**: `StratChessTests/TTTests.cpp`
**Rationale**: The TT is fully self-contained (no Board dependency). Its correctness affects every depth of search — a silent TT bug could mask wins or produce ghost moves. Tests added now provide a safety net before the TT is stressed by LMR and parallel search.

**Test cases**:
- Store and probe: entry is retrievable by key
- Probe miss: unknown key returns `nullopt`
- Same-key overwrite: second store wins
- Non-mate score: unaffected by normalization at any ply
- Winning mate round-trip: `normalize`/`denormalize` at same ply recovers original value
- Losing mate round-trip: same for negative mate
- Mate score ply adjustment: probe at different ply returns adjusted distance
- `clear()`: removes all entries; subsequent probes return `nullopt`
- `entry_count` increments on new key, does not increment on overwrite
- `pv_count` tracks `PV_NODE` entries correctly
- `clear()` resets both counters to zero

### Evaluation Tests (`[eval]`)

**File**: `StratChessTests/EvalTests.cpp`
**Rationale**: Evaluation has zero tests today. The roadmap plans King Safety and Mobility evaluation — these tests establish a baseline that regressions will break.

**Approach**: `Board::Instance().SetupFromFEN(fen)` then `EvalManager::Create(type)->Evaluate()` — same pattern as RepetitionTests.

**Test cases**:
- Starting position: EvalSimple within ±200 cp (symmetric, tables may not sum to exactly 0)
- Starting position: EvalComplex within ±200 cp
- White extra queen (black has no queen), white to move: EvalSimple > 500 cp
- Black extra queen (white has no queen), black to move: EvalSimple > 500 cp
- White extra queen: EvalComplex > 500 cp
- Both engines agree: both positive when white has material advantage
- EvalComplex doubled pawn penalty: score lower with doubled pawns than without
- EvalComplex rook on 7th: endgame position with rook on 7th scores positively

### Tactical Tests (`[tactical]`)

**File**: `StratChessTests/TacticalTests.cpp`
**Rationale**: Direct regression tests for search correctness. If LMR, aspiration windows, or move ordering changes break tactical play, these catch it fast. Each test uses `AIPerplex` at depth 4 — fast on simple positions (< 100 ms each), finds forced results reliably.

**Approach**: `PlayerBase::Create(AI_PERPLEX, 4)`, `SetEvalEngine(COMPLEX)`, `SetVerboseLogging(false)`, then `GetMove(info)`. Check `m.from()` and `m.to()`.

**Test cases**:
- Mate in 1 (rook delivers back-rank mate): engine plays Ra8# (`6k1/5ppp/8/8/8/8/5PPP/R5K1`)
- Mate in 1 (queen delivers back-rank mate): engine plays Qd8# (`6k1/5ppp/8/8/8/8/3Q4/6K1`)
  — queen slides along the d-file; lands 3 squares from king (cannot be captured)
- Capture hanging rook: engine captures undefended piece (`4k3/8/8/8/8/8/8/2rQK3`)

---

## Phase 1 — Incremental (add when touching that area)

Each item below is a standalone task. Do it when the corresponding feature is being modified — not ahead of time.

### `[search]` — AIPerplex helper unit tests

**Status**: ✅ **Done.** LMR landed in March 2026; all 10 cases passing.
**File**: `StratChessTests/SearchTests.cpp`
**Activation**: `STRAT_ENABLE_TEST_ACCESS` in x64 Debug + Release preprocessor definitions in `StratChessTests.vcxproj`.

Tests for private helper methods exposed via `AIPerlexTestFixture` (friend class):

- `assess_iteration_quality()`: 6 cases — one per `RejectionReason` branch (INCOMPLETE×2, TOO_FEW_NODES, SHORT_PV, SCORE_DROP, MOVE_CHANGED)
- `should_stop_early()`: 2 cases — mate score path; short-PV forced-line path
- `handle_empty_move_emergency()`: 2 cases — mate-detected path (returns false); true-emergency path on a real starting-position board (returns true, sets legal move)

### `[sort]` — Move ordering tests

**Status**: ✅ **Done.** ScoreMoves extracted from pvs() inline loop; 5 test cases, 14 assertions, all passing.
**File**: `StratChessTests/SortTests.cpp`

Verify ordering priority: PV move → hash move → captures (MVV-LVA) → killers → history scores.

### `[board]` — DoMove/UndoMove completeness

**When**: when Move layout Phases 3 & 4 land (removing `MovPiece` and `Content` from Move)
**File**: `StratChessTests/BoardTests.cpp`

- En passant DoMove/UndoMove: captured pawn restored correctly
- Castling DoMove/UndoMove: rook and king both moved and restored
- Promotion move generation: white pawn b7→b8 generates `MoveType::PROMOTION_QUEEN`; capture-promotion c7xb8 generates queen promo with `PieceHelper::IsActual(m.Content)` true (these Move-field checks were in the retired `MoveGeneratorPromotionTests.h` and are not covered by perft)
- Promotion DoMove/UndoMove: pawn replaced by promoted piece, restored on undo
- Zobrist hash: `get_zobrist_hash()` identical before and after a DoMove/UndoMove cycle

### `[bitboard]` — Bitboard helper tests

**When**: opportunistically (low priority)
**File**: `StratChessTests/BitboardTests.cpp`

Basic operations from `defines.h`: set bit, clear bit, popcount, LSB extraction.

### Full tactical suite in main executable

**When**: when evaluation is extended (King Safety, Mobility, Tapered Eval)
**Files**: `StratEngine/Tests/TacticalTestRunner.h/cpp`, `Tests/tactical_test_cases.json`
**Invocation**: `StratChessEvolved.exe tactical test`
**Acceptance**: 90%+ pass rate on included positions

Content:
- WAC (Win At Chess) subset — 25 representative positions
- Mate-in-2 positions — 10 positions
- Endgame K+Q vs K, K+R vs K basic positions

---

## Phase 2 — With Board De-Singletonization

The Board singleton is **incompatible with parallel search** (each thread needs its own Board copy). Removing it is already on the roadmap as a Critical Priority item (see `Roadmap.md`). These test changes come naturally with that refactor.

**Impact on tests**:
- Replace `Board::Instance().SetupFromFEN(fen)` with constructing a Board directly
- All test fixtures updated to pass Board by reference
- `EvalTests.cpp`, `RepetitionTests.cpp`, `PerftTests.cpp` all updated
- New `[position]` tests for the `Position` class once it encapsulates `GameInfo` history
- Performance regression test: NPS baseline stored in a file, warn if > 10% regression across builds

**Migration rule**: new test files added during Phase 2 should be written for the non-singleton Board from the start.

---

## Test Isolation Rules

- Every `TEST_CASE` that uses `Board::Instance()` must call `Board::Instance().SetupFromFEN(fen)` as its first action — Board state is global and carries over between tests if not reset.
- TT tests use a fresh `TranspositionTable(1)` (1 MB) per test — never the AIPerplex-internal TT.
- Tactical tests create a fresh `AIPerplex` per test — this allocates a 256 MB TT, which is acceptable on development machines (virtual memory is lazy-paged).

---

## Regression Protocol

When a bug is found and fixed:
1. Create a minimal reproduction FEN
2. Add a `TEST_CASE` that would have caught it (repetition, perft, eval, or tactical)
3. Include the bug report in the test comment (like the existing BUG-1 through BUG-4 in `RepetitionTests.cpp`)
4. Commit test and fix together

---

## AIPerplex Test Access

To test private search helper methods, `AIPerplex.h` contains a conditional friend declaration:

```cpp
#ifdef STRAT_ENABLE_TEST_ACCESS
    friend class AIPerlexTestFixture;
#endif
```

To activate: add `STRAT_ENABLE_TEST_ACCESS` to the preprocessor definitions for the x64 Debug and Release configurations in `StratChessTests.vcxproj`. This is done when `SearchTests.cpp` is written (Phase 1 — do not enable before then).

The macro is intentionally absent from the production project (`StratChessEvolved.vcxproj`).
