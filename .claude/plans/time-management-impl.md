# Time Management — Clock-Aware Soft/Hard Limits Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the fixed-budget timer with clock-aware time allocation so AIPerplex uses time proportionally based on remaining clock and increment, while keeping all existing algorithms (AIBasic, AIAgent, ABIterative, archived) fully backward-compatible.

**Architecture:** New `Engine::compute_budget()` free function (pure math, testable in isolation) feeds a two-arg `TimeManager::start(soft, hard)` overload; `PlayerAiBase::SetClockInfo()` wires them together; AIPerplex's IDS loop uses a new `should_stop_iteration()` soft-limit gate; node-based polling replaces per-call `chrono::now()` in `pvs()`.

**Tech Stack:** C++20, MSVC /W4 /WX, Catch2 v3, `<chrono>`, `<atomic>`

---

## Key Correctness Properties (verify after every task)

1. `should_stop_search()` semantics **unchanged** — all existing algos unaffected.
2. `compute_budget()` always returns `hard >= soft >= 100 ms`.
3. `SetTimeLimit(ms)` still works — calls `time_manager_.start(ms)` → `start(ms, ms)`.
4. `nodes_since_check_` resets at the top of every `iterative_deepening()` call.
5. Soft-limit extension fires at most once per depth (only when `metrics.move_changed`).

---

## Task 1: Create TimeUtils.h (interface only — TDD step 1)

**Files:**
- Create: `StratEngine/Utils/TimeUtils.h`

**Step 1: Create the header**

```cpp
#pragma once
#include <chrono>

namespace Engine {

/// Computed time budget for a single move search.
/// @invariant hard >= soft >= 100 ms always holds.
struct TimeBudget {
    std::chrono::milliseconds soft;   ///< Target stop time (finish current depth, then stop)
    std::chrono::milliseconds hard;   ///< Emergency cutoff (abort mid-search)
};

/// Compute a soft and hard time budget from clock information.
///
/// @param remaining  Time left on the clock for this side (milliseconds).
/// @param increment  Per-move increment added after the move (milliseconds).
/// @param moves_to_go  Moves remaining in the current time control, or 0 if unknown.
/// @returns TimeBudget with hard >= soft >= 100 ms.
[[nodiscard]] TimeBudget compute_budget(
    std::chrono::milliseconds remaining,
    std::chrono::milliseconds increment,
    int moves_to_go) noexcept;

} // namespace Engine
```

**Step 2: Verify header is well-formed by reading it back — no build step needed yet.**

---

## Task 2: Write TimeManagerTests.cpp — formula tests (TDD: red phase)

**Files:**
- Create: `StratChessTests/TimeManagerTests.cpp`
- Modify: `StratChessTests/StratChessTests.vcxproj`

**Step 1: Create `StratChessTests/TimeManagerTests.cpp`**

```cpp
#include "StdAfx.h"
#include <catch_amalgamated.hpp>
#include "Utils/TimeUtils.h"
#include "Utils/TimeManager.h"
#include <thread>

using namespace std::chrono_literals;

// ============================================================
// Formula tests — deterministic, no sleeps, < 1 ms each
// ============================================================
TEST_CASE("compute_budget: blitz midgame (3+2, 150 s remaining)", "[time_mgr]")
{
    // Example A from design doc
    auto b = Engine::compute_budget(150'000ms, 2'000ms, 0);
    // base = 149950/30 + 1600 ≈ 6598 ms
    REQUIRE(b.soft.count() >= 6'000);
    REQUIRE(b.soft.count() <= 7'500);
    // hard = min(base*3, usable*0.5) ≈ 19800
    REQUIRE(b.hard.count() >= b.soft.count());
    REQUIRE(b.hard.count() <= 75'000);  // must be ≤ usable*0.5
}

TEST_CASE("compute_budget: classical with moves_to_go (60 min, 20 moves left)", "[time_mgr]")
{
    // Example B from design doc
    auto b = Engine::compute_budget(3'600'000ms, 0ms, 20);
    // base = 3599950/20 = 180000 ms = 3 min
    REQUIRE(b.soft.count() >= 179'000);
    REQUIRE(b.soft.count() <= 181'000);
    // hard = min(180000*3, 3599950*0.5) = 540000 ms = 9 min
    REQUIRE(b.hard.count() >= 539'000);
    REQUIRE(b.hard.count() <= 541'000);
}

TEST_CASE("compute_budget: increment-heavy (200 ms remaining, 5 s increment)", "[time_mgr]")
{
    auto b = Engine::compute_budget(200ms, 5'000ms, 0);
    // base = max(150/30 + 4000, ...) — usable is clamped to 100 ms minimum
    // soft must be >= 100 ms (floor clamp)
    REQUIRE(b.soft.count() >= 100);
    REQUIRE(b.hard.count() >= b.soft.count());
}

TEST_CASE("compute_budget: time trouble floor (remaining < overhead)", "[time_mgr]")
{
    // Less than overhead (50 ms) — must not crash or return zero/negative
    auto b = Engine::compute_budget(30ms, 0ms, 0);
    REQUIRE(b.soft.count() >= 100);
    REQUIRE(b.hard.count() >= b.soft.count());
}

TEST_CASE("compute_budget: zero increment, no moves_to_go", "[time_mgr]")
{
    auto b = Engine::compute_budget(60'000ms, 0ms, 0);
    // base = 59950/30 ≈ 1998 ms ≈ 2 s
    REQUIRE(b.soft.count() >= 1'800);
    REQUIRE(b.soft.count() <= 2'200);
    REQUIRE(b.hard.count() >= b.soft.count());
}

TEST_CASE("compute_budget: hard is always >= soft", "[time_mgr]")
{
    // Invariant check across several inputs
    struct Case { int remaining_ms, inc_ms, mtg; };
    for (auto [r, i, m] : std::initializer_list<Case>{
            {5000, 0, 0}, {500, 500, 5}, {100000, 1000, 15}, {50, 0, 0}})
    {
        auto b = Engine::compute_budget(
            std::chrono::milliseconds(r),
            std::chrono::milliseconds(i),
            m);
        REQUIRE(b.hard.count() >= b.soft.count());
        REQUIRE(b.soft.count() >= 100);
    }
}

// ============================================================
// TimeManager timing tests — short sleeps, total < 300 ms
// ============================================================
TEST_CASE("TimeManager: two-arg start — soft fires before hard", "[time_mgr]")
{
    chess::TimeManager tm;
    tm.start(20ms, 60ms);

    REQUIRE_FALSE(tm.should_stop_search());
    REQUIRE_FALSE(tm.should_stop_iteration());

    std::this_thread::sleep_for(25ms);
    // soft should have fired, hard should not yet
    REQUIRE(tm.should_stop_iteration());
    REQUIRE_FALSE(tm.should_stop_search());

    std::this_thread::sleep_for(40ms);
    // now hard fires too
    REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: one-arg start — soft == hard (backward compat)", "[time_mgr]")
{
    chess::TimeManager tm;
    tm.start(20ms);  // existing one-arg overload

    REQUIRE_FALSE(tm.should_stop_search());
    std::this_thread::sleep_for(25ms);
    // both fire at the same time
    REQUIRE(tm.should_stop_iteration());
    REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: stop() fires should_stop_search immediately", "[time_mgr]")
{
    chess::TimeManager tm;
    tm.start(10'000ms, 20'000ms);   // very long budget
    REQUIRE_FALSE(tm.should_stop_search());
    tm.stop();
    REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: elapsed() is monotone", "[time_mgr]")
{
    chess::TimeManager tm;
    tm.start(5'000ms);
    auto t1 = tm.elapsed();
    std::this_thread::sleep_for(10ms);
    auto t2 = tm.elapsed();
    REQUIRE(t2 > t1);
}
```

**Step 2: Add `TimeManagerTests.cpp` to `StratChessTests/StratChessTests.vcxproj`**

In the `<ItemGroup>` that contains the other test `.cpp` files (after `MoveFormatterTests.cpp`), add:
```xml
    <ClCompile Include="TimeManagerTests.cpp" />
```

**Step 3: Build to verify compile failure (TimeUtils.cpp missing)**

```powershell
.\build.ps1 tests
```
Expected: **build error** — `Engine::compute_budget` unresolved external. This confirms the test is wired up and truly tests the new code.

---

## Task 3: Implement TimeUtils.cpp (TDD: green phase for formula tests)

**Files:**
- Create: `StratEngine/Utils/TimeUtils.cpp`
- Modify: `StratChessEvolved/StratChessEvolved.vcxproj` (add ClCompile)
- Modify: `StratChessEvolved/StratChessEvolved.vcxproj.filters` (add ClCompile + ClInclude filter entries)
- Modify: `StratChessTests/StratChessTests.vcxproj` (add ClCompile for TimeUtils.cpp)

**Step 1: Create `StratEngine/Utils/TimeUtils.cpp`**

```cpp
#include "../StdAfx.h"
#include "TimeUtils.h"

namespace Engine {

TimeBudget compute_budget(
    std::chrono::milliseconds remaining,
    std::chrono::milliseconds increment,
    int moves_to_go) noexcept
{
    using ms = std::chrono::milliseconds;

    constexpr ms overhead{ 50 };
    constexpr ms floor_time{ 100 };

    // Clamp usable time to avoid zero or negative division
    const ms usable = std::max(remaining - overhead, floor_time);

    // Horizon: use provided moves_to_go, otherwise assume 30 moves left
    const int horizon = (moves_to_go > 0) ? moves_to_go : 30;

    // Base allocation: spread time evenly + capture most of the increment
    const ms base{
        static_cast<ms::rep>(usable.count() / horizon
            + static_cast<ms::rep>(increment.count() * 0.8))
    };

    const ms soft = std::max(base, floor_time);

    // Hard limit: never burn more than half the remaining clock in one move
    const ms hard_candidate{ static_cast<ms::rep>(soft.count() * 3) };
    const ms hard_cap{ static_cast<ms::rep>(usable.count() / 2) };
    const ms hard = std::max(std::min(hard_candidate, hard_cap), soft);

    return TimeBudget{ soft, hard };
}

} // namespace Engine
```

**Step 2: Add `TimeUtils.cpp` to `StratChessEvolved/StratChessEvolved.vcxproj`**

After the `<ClCompile Include="..\StratEngine\Utils\Logger.cpp" />` line:
```xml
    <ClCompile Include="..\StratEngine\Utils\TimeUtils.cpp" />
```

**Step 3: Add filter entries to `StratChessEvolved/StratChessEvolved.vcxproj.filters`**

After the `Logger.cpp` filter entry (which uses `<Filter>Source Files\Utils</Filter>`):
```xml
    <ClCompile Include="..\StratEngine\Utils\TimeUtils.cpp">
      <Filter>Source Files\Utils</Filter>
    </ClCompile>
```

After the `TimeManager.h` filter entry (which uses `<Filter>Header Files\Util</Filter>`):
```xml
    <ClInclude Include="..\StratEngine\Utils\TimeUtils.h">
      <Filter>Header Files\Util</Filter>
    </ClInclude>
```

**Step 4: Add `TimeUtils.cpp` to `StratChessTests/StratChessTests.vcxproj`**

After `<ClCompile Include="..\StratEngine\Utils\Logger.cpp" />`:
```xml
    <ClCompile Include="..\StratEngine\Utils\TimeUtils.cpp" />
```

**Step 5: Build and run formula tests**

```powershell
.\build.ps1 run-tests "[time_mgr]"
```
Expected: all 6 formula test cases **PASS**. The 4 timing test cases will **FAIL** (missing `should_stop_iteration()` method). That is correct — they are the next red phase.

---

## Task 4: Enhance TimeManager.h (green phase for timing tests)

**Files:**
- Modify: `StratEngine/Utils/TimeManager.h`

**Step 1: Update `TimeManager.h`**

Replace the entire file content with:

```cpp
#pragma once
#include <chrono>
#include <atomic>

namespace chess {

/// Time management for search — tracks both a soft (iteration) and hard (abort) limit.
///
/// Soft limit (`should_stop_iteration`): finish the current depth, then stop.
/// Hard limit (`should_stop_search`):    abort the search immediately mid-node.
///
/// Backward compat: the one-arg `start(allocated)` sets soft == hard (no early-stop).
class TimeManager {
public:
    /// Start timer with separate soft and hard budgets.
    /// @param soft  Target stop time: stop after current depth completes.
    /// @param hard  Emergency cutoff: abort mid-search when exceeded.
    void start(std::chrono::milliseconds soft,
               std::chrono::milliseconds hard) noexcept {
        start_time_    = std::chrono::steady_clock::now();
        soft_limit_    = soft;
        allocated_time_ = hard;
        should_stop_.store(false, std::memory_order_relaxed);
    }

    /// Start timer with a single budget (soft == hard).  Preserves existing behaviour
    /// for all callers that use a fixed time limit via SetTimeLimit().
    void start(std::chrono::milliseconds allocated) noexcept {
        start(allocated, allocated);
    }

    /// Signal immediate stop (e.g. from UCI 'stop' command).
    void stop() noexcept {
        should_stop_.store(true, std::memory_order_relaxed);
    }

    /// Hard limit check — abort the search immediately.
    /// Semantics UNCHANGED from the original single-budget implementation.
    [[nodiscard]] bool should_stop_search() const noexcept {
        if (should_stop_.load(std::memory_order_relaxed))
            return true;
        auto el = std::chrono::steady_clock::now() - start_time_;
        return el >= allocated_time_;
    }

    /// Soft limit check — stop after the current depth completes.
    /// Only meaningful when start(soft, hard) was called with soft < hard.
    [[nodiscard]] bool should_stop_iteration() const noexcept {
        if (should_stop_.load(std::memory_order_relaxed))
            return true;
        auto el = std::chrono::steady_clock::now() - start_time_;
        return el >= soft_limit_;
    }

    [[nodiscard]] auto elapsed() const noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_);
    }

private:
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::milliseconds soft_limit_{ 1000 };
    std::chrono::milliseconds allocated_time_{ 1000 };
    std::atomic<bool> should_stop_{ false };
};

} // namespace chess
```

**Step 2: Build and run all [time_mgr] tests**

```powershell
.\build.ps1 run-tests "[time_mgr]"
```
Expected: all 10 test cases (6 formula + 4 timing) **PASS**.

**Step 3: Run the full test suite to confirm no regressions**

```powershell
.\build.ps1 run-tests
```
Expected: all assertions pass (previously 250; now ~265 with the new [time_mgr] cases).

---

## Task 5: Add SetClockInfo() and nodes_since_check_ to PlayerAiBase

**Files:**
- Modify: `StratEngine/PlayerAI.h`
- Modify: `StratEngine/PlayerAI.cpp`

**Step 1: Update `StratEngine/PlayerAI.h`**

Add `#include "Utils/TimeUtils.h"` after the existing `#include "Utils\TimeManager.h"` line.

Add `SetClockInfo()` in the **public** section, after `SetTimeLimit()`:
```cpp
/// Call once per move when clock information is available (e.g. from UCI 'go' command).
/// Computes soft and hard budgets via Engine::compute_budget() and arms the timer.
/// The next call to StartTimer() inside GetMove() will use these budgets.
/// @param remaining    Time remaining on the clock for this side.
/// @param increment    Per-move increment.
/// @param moves_to_go  Moves remaining in time control, or 0 if unknown.
void SetClockInfo(std::chrono::milliseconds remaining,
                  std::chrono::milliseconds increment,
                  int moves_to_go = 0) noexcept;
```

Add `nodes_since_check_` in the **protected** member variable section, after `stop_search_`:
```cpp
// Node-based time-check counter — reset at the start of each search; incremented in pvs().
// Checking time every 1024 nodes amortises the cost of chrono::now() calls.
int64_t nodes_since_check_{ 0 };
```

**Step 2: Update `StratEngine/PlayerAI.cpp`**

Add `#include "Utils/TimeUtils.h"` at the top of the include block (after `#include "StdAfx.h"`).

Add the implementation of `SetClockInfo()` (place it near `SetTimeLimit`, e.g. after `StopTimerAndAdjustVars()`):
```cpp
void PlayerAiBase::SetClockInfo(std::chrono::milliseconds remaining,
                                std::chrono::milliseconds increment,
                                int moves_to_go) noexcept
{
    auto [soft, hard] = Engine::compute_budget(remaining, increment, moves_to_go);
    time_limit_ = hard;     // keep time_limit_ in sync so log output stays meaningful
    time_manager_.start(soft, hard);
}
```

**Note:** `time_manager_.start(soft, hard)` is called here, which arms the timer immediately. The `StartTimer()` call inside `GetMove()` will call `time_manager_.start(time_limit_)` (one-arg, sets soft==hard). That would overwrite the clock-aware budgets! This is why `SetClockInfo()` must be called **after** `GetMove()` returns for the previous move and **before** `GetMove()` is called for the next move, OR — better — we need `StartTimer()` to honour a flag.

**Correct approach:** Store the soft limit separately and use it in `StartTimer()`:

Add a private member to `PlayerAI.h`:
```cpp
bool clock_info_set_{ false };   // true when SetClockInfo() has been called for this move
std::chrono::milliseconds soft_limit_{ std::chrono::seconds(15) };
```

Update `StartTimer()` in `PlayerAI.h`:
```cpp
void StartTimer()
{
    _startingTime = std::chrono::high_resolution_clock::now();
    nodes_since_check_ = 0;
    if (clock_info_set_) {
        // SetClockInfo() was called — budgets already set on time_manager_; don't overwrite
        clock_info_set_ = false;   // reset for next move
    } else {
        time_manager_.start(time_limit_);
    }
    stop_search_.store(false, std::memory_order_relaxed);
}
```

Update `SetClockInfo()` in `PlayerAI.cpp`:
```cpp
void PlayerAiBase::SetClockInfo(std::chrono::milliseconds remaining,
                                std::chrono::milliseconds increment,
                                int moves_to_go) noexcept
{
    auto [soft, hard] = Engine::compute_budget(remaining, increment, moves_to_go);
    time_limit_ = hard;               // keep in sync for log output
    soft_limit_ = soft;
    time_manager_.start(soft, hard);  // arm now; StartTimer() will skip re-arming
    clock_info_set_ = true;
}
```

**Step 3: Build to verify no compilation errors**

```powershell
.\build.ps1 main
```
Expected: **0 errors, 0 warnings**.

**Step 4: Run full test suite**

```powershell
.\build.ps1 run-tests
```
Expected: all assertions pass.

---

## Task 6: AIPerplex.cpp — soft-limit gate + node-based polling

**Files:**
- Modify: `StratEngine/AIPerplex.h` (add `nodes_since_check_` private member)
- Modify: `StratEngine/AIPerplex.cpp`

**Note:** `nodes_since_check_` is defined in `PlayerAiBase` (added in Task 5). AIPerplex inherits it — no additional member needed. We just use `nodes_since_check_` directly in AIPerplex's methods.

**Step 1: Add node-based polling to `pvs()` in `AIPerplex.cpp`**

Find the current first lines of `pvs()`:
```cpp
int AIPerplex::pvs(int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt, PVTable& pv_table)
{
	// Check time and stop signal
	if (ShouldStopSearch()) {
		return GameValues::Draw;
```

Replace with:
```cpp
int AIPerplex::pvs(int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt, PVTable& pv_table)
{
	// Node-based time polling: check every 1024 nodes to amortise chrono::now() cost.
	// The bitmask (& 1023) is equivalent to (% 1024) but branch-free.
	if ((++nodes_since_check_ & 1023) == 0) {
		if (ShouldStopSearch())
			return GameValues::Draw;
	}
```

**Step 2: Add soft-limit gate in `iterative_deepening()` in `AIPerplex.cpp`**

Reset `nodes_since_check_` at the top of `iterative_deepening()`, before the for loop:
```cpp
SearchResult AIPerplex::iterative_deepening(int max_depth, TranspositionTable& tt, PVTable& pv_table) {
	SearchState state;
	nodes_since_check_ = 0;   // reset node counter for this search

	clear_killers();
	// ... rest of function unchanged
```

Find the `ACCEPT_AND_CONTINUE` case inside the `switch(decision)` block:
```cpp
		case IterationDecision::ACCEPT_AND_CONTINUE:
			state.best_move = metrics.current_move;
			state.best_score = metrics.current_score;
			state.depth_completed = depth;
			state.nodes_at_completed_depth = metrics.nodes_searched;
			state.last_iteration_move = metrics.current_move;
			state.search_was_stable = !metrics.move_changed;

			log_completed_iteration(metrics, pv_table);

			continue_iteration = !should_stop_early(depth, metrics.current_score, metrics.pv_length);
			break;
```

Replace with:
```cpp
		case IterationDecision::ACCEPT_AND_CONTINUE:
			state.best_move = metrics.current_move;
			state.best_score = metrics.current_score;
			state.depth_completed = depth;
			state.nodes_at_completed_depth = metrics.nodes_searched;
			state.last_iteration_move = metrics.current_move;
			state.search_was_stable = !metrics.move_changed;

			log_completed_iteration(metrics, pv_table);

			// Soft limit gate: stop after this depth if the allocated time budget
			// is consumed.  Exception: if the best move just changed, allow one
			// more depth to verify the new move (the hard limit will cut it off).
			if (time_manager_.should_stop_iteration() && !metrics.move_changed) {
				continue_iteration = false;
				break;
			}

			continue_iteration = !should_stop_early(depth, metrics.current_score, metrics.pv_length);
			break;
```

**Step 3: Build**

```powershell
.\build.ps1 main
```
Expected: **0 errors, 0 warnings**.

**Step 4: Run full test suite**

```powershell
.\build.ps1 run-tests
```
Expected: all assertions pass.

**Step 5: Run the tactical suite**

```
cd C:\Users\thees\source\repos\StratChessEvolved\.claude\worktrees\reverent-nightingale\Tests
..\x64\Release\StratChessEvolved.exe tactical test
```
Expected: 8/8 PASS (≥ 90%).

---

## Task 7: Update Docs

**Files:**
- Modify: `Docs/TestDesign.md`
- Modify: `Docs/Roadmap.md`

**Step 1: Add `[time_mgr]` row to the coverage map in `Docs/TestDesign.md`**

In the Coverage Map table, after the `[sort]` row:
```markdown
| Time management (TimeManager + compute_budget) | `[time_mgr]` | ✅ Phase 1 | `TimeManagerTests.cpp` |
```

**Step 2: Add Phase 1 section for `[time_mgr]` in `Docs/TestDesign.md`**

After the `[board]` section, add:

```markdown
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
```

**Step 3: Update Roadmap.md completed section**

Append to the `## ✅ Completed Work` section at the bottom:

```markdown
### Time Management: Clock-Aware Soft/Hard Limits (March 2026)
- `Engine::compute_budget(remaining, increment, moves_to_go)` free function in `TimeUtils.h/cpp`
  — pure math, independently testable; formula: `soft = usable/horizon + inc*0.8`, `hard = min(soft*3, usable*0.5)`
- `TimeManager` gains two-arg `start(soft, hard)` + `should_stop_iteration()` (soft limit check)
- `PlayerAiBase::SetClockInfo()` public method computes budget and arms timer; `clock_info_set_` flag prevents StartTimer() from overwriting the clock-aware budgets
- `AIPerplex::iterative_deepening()` soft-limit gate: stop after depth if `should_stop_iteration()` and move was stable; allow one extra depth if best move just changed
- Node-based time polling in `pvs()`: check every 1,024 nodes instead of every call (amortises `chrono::now()` overhead)
- `[time_mgr]` test tag: 10 assertions (6 formula, 4 timing), total < 300 ms
- Plan: `.claude/plans/time-management-clock-aware.md`
```

---

## Task 8: Self-play validation and commit

**Step 1: Self-play validation**

Ensure `StratChessEvolved/logs/` directory exists. Edit `StratChessEvolved/game_settings.json` to use `AIPerplex` vs `AIPerplex` (both `"type": 6`). Ensure the FEN is set to the starting position (`"fen": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"`).

Run from `StratChessEvolved/` directory:
```
cd StratChessEvolved
..\x64\Release\StratChessEvolved.exe
```

Inspect `logs/multisink.txt`. Verify that `GetMove complete: move=..., time=Xms` lines show reasonable per-move times (not 15,000 ms flat — the old fixed budget). With no `SetClockInfo()` call (game mode uses `SetTimeLimit`), the behavior should be identical to before (time_limit_ = 15 s, soft == hard = 15 s). This confirms backward compat.

**Step 2: Restore `game_settings.json` to starting position FEN** (per CLAUDE.md: always verify FEN before committing).

**Step 3: Commit**

```bash
git add StratEngine/Utils/TimeUtils.h StratEngine/Utils/TimeUtils.cpp \
        StratEngine/Utils/TimeManager.h \
        StratEngine/PlayerAI.h StratEngine/PlayerAI.cpp \
        StratEngine/AIPerplex.cpp \
        StratChessEvolved/StratChessEvolved.vcxproj \
        StratChessEvolved/StratChessEvolved.vcxproj.filters \
        StratChessTests/StratChessTests.vcxproj \
        StratChessTests/TimeManagerTests.cpp \
        Docs/TestDesign.md \
        Docs/Roadmap.md
git commit -m "feat: clock-aware time management — soft/hard limits + node polling

Adds Engine::compute_budget() (TimeUtils.h/cpp), two-arg TimeManager::start(),
should_stop_iteration() soft-limit gate, PlayerAiBase::SetClockInfo(), and
node-based time polling in AIPerplex::pvs() (every 1024 nodes).

Fully backward-compatible: SetTimeLimit() and all existing algo callers
(AIBasic, AIAgent, ABIterative) are unaffected. 10 new [time_mgr] assertions,
total suite < 300 ms.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```
