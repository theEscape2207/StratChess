# LMR + [search] Tests

**Date**: 2026-03-10
**Status**: Approved
**Worktree**: romantic-tereshkova
**Roadmap tier**: Tier 2 — Before UCI (item #3)

---

## Goal

Implement Late Move Reductions (LMR) in `pvs()` using the sqrt formula and bundle it with the `[search]` unit tests that lock in the behaviour of the three search helper methods. These ship as one PR.

LMR is the largest single ELO gain available (estimated 2–3× node reduction at depth 5+), which makes every subsequent test and ELO measurement more meaningful.

**Scope limits**: no changes to quiescence search, no changes to evaluation, no changes to aspiration windows.

---

## Design Decisions

### LMR formula — sqrt (industry standard)
`R = max(1, (int)(sqrt(depth-1) * sqrt(si-1)))`, clamped to `depth-1`.
Where `si` is the 0-based move index in the sorted order. Chosen over a fixed `-1` reduction because it scales correctly with depth and move index, matching Stockfish/Ethereal and giving the full ELO benefit.

### Skip conditions (conservative first pass)
LMR is **not** applied when any of the following is true:
- `is_pv_node` — PV nodes are searched at full depth
- `first_child` (si == 0) — handled by the existing full-window branch
- `in_check` — all evasions must be searched carefully
- `isCapture` — captures should not be reduced
- `isPromotion` — promotions should not be reduced
- `isKiller` — killer moves are likely good; search at full depth
- `si < tuning_.lmr_min_move_index` — first N moves in sort order (default: 3)
- `depth < tuning_.lmr_min_depth` — shallow nodes (default: 3)
- `tuning_.lmr_enabled == false` — kill-switch for regression testing

Future candidates (not yet available): passed pawn pushes, moves that give check, counter-moves.

### Re-search pattern (2-step)
If the LMR-reduced search beats alpha, re-search at full `depth-1` null window before accepting the result. The existing PV-node full-window re-search (`value > alpha && is_pv_node`) is preserved unchanged after both branches.

### `lmr_enabled` kill-switch
Follows the `aspiration_enabled` precedent in `SearchTuning`. Allows toggling LMR off at runtime for manual performance comparison via `SimplePerfStats.txt`.

### [search] tests — AIPerlexTestFixture pattern
`STRAT_ENABLE_TEST_ACCESS` activates the existing `friend class AIPerlexTestFixture` declaration in `AIPerplex.h`. The fixture class (defined in `SearchTests.cpp`) re-exports private types as public aliases so `TEST_CASE` functions can use them. This is the same pattern documented in `TestDesign.md §AIPerplex Test Access`.

### Performance measurement — SimplePerfStats.txt
`SimplePerfStats.txt` (written automatically in game mode) already provides per-move node counts. Comparison procedure: run self-play → save file → rebuild with `lmr_enabled = false` → run again → compare node columns. No new infrastructure required for this PR; the Tier 3 NPS baseline is a separate task.

---

## Files Changed

| File | Change |
|------|--------|
| `StratEngine/AIPerplex.h` | Add `lmr_min_depth`, `lmr_min_move_index`, `lmr_enabled` to `SearchTuning`; remove `// Future: Add LMR thresholds` comment |
| `StratEngine/AIPerplex.cpp` | Add `in_check` pre-computation before move loop; replace non-first-child `else` block with LMR logic |
| `StratChessTests/SearchTests.cpp` | New file — `AIPerlexTestFixture` + 10 `[search]` test cases |
| `StratChessTests/StratChessTests.vcxproj` | Add `STRAT_ENABLE_TEST_ACCESS` to preprocessor (Debug + Release x64); add `SearchTests.cpp` as `<ClCompile>` |
| `StratChessTests/StratChessTests.vcxproj.filters` | Add `SearchTests.cpp` under `Source Files\Tests` |
| `Docs/TestDesign.md` | Mark `[search]` row ✅ Phase 1; fill in the Phase 1 `[search]` section body with the 10 test cases |

---

## Step-by-Step Changes

### Step 1 — Enable STRAT_ENABLE_TEST_ACCESS in vcxproj

In `StratChessTests.vcxproj`, for both `x64 Debug` and `x64 Release` `<ClCompile>` property groups, add `STRAT_ENABLE_TEST_ACCESS` to the `<PreprocessorDefinitions>` list.

### Step 2 — Write SearchTests.cpp (TDD: tests before LMR)

Create `StratChessTests/SearchTests.cpp`. Define `AIPerlexTestFixture` first (must be defined before the TEST_CASEs that use it):

```cpp
// SearchTests.cpp — Catch2 [search] tests for AIPerplex private helper methods.
// Requires STRAT_ENABLE_TEST_ACCESS in the test project preprocessor definitions.

#include <catch_amalgamated.hpp>
#include "AIPerplex.h"
#include "Board.h"
#include "PlayerBase.h"
#include "PVTable.h"
#include "defines.h"

class AIPerlexTestFixture {
public:
    // Re-export private types so TEST_CASE functions can reference them
    using RejectionReason  = AIPerplex::RejectionReason;
    using Metrics          = AIPerplex::IterationMetrics;
    using State            = AIPerplex::SearchState;

    std::unique_ptr<PlayerAiBase> ai_owner;
    AIPerplex* ai;

    AIPerlexTestFixture() {
        ai_owner = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, 4);
        ai = static_cast<AIPerplex*>(ai_owner.get());
        AIPerplex::SetVerboseLogging(false);
    }

    RejectionReason assess(const Metrics& m, const State& s) const
        { return ai->assess_iteration_quality(m, s); }

    bool stop_early(int depth, int score, int pv_len) const
        { return ai->should_stop_early(depth, score, pv_len); }

    bool emergency(State& s, PVTable& pv) const
        { return ai->handle_empty_move_emergency(s, pv); }
};
```

**Test cases** (10 total, `[search]` tag):

| # | Target | Scenario | Expected |
|---|--------|----------|----------|
| 1 | `assess_iteration_quality` | `current_move.is_null()` | `INCOMPLETE` |
| 2 | `assess_iteration_quality` | nodes_searched < min_nodes_threshold | `INCOMPLETE` |
| 3 | `assess_iteration_quality` | completion_ratio < min_completion_ratio (prev depth exists) | `TOO_FEW_NODES` |
| 4 | `assess_iteration_quality` | pv_length < depth * min_pv_ratio (prev depth exists) | `SHORT_PV` |
| 5 | `assess_iteration_quality` | score == 0, prev score was 300 (abs > draw threshold) | `SCORE_DROP` |
| 6 | `assess_iteration_quality` | move_changed == true (prev depth exists) | `MOVE_CHANGED` |
| 7 | `should_stop_early` | score >= Mate_Threshold | `true` |
| 8 | `should_stop_early` | depth > 1, pv_length < (depth - depth/2) | `true` |
| 9 | `handle_empty_move_emergency` | best_score >= Mate_Threshold | `false` (mate detected, no move needed) |
| 10 | `handle_empty_move_emergency` | score = 0, starting FEN (legal moves exist) | `true` + state.best_move not null |

Test 10 calls `Board::Instance().SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")` first.

Struct construction reference for assess tests:
```cpp
// Minimal Metrics that passes all checks (use as baseline, then break one field per test)
AIPerlexTestFixture::Metrics m{};
m.depth          = 4;
m.current_move   = /* any non-null move from the move list */;
m.nodes_searched = 5000;     // > min_nodes_threshold (1000)
m.pv_length      = 3;        // >= depth * 0.33
m.interrupted    = true;
m.move_changed   = false;
m.score_delta    = 10;
m.completion_ratio = 0.5;    // > min_completion_ratio (0.10)

AIPerlexTestFixture::State s{};
s.depth_completed          = 3;
s.best_score               = 50;
s.nodes_at_completed_depth = 5000;
s.last_iteration_move      = m.current_move;
```

### Step 3 — Add SearchTests.cpp to vcxproj + filters

In `StratChessTests.vcxproj`: add `<ClCompile Include="SearchTests.cpp" />` after the `BoardTests.cpp` entry.
In `StratChessTests.vcxproj.filters`: add `SearchTests.cpp` under the `Source Files\Tests` filter.

### Step 4 — Build and run [search] tests

Run `.\build.ps1 run-tests "[search]"`. All 10 test cases should pass (they test existing behaviour that already works correctly).

### Step 5 — Add LMR to SearchTuning (AIPerplex.h)

Replace:
```cpp
// Future: Add LMR thresholds, etc.
```
With:
```cpp
int  lmr_min_depth      = 3;    // don't reduce at depth < 3
int  lmr_min_move_index = 3;    // don't reduce the first 3 moves (si 0, 1, 2)
bool lmr_enabled        = true; // kill-switch for regression testing
```

### Step 6 — Implement LMR in pvs() (AIPerplex.cpp)

**Before the move loop**, after `MoveList moveList;` / `bool first_child = true;` setup, add:
```cpp
const bool in_check = m_Board.InCheck();
```

**Replace the non-first-child `else` block** (currently lines ~341-349):

```cpp
} else {
    const bool isCapture   = MoveHelper::IsCapture(move);
    const bool isPromotion = MoveHelper::IsPromote(move);
    const bool isKiller    = (move == killers_[ply][0] || move == killers_[ply][1]);

    const bool applyLMR = tuning_.lmr_enabled
        && !in_check
        && !isCapture
        && !isPromotion
        && !isKiller
        && si >= tuning_.lmr_min_move_index
        && depth >= tuning_.lmr_min_depth;

    if (applyLMR) {
        const int R = std::min(
            std::max(1, static_cast<int>(
                std::sqrt(static_cast<double>(depth - 1)) *
                std::sqrt(static_cast<double>(si - 1)))),
            depth - 1);   // clamp: never reduce to 0 or negative

        value = -pvs(depth - 1 - R, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);

        // Re-search at full depth-1 null window if the reduced search beats alpha
        if (value > alpha && !ShouldStopSearch())
            value = -pvs(depth - 1, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);
    } else {
        value = -pvs(depth - 1, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);
    }

    // Re-search with full window at PV node (unchanged)
    if (value > alpha && is_pv_node)
        value = -pvs(depth - 1, -beta, -alpha, ply + 1, true, tt, pv_table);
}
```

### Step 7 — Update TestDesign.md

- Coverage map: `[search]` row → `✅ Phase 1`, file `SearchTests.cpp`
- Phase 1 `[search]` section body: update from placeholder to the 10 test cases above

### Step 8 — Build and run full test suite

`.\build.ps1 run-tests` — all tests must pass, zero warnings.

### Step 9 — Self-play smoke test + performance comparison

1. Run self-play (AIPerplex vs AIPerplex) to checkmate. Verify game completes and moves look sensible.
2. Save `StratChessEvolved/logs/SimplePerfStats.txt` (nodes-with-LMR baseline).
3. Temporarily set `lmr_enabled = false` in `SearchTuning`, rebuild, run self-play again.
4. Compare per-move node counts. Expect significantly fewer nodes with LMR enabled at depth 5+.
5. Revert the temporary `lmr_enabled = false` change (or confirm the default is `true`).

### Step 10 — Commit

Single commit: `Implement LMR (sqrt formula) + [search] tests`

---

## Validation Plan

```
.\build.ps1 run-tests "[search]"     # 10 new assertions pass
.\build.ps1 run-tests                # all existing assertions still pass (228+)
# Self-play to checkmate (from StratChessEvolved/ dir)
# Compare SimplePerfStats.txt with lmr_enabled=true vs false
```

---

## Key Correctness Properties

- `applyLMR` is evaluated inside the `else` branch — `first_child` is never true when `applyLMR` is checked, so there is no overlap with the full-window first-child path
- `R` is always ≥ 1 and ≤ `depth - 2`, so `depth - 1 - R` ≥ 0 — no zero-depth or negative-depth recursive calls
- The PV re-search block (`if (value > alpha && is_pv_node)`) fires correctly regardless of whether LMR was applied; if LMR already triggered a `depth-1` re-search, the PV re-search still gets the correct wider window
- `store_killer` and `update_history` at the beta-cutoff site are unchanged — LMR does not affect what gets rewarded
- `in_check` is computed once before the loop from the position before any move is made — this is the correct check (are we defending from check?) and matches the quiescence pattern
