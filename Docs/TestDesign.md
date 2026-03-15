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
x64/Release/StratChessTests.exe [time_mgr]
```

The `[tactical_full]` suite is tagged `[slow]` and excluded from the default `~[slow]` run. Use `extended-tests` to include it:
```powershell
.\build.ps1 extended-tests          # all tests including [slow]
.\build.ps1 run-tests "[tactical_full]"  # slow tier only
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
| **Search regression (slow tier)** | `[tactical_full][slow]` | ✅ Phase 0 | `TacticalFullTests.cpp` |
| Search helpers (assess_quality etc.) | `[search]` | ✅ Phase 1 | `SearchTests.cpp` |
| Move ordering (Sort) | `[sort]` | ✅ Phase 1 | `SortTests.cpp` |
| Board DoMove/UndoMove completeness | `[board]` | ✅ Phase 1 | `BoardTests.cpp` |
| Time management (TimeManager + compute_budget) | `[time_mgr]` | ✅ Phase 1 | `TimeManagerTests.cpp` |
| Bitboard helpers | `[bitboard]` | ⏳ Phase 1 | `BitboardTests.cpp` (future) |
| UCI command loop | `[uci]` | ✅ validated via pipe test | `StratChessEvolved.exe uci` (pipe smoke test) |
| Full tactical suite (WAC/mate-in-N) | — | ✅ Phase 1 | `StratChessEvolved.exe tactical test` |
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

### Slow Tactical Tier (`[tactical_full][slow]`)

**File**: `StratChessTests/TacticalFullTests.cpp`
**Rationale**: Deeper regression coverage at depth 6 (~1.8 s total). Excluded from the default `run-tests` loop (`~[slow]`) to keep the fast feedback cycle under 5 s. Run via `.\build.ps1 extended-tests` or `run-tests "[tactical_full]"` before merging search changes.

**Approach**: Same `GENERATE(from_range(...))` / `TacticalTestHelpers.h` pattern as the fast tier, depth 6.

**Current positions (25)**: 2 mate-in-1 back-rank mates + 23 winning captures across diverse piece types (queen, rook, bishop, knight) and board regions. Every position was individually verified against the engine at depth 6 before committing.

**⚠ Technical debt — position diversity**: the initial 25 positions are dominated by simple hanging captures. Multi-move tactics (mate-in-2, forks, pins, discovered attacks) proved hard to construct with a *unique* best move at depth 6 for this engine's current tactical strength. As the engine improves, replace simpler captures with positions from the WAC-25 set or crafted M2 suites. The selection invariant (unique best move at the target depth, verified before committing) must be maintained.

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

**Status**: ✅ **Done.** Move layout Phases 3 & 4 landed in March 2026; all 5 cases (en passant, castling, promotion generation, promotion round-trip, Zobrist hash cycle) implemented and passing (6 test cases, 33 assertions).
**File**: `StratChessTests/BoardTests.cpp`

- En passant DoMove/UndoMove: captured pawn restored correctly
- Castling DoMove/UndoMove: rook and king both moved and restored
- Promotion move generation: white pawn b7→b8 generates `MoveType::PROMOTION_QUEEN`; capture-promotion c7xb8 generates queen promo with `PieceHelper::IsActual(m.Content)` true (these Move-field checks were in the retired `MoveGeneratorPromotionTests.h` and are not covered by perft)
- Promotion DoMove/UndoMove: pawn replaced by promoted piece, restored on undo
- Zobrist hash: `get_zobrist_hash()` identical before and after a DoMove/UndoMove cycle

### `[time_mgr]` — Time management unit tests

**Status**: ✅ **Done.** Clock-aware time management landed March 2026.
**File**: `StratChessTests/TimeManagerTests.cpp`

**Formula tests (6 cases, no sleep):**
- Blitz midgame: `compute_budget(150000ms, 2000ms, 0)` → soft ≈ 6.6 s, hard ≈ 19.8 s
- Classical with movestogo: `compute_budget(3600000ms, 0ms, 20)` → soft = 3 min, hard = 9 min
- Increment-heavy time trouble: soft ≥ 100 ms (floor clamp enforced)
- Time trouble (remaining < overhead): no crash, soft ≥ 100 ms
- Zero increment, no movestogo: soft ≈ 2 s
- Invariant: `hard >= soft >= 100 ms` across 4 input combinations

**TimeManager timing tests (4 cases, short sleeps ≤ 60 ms each):**
- Two-arg `start(soft=20ms, hard=60ms)`: `should_stop_iteration()` fires after 25 ms, `should_stop_search()` still false; then true after 65 ms
- One-arg `start(20ms)`: both methods fire together (backward compat)
- `stop()` fires `should_stop_search()` immediately
- `elapsed()` increases monotonically

### `[bitboard]` — Bitboard helper tests

**When**: opportunistically (low priority)
**File**: `StratChessTests/BitboardTests.cpp`

Basic operations from `defines.h`: set bit, clear bit, popcount, LSB extraction.

### Full tactical suite in main executable

**Status**: ✅ **Done.** Tactical runner landed March 2026; 8 positions, 8/8 passing (100%).
**Files**: `StratEngine/Tests/TacticalTestRunner.h/cpp`, `Tests/tactical_test_cases.json`
**Invocation**: run from `Tests/` directory: `StratChessEvolved.exe tactical test`
**Acceptance**: 90%+ pass rate on included positions

**Current positions (8)**:
- Mate-in-1 (rook): `6k1/5ppp/8/8/8/8/5PPP/R5K1` → Ra8# (d4)
- Mate-in-1 (queen): `6k1/5ppp/8/8/8/8/3Q4/6K1` → Qd8# (d4)
- Hanging piece (rook): `4k3/8/8/8/8/8/8/2rQK3` → Qxc1 (d4)
- Back rank invasion (d7/d8): `r4rk1/pp3ppp/...` (d5)
- Back rank invasion (d7/d8): `5rk1/p4ppp/...` (d5)
- Queen wins rook (fork/threat): `8/8/8/3r4/4k3/8/8/3QK3` (d4)
- Back rank capture (direct): `3r2k1/p4ppp/...` → Rxd8 (d5)
- Ladder mate or Rb8#: `7k/8/6K1/8/8/8/8/1R5R` (d5)

**Expansion note**: suite can be extended to WAC-25 + mate-in-2 set before UCI. Run time per position is 25–35 ms at depth 4–5 (Release build).

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
