# Time Management — Clock-Aware Soft/Hard Limits

**Date**: March 2026
**Status**: Design approved, implementation pending
**Roadmap item**: Tier 2 #5 (pre-UCI)

---

## Goal

Replace the fixed-budget timer with clock-aware time allocation so AIPerplex uses
time proportionally based on remaining clock and increment.  The change must be
fully backward-compatible: all existing algorithms (`AIBasic`, `AIAgent`,
`ABIterative`, archived algos) compile and behave identically.

**Scope limit**: this PR does NOT wire UCI `go wtime/btime` — it builds the
infrastructure that the UCI layer (#6) will call.

**Long-term note**: Option 3 (pass a `TimeControl` struct into `GetMove()`) is the
right eventual architecture. Tracked in `Roadmap.md` as "GetMove TimeControl
refactor" — do after UCI lands.

---

## Design Decisions

### Approach chosen: Layered soft/hard limits in `TimeManager` (Option 1)

Rejected alternatives:
- **Option 2** (AIPerplex-private logic): buries formula inside the AI class;
  not reusable, harder to test.
- **Option 3** (`TimeControl` into `GetMove()`): cleanest long-term API but
  touches every algo's `GetMove()` signature today; deferred post-UCI.

### Key invariants

1. `should_stop_search()` semantics are **unchanged** — hard limit, used by all algos.
2. `should_stop_iteration()` is a **new, additive** method — soft limit, only
   `AIPerplex` calls it.
3. Existing `start(allocated)` one-arg overload is preserved verbatim — callers
   that pass a single budget get `soft == hard` (no early-stop benefit, same as today).
4. `SetTimeLimit(ms)` on `PlayerAiBase` stays untouched — config path unchanged.

### New free function: `TimeUtils::compute_budget()`

Extracting the formula as a free function in `TimeUtils.h` makes it independently
testable without running any search.

```cpp
struct TimeBudget {
    std::chrono::milliseconds soft;
    std::chrono::milliseconds hard;
};

TimeBudget compute_budget(std::chrono::milliseconds remaining,
                          std::chrono::milliseconds increment,
                          int moves_to_go) noexcept;
```

### Allocation formula

```
constexpr overhead = 50 ms
usable     = max(remaining - overhead, 100 ms)   // floor prevents zero/negative
horizon    = moves_to_go > 0 ? moves_to_go : 30
base       = usable / horizon  +  increment * 0.8
soft_limit = max(base, 100 ms)
hard_limit = min(base * 3, usable * 0.5)
hard_limit = max(hard_limit, soft_limit)          // hard is always >= soft
```

Verified examples:

| Scenario | remaining | inc | movestogo | soft | hard |
|----------|-----------|-----|-----------|------|------|
| Blitz 3+2, move 20 | 150,000 ms | 2,000 ms | 0 | 6.6 s | 19.8 s |
| Classical 40/90, 20 left | 3,600,000 ms | 0 | 20 | 3.0 min | 9.0 min |

### Node-based polling in `pvs()`

`ShouldStopSearch()` is currently called on every IDS depth boundary.
A `nodes_since_check_` counter in `AIPerplex` will gate the check to every
**1,024 nodes** inside `pvs()` / quiescence, reducing `chrono::now()` overhead
on deep searches. The member is reset in `StartTimer()`.

### Soft-limit extension in IDS loop

After each completed depth, AIPerplex checks:
```
if (best_move_changed_this_iteration && remaining_since_last_check < soft_limit * 0.5)
    continue;   // extend by up to 50% of soft budget if move changed
```
This prevents cutting off a depth mid-PV-change, a common cause of blunders.

---

## Files Changed

| File | Change |
|------|--------|
| `StratEngine/Utils/TimeUtils.h` | **New** — `TimeBudget` struct + `compute_budget()` declaration |
| `StratEngine/Utils/TimeUtils.cpp` | **New** — formula implementation |
| `StratEngine/Utils/TimeManager.h` | Add `soft_limit_`, two-arg `start()`, `should_stop_iteration()` |
| `StratEngine/PlayerAI.h` | Add `SetClockInfo()`, `nodes_since_check_` |
| `StratEngine/PlayerAI.cpp` | Implement `SetClockInfo()` |
| `StratEngine/AIPerplex.cpp` | IDS soft-limit gate, node-based polling, soft-limit extension |
| `StratChessEvolved/StratChessEvolved.vcxproj` | Wire `TimeUtils.cpp` / `.h` |
| `StratChessEvolved/StratChessEvolved.vcxproj.filters` | Filter entries |
| `StratChessTests/TimeManagerTests.cpp` | **New** — `[time_mgr]` test cases |
| `StratChessTests/StratChessTests.vcxproj` | Wire `TimeManagerTests.cpp` |
| `Docs/TestDesign.md` | Add `[time_mgr]` row and section |
| `Docs/Roadmap.md` | Mark #5 done; add GetMove TimeControl refactor item |

---

## Step-by-Step Changes

### Step 1 — `TimeUtils.h` / `TimeUtils.cpp`

```cpp
// TimeUtils.h
#pragma once
#include <chrono>

namespace Engine {
struct TimeBudget {
    std::chrono::milliseconds soft;
    std::chrono::milliseconds hard;
};

[[nodiscard]] TimeBudget compute_budget(
    std::chrono::milliseconds remaining,
    std::chrono::milliseconds increment,
    int moves_to_go) noexcept;
}
```

`TimeUtils.cpp` implements the formula exactly as documented above.
Include `"../StdAfx.h"` as the first line.

### Step 2 — `TimeManager.h`

Add private member:
```cpp
std::chrono::milliseconds soft_limit_{ std::chrono::seconds(15) };
```

Add overload (existing one-arg stays):
```cpp
void start(std::chrono::milliseconds soft, std::chrono::milliseconds hard) noexcept;
```

Add method:
```cpp
[[nodiscard]] bool should_stop_iteration() const noexcept;
```

The two-arg `start()` sets both `soft_limit_` and `allocated_time_` (hard).
The one-arg `start(ms)` calls `start(ms, ms)` — backward compat.

### Step 3 — `PlayerAI.h` / `PlayerAI.cpp`

In `PlayerAI.h`, add to protected section:
```cpp
int64_t nodes_since_check_{ 0 };
```

Add public method:
```cpp
void SetClockInfo(std::chrono::milliseconds remaining,
                  std::chrono::milliseconds increment,
                  int moves_to_go = 0) noexcept;
```

`PlayerAI.cpp` — implement `SetClockInfo()`:
```cpp
void PlayerAiBase::SetClockInfo(milliseconds remaining, milliseconds increment, int mtg) noexcept {
    auto [soft, hard] = Engine::compute_budget(remaining, increment, mtg);
    time_limit_ = hard;                    // keeps GetMove logging consistent
    time_manager_.start(soft, hard);
}
```

Reset `nodes_since_check_` in `StartTimer()`.

### Step 4 — `AIPerplex.cpp`

**Node-based polling** — replace bare `ShouldStopSearch()` calls inside `pvs()` with:
```cpp
if (++nodes_since_check_ >= 1024) {
    nodes_since_check_ = 0;
    if (ShouldStopSearch()) return SEARCH_ABORTED;
}
```
Keep the depth-boundary `ShouldStopSearch()` call as-is in the IDS outer loop.

**IDS soft-limit gate** — after each completed depth iteration, before starting the
next depth:
```cpp
if (time_manager_.should_stop_iteration()) {
    // Extend if move changed and we've used less than half the soft budget
    if (!best_move_changed || time_manager_.elapsed() >= soft_limit * 0.5)
        break;
}
```

**Note**: `best_move_changed` is already tracked by the existing iteration-quality
logic (`assess_iteration_quality` / `SearchResult`). Wire into that existing flag.

### Step 5 — `TimeManagerTests.cpp`

Tag: `[time_mgr]`
~15 assertions, < 300 ms total:

**Formula tests (no sleep, fully deterministic):**
- Blitz midgame: `compute_budget(150000ms, 2000ms, 0)` → soft≈6600ms, hard≈19800ms
- Classical with movestogo: `compute_budget(3600000ms, 0ms, 20)` → soft=180000ms, hard=540000ms
- Increment-heavy (time trouble): `compute_budget(200ms, 5000ms, 0)` → soft≥100ms (floor), hard≥soft
- Zero increment, no movestogo: `compute_budget(60000ms, 0ms, 0)` → soft≈2000ms
- Floor clamp: `compute_budget(30ms, 0ms, 0)` → soft≥100ms, hard≥soft

**TimeManager timing tests (short sleeps):**
- Two-arg `start(soft=20ms, hard=50ms)`: `should_stop_iteration()` true after 25ms sleep, `should_stop_search()` false; true after 55ms sleep
- One-arg `start(20ms)`: both fire together after 25ms
- `stop()` fires `should_stop_search()` immediately
- `elapsed()` increases monotonically

### Step 6 — vcxproj / filters

Add `TimeUtils.cpp` as `ClCompile`, `TimeUtils.h` as `ClInclude` under `Utils`
filter in both `StratChessEvolved.vcxproj`.

Add `TimeManagerTests.cpp` to `StratChessTests.vcxproj`.

### Step 7 — `Docs/Roadmap.md` + `Docs/TestDesign.md`

`Roadmap.md`: Mark #5 done; add new item under Tier 3:
> **GetMove TimeControl refactor** — Pass `TimeControl` struct (wtime, btime, winc,
> binc, movestogo, is_white) directly into `GetMove()`; eliminates `SetClockInfo()`
> as a pre-call side-effect. Do after UCI lands.

`TestDesign.md`: Add `[time_mgr]` row to coverage map and a Phase 1 section.

---

## Validation Plan

```powershell
# 1. Build
.\build.ps1             # Release|x64, 0 errors 0 warnings

# 2. Unit tests (includes new [time_mgr])
.\build.ps1 run-tests

# 3. Regression — existing tactical suite
cd Tests
..\x64\Release\StratChessEvolved.exe tactical test   # 8/8 ≥ 90%

# 4. Self-play validation
# Edit game_settings.json: both sides type=6, use_clock=true, wtime=30000, btime=30000, inc=2000
# Run from StratChessEvolved/ dir:
cd StratChessEvolved
..\x64\Release\StratChessEvolved.exe
# Inspect logs/multisink.txt: verify GetMove times stay ≤ hard_limit across all moves
```

---

## Key Correctness Properties

1. `should_stop_search()` semantics **unchanged** — all existing algos unaffected.
2. `compute_budget()` always returns `hard >= soft >= 100 ms`.
3. `SetTimeLimit(ms)` still works — `time_manager_.start(ms, ms)` via one-arg overload.
4. `SetClockInfo()` called once per move (in UCI handler, before `GetMove()`).
5. Node counter `nodes_since_check_` is reset in `StartTimer()` — no cross-move state leak.
6. Soft-limit extension fires at most once per depth (bound to `best_move_changed` flag).
7. No changes to `GameInfo`, `GetMove()` signature, or any test fixture setup order.
