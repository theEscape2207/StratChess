# GetMove SearchLimits Refactor (Design + Plan)

**Status**: design approved 2026-07-03; implementation not started — resumable in any session.
**Prerequisite for**: Lazy SMP (`.claude/plans/lazy-smp.md`) — land this first.
**Roadmap item**: 🟢 "GetMove TimeControl Refactor" (scope expanded by decision below).

## Goal

Make every `GetMove()` call self-contained: the caller passes all per-move search
constraints (clock, movetime, depth, infinite) in one `SearchLimits` argument instead of
mutating AI state via `SetClockInfo()` / per-move `SetTimeLimit()` / `SetMaxDepth()` calls
before invoking the search. Eliminates the temporal coupling (`clock_info_set_` flag) and
removes the pre-call ordering contract that a Lazy SMP search thread could violate.

**Scope limits**: no behavior change — identical budgets and identical search decisions for
identical inputs. No search/eval changes. Legacy AIs (`AIBasic`/`AIAgent`/`ABIterative`) and
`PlayerHuman` only change signature, not behavior.

## Design Decisions (approved 2026-07-03)

1. **Explicit parameter, not GameInfo field.** The Roadmap sketched `GameInfo` gaining an
   `optional<TimeControl>`; rejected because `GameInfo` is copied into `info_seq` at every
   search ply (dead bytes per node) and board state ≠ search budget. Instead:
   `Move GetMove(GameInfo& info, const SearchLimits& limits)`.
2. **Unify ALL go constraints**, not just clock. `UCIHandler::cmd_go` currently pushes
   movetime/depth/infinite through `SetTimeLimit()`/`SetMaxDepth()` — the same temporal
   coupling. One struct carries everything; `cmd_go` mutates no AI state.
3. **Back-compat overload.** Non-virtual `PlayerBase::GetMove(GameInfo&)` forwards
   `SearchLimits{}` (= "use configured defaults") so all existing tests and game-mode call
   sites compile and behave unchanged. Avoids default-argument-on-virtual pitfalls.
4. **`SetClockInfo()` and `clock_info_set_` are deleted**, along with the `StartTimer()`
   branch that honored the pre-armed flag. `SetTimeLimit`/`SetMaxDepth` survive as
   config-time setters only (`ucinewgame` defaults; and the depth still seeds
   `PlayerBase::Create`).
5. **`game_settings.json` exposes `search_limits` directly** (added 2026-07-04): the
   per-player `max_depth` and `time_limit` fields migrate into a `search_limits` block
   whose vocabulary maps 1:1 onto the struct — `max_depth` → `depth`, `time_limit` →
   `movetime` (semantics unchanged: depth cap + soft==hard budget, exactly what
   `SetTimeLimit` produces today). Game mode builds one `SearchLimits` per player from
   config and passes it on every `GetMove(info, limits)` call — game mode dogfoods the
   new path instead of the defaults overload. Legacy keys (`max_depth`/`time_limit` at
   player level) are honored as a fallback with a deprecation warning logged, so stale
   local copies keep working; the committed file is migrated in the same PR.

   ```json
   "white": {
     "type": 6,
     "eval": 2,
     /* search_limits: per-move search constraints, mirrors StratEngine/SearchLimits.h.
        All fields optional: depth (cap), movetime (ms, soft==hard budget),
        infinite (bool), clock { remaining, increment, moves_to_go } */
     "search_limits": {
       "depth": 15,
       "movetime": 15000
     },
     "search_tuning": { ... }
   }
   ```

## The struct

New header `StratEngine/SearchLimits.h` (pure value type, no dependencies beyond `<chrono>`
and `<optional>`):

```cpp
struct ClockInfo {
    std::chrono::milliseconds remaining{ 0 };
    std::chrono::milliseconds increment{ 0 };
    int moves_to_go = 0;               // 0 = unknown
};

// All per-move search constraints a caller can express. Default-constructed
// SearchLimits{} means "use the engine's configured defaults" (time_limit_, max_depth_).
struct SearchLimits {
    std::optional<ClockInfo> clock;                        // UCI: wtime/btime/winc/binc/movestogo
    std::optional<std::chrono::milliseconds> movetime;     // UCI: movetime
    std::optional<int> depth;                              // UCI: depth (per-call cap)
    bool infinite = false;                                 // UCI: infinite

    static SearchLimits from_clock(std::chrono::milliseconds remaining,
                                   std::chrono::milliseconds increment,
                                   int moves_to_go) noexcept;
    static SearchLimits fixed_time(std::chrono::milliseconds movetime) noexcept;
    static SearchLimits fixed_depth(int depth) noexcept;
    static SearchLimits infinite_search() noexcept;
};
```

## Resolution semantics (must match today's behavior exactly)

A **pure function** (same pattern as `Engine::compute_budget`, unit-testable without a
player object) plus a thin protected applier on `PlayerAiBase` that replaces the current
`StartTimer()` flag dance:

```cpp
// SearchLimits.h — ClockInfo/SearchLimits are global (interface types, like Move/Board);
// the resolver lives in namespace Engine beside compute_budget.
namespace Engine {
struct ResolvedLimits {
    TimeBudget budget;           // soft/hard, from TimeUtils.h
    unsigned effective_depth;
};
[[nodiscard]] ResolvedLimits resolve_limits(const SearchLimits& limits,
                                            std::chrono::milliseconds default_time,
                                            unsigned default_depth) noexcept;
} // namespace Engine

// PlayerAiBase (protected) — arms time_manager_ with the resolved budget and returns
// the effective depth; also does what StartTimer() does today (reset _startingTime,
// nodes_since_check_, stop flag).
unsigned ApplyLimits(const SearchLimits& limits);
```

| limits state | timer arming | effective depth |
|---|---|---|
| `clock` set | `compute_budget(remaining, increment, moves_to_go)` → `time_manager_.start(soft, hard)` | `depth.value_or(max_depth_)` |
| `movetime` set | `time_manager_.start(movetime)` (soft == hard) | `depth.value_or(max_depth_)` |
| `infinite` | `time_manager_.start(1h)` | `depth.value_or(50)` — mirrors today's `cmd_go` |
| `depth` only | `time_manager_.start(1h)` (depth cap is the sole stop) | `*depth` |
| empty (defaults) | `time_manager_.start(time_limit_)` | `max_depth_` |

Precedence when several fields are set (defensive; UCI never sends conflicting combos
except `depth` alongside time fields): `movetime` > `clock` for timing; `depth` always caps
depth. `_startingTime`, `nodes_since_check_` reset and `stop_search_` clearing stay exactly
as in today's `StartTimer()`.

`iterative_deepening()` receives the effective depth per call (already a parameter);
`max_depth_` remains the configured default only.

## Files Changed

- Create: `StratEngine/SearchLimits.h` (+ `ClInclude` entries in `StratEngine.vcxproj` / `.filters`)
- `StratEngine/IPlayer.h` / `PlayerBase.h`: virtual signature + forwarding overload
- `StratEngine/PlayerAI.h/.cpp` (`PlayerAiBase`): `ResolveLimits()` helper; delete
  `SetClockInfo`, `clock_info_set_`, and the `StartTimer()` flag branch
- `StratEngine/AIPerplex.h/.cpp`, `AIBasic`, `AIAgent`, `ABIterative`, `PlayerHuman`:
  signature updates (legacy AIs: resolve limits → existing behavior; Human ignores limits)
- `StratEngine/UCIHandler.cpp` (`cmd_go`): build `SearchLimits`, stop mutating AI state
- `StratEngine/Config.h/.cpp`: parse `search_limits` block into a `SearchLimits`
  (legacy `max_depth`/`time_limit` fallback + deprecation warning)
- `StratEngine/Game.cpp`: `SetPlayerParams` keeps per-player `SearchLimits` from config;
  the game loop passes it on every `GetMove(info, limits)` call
- `StratChessEvolved/game_settings.json`: migrate both players to the `search_limits` block
- `StratChessTests`: new `[limits]` unit tests; existing tests unchanged via overload

## Key Correctness Properties

1. Behavior-inert: for every input `cmd_go` handles today, the armed soft/hard budgets and
   effective depth are identical to the current `SetClockInfo`/`SetTimeLimit`/`SetMaxDepth`
   path (unit-tested by comparing against `compute_budget` directly).
2. Game mode via the `search_limits` config block ≡ today's setter path: identical depth
   cap and budget, fixed-depth self-play node counts byte-identical pre/post refactor.
   `GetMove(info)` (no limits) likewise ≡ today's defaults path.
3. No pre-call ordering contract remains: two consecutive `GetMove` calls with different
   limits are fully independent.
4. `PlayerHuman::GetMove` ignores limits; legacy AIs honor depth/movetime identically to
   today's setter path.

## Validation Plan

- `.\build.ps1 run-tests "[limits]"` — new resolution unit tests (each row of the table above)
- `.\build.ps1 run-tests` + `extended-tests` — full tiers
- Fixed-depth self-play: byte-identical node counts vs pre-refactor baseline (same method
  as the ThreadData refactor, see `.claude/plans/extract-threaddata-structure.md`)
- **AIAgent self-play** (`"type": 3` both sides): this refactor changes `PlayerAiBase` /
  `PlayerBase` base classes, so legacy-AI regression coverage is mandatory per CLAUDE.md —
  a completed game with plausible moves and no crash/hang, plus the legacy AIs' depth and
  movetime resolution verified against their pre-refactor behavior
- UCI smoke: piped session exercising `go wtime/btime`, `go movetime`, `go depth`,
  `go infinite` + `stop`
- `Scripts\Validate-PrePR.ps1` full gate before PR

## Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Estimate**: 1–1.5 days including validation.

### Global constraints (bind every task)

- MSVC Level4 + `/WX` on x64 — any new warning is a build error; `static_cast<>` for
  intentional narrowing; never `#pragma warning(disable)`. C++20.
- Work in a fresh worktree branched from `origin/main`; pre-commit hook runs FEN check +
  fast tests on every commit.
- Behavior-inert throughout: after every task the engine must produce identical search
  behavior for identical inputs (validated at Task 5; don't defer surprises).
- Names fixed by this design: `ClockInfo`, `SearchLimits`, `ResolvedLimits`,
  `resolve_limits()`, `PlayerAiBase::ApplyLimits()`, JSON block `"search_limits"` with
  keys `depth`, `movetime`, `infinite`, `clock{remaining,increment,moves_to_go}`.

### Task 1: `SearchLimits.h` + pure `resolve_limits()` (TDD)

**Files**: create `StratEngine/SearchLimits.h` and `StratEngine/SearchLimits.cpp`
(+ `ClInclude`/`ClCompile` in `StratEngine.vcxproj` and matching `.filters` entries);
test file `StratChessTests/SearchLimitsTests.cpp` (+ vcxproj/filters), tag `[limits]`.

- [ ] Write failing tests first: one per resolution-table row, asserting against
  `Engine::compute_budget` for the clock row —
  `resolve_limits(SearchLimits::from_clock(60s, 1s, 40), 15s, 15)` yields exactly
  `compute_budget(60'000ms, 1'000ms, 40)` and depth 15;
  `fixed_time(5s)` → soft==hard==5s, depth 15; `fixed_depth(9)` → 1h/1h, depth 9;
  `infinite_search()` → 1h/1h, depth 50; `SearchLimits{}` → 15s/15s (the passed
  default_time), depth 15 (default_depth); precedence: movetime+depth set → movetime
  budget, that depth; clock+depth → clock budget, that depth.
- [ ] Run `.\build.ps1 run-tests "[limits]"` — expect compile failure/red.
- [ ] Implement the struct (exactly the fields/factories from "The struct" above) and:

```cpp
ResolvedLimits resolve_limits(const SearchLimits& limits,
                              std::chrono::milliseconds default_time,
                              unsigned default_depth) noexcept
{
    using namespace std::chrono_literals;
    ResolvedLimits r{};
    r.effective_depth = limits.depth ? static_cast<unsigned>(*limits.depth)
                                     : (limits.infinite ? 50u : default_depth);
    if (limits.movetime) {                       // movetime wins over clock (defensive)
        r.budget = { *limits.movetime, *limits.movetime };
    } else if (limits.clock) {
        r.budget = Engine::compute_budget(limits.clock->remaining,
                                          limits.clock->increment,
                                          limits.clock->moves_to_go);
    } else if (limits.infinite || limits.depth) {
        r.budget = { std::chrono::hours(1), std::chrono::hours(1) };
    } else {
        r.budget = { default_time, default_time };
    }
    return r;
}
```

  (Check `Engine::TimeBudget`'s actual member names in `Utils/TimeUtils.h` before writing
  the aggregate initializers.)
- [ ] `.\build.ps1 run-tests "[limits]"` green; commit
  (`SearchLimits: unified per-move search constraints + pure resolver`).

### Task 2: interface change + `ApplyLimits` + delete `SetClockInfo`

**Files**: `IPlayer.h`, `PlayerBase.h`, `PlayerAI.h/.cpp`, `PlayerHuman.h/.cpp`,
`AIBasic.h/.cpp`, `AIAgent.h/.cpp`, `ABIterative.h/.cpp`, `AIPerplex.h/.cpp`,
`Archived/AITrans.h/.cpp`, `Archived/ABIterTrans.h/.cpp` (archived files compile — update
signatures there too).

- [ ] `IPlayer.h:13` → `virtual Move GetMove(_Inout_ GameInfo& info, const SearchLimits& limits) = 0;`
  and add a non-virtual forwarder on `PlayerBase`:
  `Move GetMove(_Inout_ GameInfo& info) { return GetMove(info, SearchLimits{}); }`
- [ ] Update every override's signature (all files above). `PlayerHuman` ignores `limits`
  (`[[maybe_unused]]` not needed — just omit the param name). Legacy AIs + AIPerplex:
  first line of `GetMove` becomes `const unsigned effective_depth = ApplyLimits(limits);`
  replacing their `StartTimer()` call.
- [ ] `PlayerAiBase::ApplyLimits` (protected, in `PlayerAI.h/.cpp`):

```cpp
unsigned PlayerAiBase::ApplyLimits(const SearchLimits& limits)
{
    const auto r = Engine::resolve_limits(limits, time_limit_, max_depth_);
    _startingTime = std::chrono::high_resolution_clock::now();
    nodes_since_check_ = 0;
    time_manager_.start(r.budget.soft, r.budget.hard);
    stop_search_.store(false, std::memory_order_relaxed);
    return r.effective_depth;
}
```

- [ ] Delete `SetClockInfo` (decl `PlayerAI.h:34-36`, impl `PlayerAI.cpp:176`),
  `clock_info_set_` (`PlayerAI.h:254`), and the `StartTimer()` flag branch
  (`PlayerAI.h:92-104`). Keep `StartTimer()` itself only if legacy AIs still call it for
  non-limits paths — expected outcome: nothing calls it; delete it.
- [ ] AIPerplex `GetMove`: `iterative_deepening(td_, static_cast<int>(effective_depth), *_tt)`.
- [ ] Legacy AIs: wherever they read `max_depth_` as the search bound inside `GetMove`,
  use `effective_depth` instead (grep each `GetMove` body; behavior identical when limits
  are empty).
- [ ] Build both projects + full fast tier: `.\build.ps1 all` then `run-tests`. Commit.

### Task 3: UCI — `cmd_go` builds `SearchLimits`

**Files**: `StratEngine/UCIHandler.cpp` (`cmd_go`, lines ~111–141; defaults reset ~189).

- [ ] Replace the setter cascade with (preserving today's exact values, including the 10 s
  no-constraint fallback and `UCI_DEFAULT_DEPTH`/50 depth defaults):

```cpp
SearchLimits limits;
if (p.movetime > 0)              limits.movetime = std::chrono::milliseconds(p.movetime);
else if (p.wtime > 0 || p.btime > 0)
    limits.clock = ClockInfo{ std::chrono::milliseconds(white ? p.wtime : p.btime),
                              std::chrono::milliseconds(white ? p.winc  : p.binc),
                              p.movestogo };
else if (!p.infinite && p.depth <= 0)
    limits.movetime = std::chrono::seconds(10);          // today's fallback
limits.infinite = p.infinite;
limits.depth = (p.depth > 0) ? std::optional<int>(p.depth)
                             : std::optional<int>(p.infinite ? 50 : static_cast<int>(UCI_DEFAULT_DEPTH));
```

  then pass `limits` through to the search call (`GetMove(info, limits)` on whatever
  thread `cmd_go` dispatches to). Remove the `SetTimeLimit`/`SetMaxDepth`/`SetClockInfo`
  calls from `cmd_go`; the `ucinewgame`/constructor defaults (lines ~189–190) stay.
- [ ] UCI smoke (foreground pipe, from repo root):
  `(printf "uci\nisready\nposition startpos\ngo movetime 500\n"; sleep 2) | ./x64/Release/StratChessEvolved.exe`
  → one `bestmove` in ~0.5 s; repeat for `go depth 6`, `go wtime 60000 btime 60000 winc 1000 binc 1000`,
  and `go infinite` + `stop`. Commit.

### Task 4: `game_settings.json` migration

**Files**: `StratEngine/Config.h/.cpp`, `StratEngine/Game.cpp` (`SetPlayerParams`,
game-loop `GetMove` call site), `StratChessEvolved/game_settings.json`.

- [ ] `Config::PlayerConfig`: replace `depth`/`time_limit_ms` with
  `SearchLimits search_limits;` (plus keep `unsigned depth` — still needed by
  `PlayerBase::Create`; fill it from `search_limits.depth.value_or(DEFAULT_DEPTH)`).
  Parse the `"search_limits"` block (all keys optional). If the block is absent and
  legacy `"max_depth"`/`"time_limit"` keys exist, map them
  (`max_depth`→`depth`, `time_limit`→`movetime`) and log one deprecation warning via
  `spdlog::default_logger()->warn`.
- [ ] `Game`: store each player's `SearchLimits` beside `m_pPlayers` (e.g.
  `std::array<SearchLimits, 2> player_limits_`) in `SetPlayerParams`; the game loop's
  `GetMove(info)` call becomes `GetMove(info, player_limits_[color])`. Remove the
  `SetTimeLimit` call at `Game.cpp:186` (superseded); `SetMaxDepth` via `Create` stays.
- [ ] Migrate the committed `game_settings.json`: both players get
  `"search_limits": { "depth": 15, "movetime": 15000 }`; delete the old
  `max_depth`/`time_limit` lines; keep the explanatory comments (nlohmann accepts them);
  FEN must remain the starting position.
- [ ] Verify: run a short game-mode session (`Start-Process ... -ArgumentList "game"`,
  kill after ~20 s) — moves logged, no deprecation warning with the migrated file; then
  temporarily test the legacy-key fallback with a scratch copy (warning appears, same
  behavior). Commit.

### Task 5: validation + PR

- [ ] Byte-identical node counts: fixed-depth self-play (both `"type": 6`,
  `search_limits.depth` only, generous movetime) vs a pre-refactor baseline run of the
  same config — compare the `GetMove complete: ... nodes=` sequence in
  `logs/multisink.txt` (method: `.claude/plans/extract-threaddata-structure.md`).
- [ ] AIAgent self-play (`"type": 3` both sides, mandatory — base classes changed):
  completes/plays plausibly for 60 s, no crash.
- [ ] `Validate-PrePR.ps1` full gate; sync `origin/main`; PR (Why/Summary/Test plan/Notes
  body, template manually applied). Update the Roadmap item to ✅ with a one-line delta
  (explicit-parameter + JSON migration instead of the GameInfo-field sketch).
