# Aspiration Windows — Implementation Plan

## Goal
Add aspiration windows to `AIPerplex::iterative_deepening()` so that depths 2+ search a
narrow window around the previous depth's score instead of always starting at full
`(-50000, +50000)`. Expected: ~10–15% speedup in stable positions by reducing the
effective branching factor at the root.

## Scope Limits
- Only touches `AIPerplex::iterative_deepening()` dispatch and adds two new private methods
- No changes to `pvs()`, `quiescence()`, move ordering, TT, or any other player class

## Design Decisions
| Decision | Choice | Rationale |
|---|---|---|
| Window strategy | Gradual widening (double on miss) | Better than one-step-to-INF when position is stable; cheaper than a full retry per miss |
| First iteration | depth 1 always uses full window | No reliable seed at depth 1; `state.best_score` is 0 (default) which would wrongly bias the window |
| Retry + timeout | Retries count as part of same depth attempt | `m_SearchCount` accumulates across retries; `metrics.interrupted` naturally reflects any timeout |
| Kill-switch | `tuning_.aspiration_enabled = false` | Allows byte-identical regression comparison without a rebuild |
| Architecture | Option B: extracted helper method | Matches the project pattern; testable from `[search]` unit tests |

## Files Changed
- `StratEngine/AIPerplex.h` — 3 new `SearchTuning` fields, 2 new method declarations
- `StratEngine/AIPerplex.cpp` — dispatch change in `iterative_deepening()` (+6 lines replacing 8), new `search_with_aspiration()` (~40 lines), new `log_aspiration_retry()` (~15 lines)
- `.claude/plans/aspiration-windows.md` — this file

## Step-by-Step Changes

### 1. `AIPerplex.h` — `SearchTuning` additions
After `delta_pruning_margin`, add:
```cpp
int aspiration_initial_delta = 50;  // centipawns; initial half-width on each side
int aspiration_max_retries   = 4;   // widen iterations before opening full window
bool aspiration_enabled      = true; // runtime kill-switch for regression testing
```
Update trailing comment to `// Future: Add LMR thresholds, etc.`

### 2. `AIPerplex.h` — Method declarations
In `// SEARCH METHODS` section, before `pvs()`:
```cpp
int search_with_aspiration(int depth, int seed_score, TranspositionTable& tt, PVTable& pv_table);
```
In `// Logging helpers` section:
```cpp
void log_aspiration_retry(int depth, int retry, int score, int alpha, int beta, bool fail_low) const;
```

### 3. `AIPerplex.cpp` — `iterative_deepening()` dispatch
Replace the 8-line `pvs()` call block (lines 134-141) with:
```cpp
int currentBestScore;
if (state.depth_completed == 0 || !tuning_.aspiration_enabled) {
    currentBestScore = pvs(depth, -GameValues::Search_Init, GameValues::Search_Init,
        0, true, tt, pv_table);
} else {
    currentBestScore = search_with_aspiration(depth, state.best_score, tt, pv_table);
}
```

### 4. `AIPerplex.cpp` — `search_with_aspiration()` definition
Algorithm:
- Set `delta = aspiration_initial_delta` (50 cp)
- `alpha = max(seed - delta, -Search_Init)`, `beta = min(seed + delta, +Search_Init)`
- Loop:
  - Check `ShouldStopSearch()` before and after each `pvs()` call
  - If in-window (`score > alpha && score < beta`) → return score
  - If max retries hit → one final full-window `pvs()`, return
  - Fail-low: `delta *= 2; alpha = max(seed - delta, -Search_Init)`
  - Fail-high: `delta *= 2; beta = min(seed + delta, +Search_Init)`

### 5. `AIPerplex.cpp` — `log_aspiration_retry()` definition
Debug-level log: direction (FAIL-LOW/FAIL-HIGH), retry count, score, current window.

## Validation Plan
```
.\build.ps1                          # must build clean
.\build.ps1 run-tests                # all tags must pass: [repetition][perft][tt][eval][tactical]
# Kill-switch test: set tuning_.aspiration_enabled=false via tuning() accessor,
#   verify metrics.nodes_searched matches pre-change baseline at same depth
cd Tests && ../x64/Release/StratChessEvolved.exe perft test  # node counts must be identical
# Self-play: type-6 vs type-6 in game_settings.json, ~5 games, no crashes
# Enable verbose logging: confirm ASPIRATION log lines appear in aiperplex.log
# Reset game_settings.json FEN to starting position before committing
```

## Key Correctness Properties
1. **No behaviour change when disabled**: `aspiration_enabled=false` or `depth_completed==0` → `pvs()` receives `(-Search_Init, +Search_Init)`, byte-identical to pre-change.
2. **Unified node count**: `m_SearchCount` accumulates across all retries; `metrics.nodes_searched` reflects total work for quality checks.
3. **PV preservation**: each `pvs()` call in the retry loop starts with `pv_table.clear_ply(0)`; final in-window search's PV is left in `pv_table`.
4. **`alpha < beta` invariant**: initial delta 50 cp > 0; geometric doubling with `Search_Init` clamp prevents crossing; `aspiration_max_retries=4` fallback terminates the loop.
5. **Mate scores**: a mate return causes a fail-high; after widening `beta` to `+Search_Init` the retry finds the mate again and is in-window.
6. **Thread safety**: no new shared state; all touched members (`m_SearchCount`, `pv_table`, per-bucket-locked `_tt`) have the same access pattern as before.
