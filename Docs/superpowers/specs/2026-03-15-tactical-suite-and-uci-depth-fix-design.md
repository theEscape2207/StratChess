# Design Spec: Two-Tier Tactical Suite + UCI Depth/Infinite Fix

**Date:** 2026-03-15
**Status:** Approved for implementation planning
**Scope:** Plan item #4 (full tactical suite) + two latent UCI bugs discovered during review

---

## Goal

1. Expand the tactical test suite from 3 positions to ~35, split across a fast smoke tier and a
   thorough readiness tier — providing a clear pre-UCI signal of engine strength.
2. Fix two related UCI issues found during the depth-constraint review:
   - `go depth N` (no time) incorrectly caps search time at 10 s instead of treating depth as
     the sole constraint.
   - `go infinite` inherits `time_limit_` from the previous move, so the engine stops before
     `stop` arrives.

---

## Design Decisions

### A. Two-tier structure

| Tier | Tag | Depth | Target count | Runtime ceiling |
|---|---|---|---|---|
| Fast smoke | `[tactical]` | 4 | ~10 positions | < 5 s |
| Thorough readiness | `[tactical_full][slow]` | 6 | ~25 positions | < 60 s |

The `[slow]` tag is a new convention: any test expected to take > ~1 s is tagged `[slow]`.
`build.ps1 run-tests` (no argument) is changed to pass `~[slow]` by default, keeping the
default run fast. A new `build.ps1 extended-tests` target runs everything.

### B. File structure — mirrors the perft split

- **`StratChessTests/TacticalTests.cpp`** — fast tier `[tactical]` (existing file, refactored)
  - Migrate 3 existing individual `TEST_CASE` blocks into a single `GENERATE`-driven test.
  - Add ~7 more positions to reach ~10 total.

- **`StratChessTests/TacticalFullTests.cpp`** — slow tier `[tactical_full][slow]` (new file)
  - ~25 positions at depth 6.
  - Added to `StratChessTests.vcxproj` + `.vcxproj.filters`.

- **`StratChessTests/TacticalTestHelpers.h`** — shared header (new, header-only)
  - `TacticalCase` struct
  - `make_tactical_engine(depth)` helper (moved from `TacticalTests.cpp`)

### C. Data shape

```cpp
// TacticalTestHelpers.h
struct TacticalCase {
    const char* label;
    const char* fen;
    eSquare     expected_from;
    eSquare     expected_to;
    unsigned    depth;
};

inline std::unique_ptr<PlayerBase> make_tactical_engine(unsigned depth) {
    auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth);
    AIPerplex::SetVerboseLogging(false);
    ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
    return ai;
}
```

```cpp
// TacticalTests.cpp — position table (TacticalFullTests.cpp has its own equivalent)
static constexpr TacticalCase kFastCases[] = {
    { "mate-in-1: rook back rank", "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1", a1, a8, 4 },
    // ... ~9 more entries
};

// Test body pattern (identical in both files, different tag + table array)
TEST_CASE("Tactical - fast suite", "[tactical]")
{
    auto tc = GENERATE(from_range(kFastCases));

    INFO(tc.label);
    Board::Instance().SetupFromFEN(tc.fen);
    auto ai = make_tactical_engine(tc.depth);
    GameInfo info = Board::Instance().GetGameInfo();
    Move m = ai->GetMove(info);

    REQUIRE(m.from() == tc.expected_from);
    REQUIRE(m.to()   == tc.expected_to);
}
```

`from_range` is the idiomatic Catch2 v3 mechanism for iterating a struct array;
`table<T>` requires tuples and is not appropriate here.
`INFO(tc.label)` surfaces the position label in any failure output without counting as an
assertion. Each generated case appears as a distinct entry in the Catch2 report.

### D. Position selection

**Fast tier (depth 4, ~10 positions)**

| Category | Count | Notes |
|---|---|---|
| Mate-in-1 | 4 | Existing 2 + 2 new (different delivery pieces / patterns) |
| Winning captures | 3 | Existing 1 + 2 new (hanging queen, exchange win) |
| Simple 2-ply tactics | 3 | Knight fork, rook skewer, queen fork — forced win visible in ≤ 2 moves |

**Slow tier (depth 6, ~25 positions)**

| Category | Count | Notes |
|---|---|---|
| Mate-in-2 | 10 | Forced in both lines, no defensive resource escapes |
| WAC tactical patterns | 15 | Forks, pins, discovered attacks, back-rank combos, clearances |

**Selection invariant:** every committed position must have a single best move at the target
depth, verified against the current engine. Positions the engine does not yet solve at the
target depth are excluded from the initial commit and tracked as future additions (engine
readiness indicator, not a fixed-pass-rate assertion).

### E. `build.ps1` changes

```powershell
# New default: run-tests without args excludes [slow]
.\build.ps1 run-tests             # passes ~[slow] — stays < 10 s  ← behaviour change
.\build.ps1 run-tests "[tactical_full]"  # explicit slow-tier run
.\build.ps1 extended-tests        # new target: no tag filter, runs everything
```

**Breaking change acknowledged:** `run-tests` with no argument previously ran all tests.
After this change it excludes `[slow]` tests. Any workflow or CI step relying on bare
`run-tests` for a full run must be updated to `extended-tests`.

**`build.ps1` `ValidateSet` update required:** `extended-tests` must be added to the
`[ValidateSet(...)]` attribute on the `$Command` parameter alongside `main`, `tests`, `all`,
and `run-tests`. This is a one-line change in `build.ps1`.

`extended-tests` is the pre-commit/pre-PR command. A full `extended_test_suite` script is
noted as a future Roadmap item; `extended-tests` is the seed for it.

### F. UCI depth/infinite fix

**Root cause:** `cmd_go()` applies a 10 s fallback time limit whenever no movetime or clock
params are present, including `go depth N`. `go infinite` inherits `time_limit_` from the
previous move, so the engine stops early without a `stop` command.

**Fix — replace the time-config block in `cmd_go()`:**

```cpp
// Before (problematic):
if (p.movetime > 0) {
    ai_->SetTimeLimit(std::chrono::milliseconds(p.movetime));
} else if (p.wtime > 0 || p.btime > 0) {
    ai_->SetClockInfo(remaining, inc, p.movestogo);
} else if (!p.infinite) {
    ai_->SetTimeLimit(std::chrono::seconds(10));  // fires for "go depth N" too — wrong
}

// After (correct):
if (p.movetime > 0) {
    ai_->SetTimeLimit(std::chrono::milliseconds(p.movetime));
} else if (p.wtime > 0 || p.btime > 0) {
    ai_->SetClockInfo(remaining, inc, p.movestogo);
} else if (p.infinite || p.depth > 0) {
    // Pure depth constraint or infinite: give the engine a large time budget.
    // The depth cap in iterative_deepening() is the sole stopping criterion.
    // For go infinite, the UCI 'stop' command is the intended termination.
    ai_->SetTimeLimit(std::chrono::hours(1));
} else {
    // No constraints at all — apply a safe fallback.
    ai_->SetTimeLimit(std::chrono::seconds(10));
}
```

**Why `hours(1)` instead of `max()`:** `std::chrono::milliseconds::max()` risks overflow
in the `>=` comparison inside `should_stop_search()`. 1 hour is safely finite and effectively
infinite for any real game or analysis session.

**Branch order is significant:** `movetime` and `wtime/btime` checks remain first and are
unchanged. The `depth > 0 || infinite` branch is only reached when neither of those applies.
Combinations such as `go wtime X depth N` correctly take the `wtime` path as before.

**`stop_and_join()` must reset `time_limit_`:** After `go infinite`, `time_limit_` is `hours(1)`.
Without a reset, a subsequent bare `go` (no params) would inherit that value and hit the
1-hour fallback instead of the intended 10 s safety fallback. Fix: reset `time_limit_` to its
default (`std::chrono::seconds(15)`) in `stop_and_join()` alongside the existing `max_depth`
reset. This ensures a clean state between moves regardless of the previous `go` variant.

---

## Files Changed

| File | Change |
|---|---|
| `StratChessTests/TacticalTests.cpp` | Refactor: 3 individual TEST_CASEs → 1 GENERATE; add ~7 more positions |
| `StratChessTests/TacticalFullTests.cpp` | New: slow-tier GENERATE test, ~25 positions |
| `StratChessTests/TacticalTestHelpers.h` | New: `TacticalCase` struct + `make_tactical_engine()` |
| `StratChessTests/StratChessTests.vcxproj` | Add `TacticalFullTests.cpp` (`ClCompile`) + `TacticalTestHelpers.h` (`ClInclude`) |
| `StratChessTests/StratChessTests.vcxproj.filters` | Add `<Filter>` entries for both `TacticalFullTests.cpp` and `TacticalTestHelpers.h` |
| `StratEngine/UCIHandler.cpp` | Fix `cmd_go()` time-config block; add `ai_->SetTimeLimit(std::chrono::seconds(15))` to `stop_and_join()` |
| `build.ps1` | Default `run-tests` → `~[slow]`; add `extended-tests` target |

---

## Validation Plan

```powershell
# 1. Build
.\build.ps1 tests

# 2. Fast suite — must complete in < 5 s, all pass
.\build.ps1 run-tests "[tactical]"

# 3. Slow suite — must complete in < 60 s, all pass
.\build.ps1 run-tests "[tactical_full]"

# 4. Full suite via new target
.\build.ps1 extended-tests

# 5. Confirm default run-tests excludes slow
.\build.ps1 run-tests    # should NOT run [tactical_full]; verify by timing + output

# 6. UCI depth fix — smoke test (save as uci_smoke.ps1, run from StratChessEvolved/)
# powershell -ExecutionPolicy Bypass -File .\uci_smoke.ps1
$commands = "uci`nisready`nposition startpos`ngo depth 5`nquit"
$commands | & ..\x64\Release\StratChessEvolved.exe
# Expect: 'bestmove' line appears promptly after depth 5 completes (not cut off at 10 s)
```

---

## Key Correctness Properties

1. `[tactical]` tier always included in default `run-tests`; `[tactical_full]` never is.
2. Every committed tactical position has a verified single best move at its target depth.
3. `go depth N` (no time) terminates at depth N, not at an arbitrary time limit.
4. `go infinite` runs until an explicit `stop` is received (or the 1-hour safety ceiling).
5. `go movetime N` and `go wtime/btime` behaviour is **unchanged** — existing paths untouched.
6. `make_tactical_engine(depth)` in tests never calls `SetClockInfo` → `StartTimer()` uses
   `time_limit_ = 15 s` default, which is never the binding constraint for depth 4/6.
