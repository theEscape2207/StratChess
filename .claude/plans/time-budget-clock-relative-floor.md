# Time Budget — Clock-Relative Floor

**Issue**: #204 — time budget floor is absolute, so the engine forfeits at increments below 100 ms
**Status**: design approved, implementation pending

---

## Goal

Make `Engine::compute_budget()` incapable of returning a budget larger than the clock it was given.
Today it can, and at any increment below 100 ms that guarantees a forfeit.

**Scope limit**: the static allocation formula only. This does *not* add the complexity-aware layer
proposed in #103, does not retune `base` or the 1.5x hard factor, and does not make `overhead`
configurable (filed as its own issue — see Follow-Up).

---

## The defect

`StratEngine/Utils/TimeUtils.cpp` has three compounding faults, not one:

1. `usable = max(remaining - overhead, floor_time)` invents time that is not on the clock. With
   `remaining = 30 ms` it reports 100 ms of usable time.
2. `soft = max(base, floor_time)` floors every move at 100 ms regardless of what is left. This is
   the issue's headline.
3. `hard = max(min(hard_candidate, hard_cap), soft)` — the outer `max` **defeats** the `usable/2`
   cap whenever `soft` exceeds it. This is the fault that actually forfeits: at
   `remaining = 200 ms, increment = 5000 ms` the cap computes 75 ms and the function returns 4005 ms.

Fault 3 matters for the fix design: the correction proposed in the issue body (making only the floor
relative) leaves it alive, and the 200 ms / 5 s case still returns 4005 ms. The floor and the cap
have to be fixed together.

**Blast radius is narrow.** `Engine::resolve_limits()` calls `compute_budget()` only on the clock
path; `movetime`, `depth` and `infinite` bypass it. Bench, perft, tactical suites and fixed-time
tests are unaffected by construction.

---

## Design

### Formula

Replace the body of `compute_budget()` (`TimeUtils.cpp:16-36`) with:

```cpp
const ms usable = std::max(remaining - overhead, ms{ 0 });  // never invent clock
const ms cap    = usable / 2;                               // never commit more than half
const ms floor  = std::min(floor_time, cap);                // floor yields to a drained clock
const int horizon = (moves_to_go > 0) ? moves_to_go : 30;
const ms base{ static_cast<ms::rep>(usable.count() / horizon + increment.count() * 8 / 10) };
const ms soft = std::clamp(base, floor, cap);
const ms hard = std::clamp(ms{ static_cast<ms::rep>(soft.count() * 3 / 2) }, soft, cap);
```

`overhead` stays 50 ms, `floor_time` stays 100 ms, `base` and the 3/2 hard factor are unchanged.

Why this shape:

- `cap` now bounds **soft as well as hard**, so fault 3 cannot recur — there is no trailing `max`
  to override it.
- `floor = min(floor_time, cap)` is the clock-relative floor. Above ~250 ms remaining it equals
  `floor_time` and behaviour is identical to today; below that it decays with the clock.
- Both `std::clamp` calls are well-formed: `floor <= cap` holds because `floor` is a `min` against
  `cap`, and `soft <= cap` makes the second call's bounds ordered.
- `hard >= soft` still holds, so `TimeManager`'s two-arg `start()` contract is preserved.

### Contract change

`TimeUtils.h:9` documents `@invariant hard >= soft >= 100 ms always holds`. That line is the defect
written down. It becomes:

> `usable/2 >= hard >= soft >= 0`, where `usable = max(remaining - overhead, 0)`.
> A drained clock yields a zero budget rather than a floored one.

Update the `@returns` line on `compute_budget()` to match.

### Zero-budget safety

Below 50 ms remaining the budget is `{0, 0}`. This is safe, not merely tolerated: `TimeManager`
reports expiry immediately, depth 1 aborts, `state.best_move` is left null, and
`handle_empty_move_emergency()` (`AIPerplex.cpp:1021`) supplies a legal move. Moving instantly is
the only non-forfeiting option at that point on the clock.

---

## Behaviour

Worked values, integer arithmetic as the code computes it:

| remaining | inc | mtg | old soft/hard | new soft/hard | |
|---|---|---|---|---|---|
| 150,000 | 2,000 | 0 | 6598 / 9897 | 6598 / 9897 | unchanged |
| 3,600,000 | 0 | 20 | 179,997 / 269,995 | 179,997 / 269,995 | unchanged |
| 60,000 | 0 | 0 | 1998 / 2997 | 1998 / 2997 | unchanged |
| 100,000 | 1,000 | 15 | 7463 / 11,194 | 7463 / 11,194 | unchanged |
| 5,000 | 0 | 0 | 165 / 247 | 165 / 247 | unchanged |
| 500 | 500 | 5 | 490 / 490 | 225 / 225 | fixed — 490 ms budget on a 500 ms clock |
| 200 | 5,000 | 0 | 4005 / 4005 | 75 / 75 | fixed — the issue's worst case |
| 50 | 0 | 0 | 100 / 100 | 0 / 0 | fixed — budget exceeded the whole clock |
| 30 | 0 | 0 | 100 / 100 | 0 / 0 | fixed |

The two formulas diverge only below ~250 ms remaining. Everything the project has ever measured at
10+0.1 sits far above that, so the existing Elo baseline carries over without a new match.

**Drain behaviour.** Deducting `hard` (worst case; the engine normally stops at `soft`) and adding
the increment each move gives a stable positive fixed point instead of a drain to zero:

- 2+0.02 converges to **91 ms** remaining, spending 20 ms/move.
- 5+0.05 converges to **150 ms** remaining, spending 50 ms/move.

Derivation: at a low clock `hard == cap == (r - 50) / 2`, so `r_next = r - (r - 50)/2 + inc`, whose
continuous fixed point is `r = 50 + 2 * inc`. Integer division makes the 2+0.02 case settle one
millisecond above that (91, not 90); 5+0.05 lands on 150 exactly. Assert the observed integers, not
the closed form.

---

## Files Changed

| File | Change |
|---|---|
| `StratEngine/Utils/TimeUtils.cpp` | New formula body |
| `StratEngine/Utils/TimeUtils.h` | Invariant and `@returns` doc |
| `StratChessTests/TimeManagerTests.cpp` | 3 cases updated, 2 added |
| `Docs/Changelog.md` | Entry |

`StratChessTests/SearchLimitsTests.cpp` calls `compute_budget()` to build its own expected values,
so it tracks the change automatically and needs no edit.

---

## Step-by-Step

### Step 1 — `TimeUtils.cpp`

Apply the formula above. `<algorithm>` (for `std::clamp`) comes in via `StdAfx.h`; confirm rather
than assume, and add it there alphabetically if missing.

The existing comment block explaining why the hard factor is 3/2 rather than 3 stays — it still
describes the code. Do not add commentary about what the formula used to be.

### Step 2 — `TimeUtils.h`

Rewrite the `@invariant` and `@returns` lines per Contract Change above.

### Step 3 — `TimeManagerTests.cpp`

Three existing `[time_mgr]` cases assert `soft >= 100` and encode the defect. Update:

- `increment-heavy time trouble (200 ms remaining, 5 s increment)` — assert `soft == 75`,
  `hard == 75`, and `hard <= remaining`.
- `time trouble floor (remaining < overhead)` — rename to
  `compute_budget: clock below overhead yields a zero budget`, since there is no longer a floor to
  test; assert `soft == 0` and `hard == 0`.
- `hard is always >= soft invariant` — drop the `soft >= 100` assertion, keep `hard >= soft`, add
  `hard <= remaining` to the loop.

Leave the blitz, classical and zero-increment cases untouched. They are the regression evidence
that normal play did not move; if they need editing, the implementation is wrong.

Add two cases:

- **`compute_budget: hard never exceeds remaining`** — sweep remaining over
  `{0, 1, 10, 30, 49, 50, 51, 100, 150, 200, 250, 500, 1000, 10'000, 100'000}` x increment over
  `{0, 20, 50, 100, 1000, 5000}` x moves_to_go over `{0, 1, 5, 30}`, asserting
  `hard <= remaining && hard >= soft && soft >= 0` with `CAPTURE`. This is the assertion the issue
  says would have caught the bug without a match.
- **`compute_budget: bullet clock does not drain to zero`** — simulate **400** moves at 2+0.02
  starting from 120,000 ms, deducting `hard` and adding 20 ms each move; assert the clock stays
  `> 0` throughout and that the final 20 values are all equal (converged, not merely positive).
  Repeat for 5+0.05. 400 rather than 200: the clock decays ~5% per move at first, so reaching the
  fixed point from 120 s takes ~140 moves and a 200-move budget leaves little margin if the early
  allocation is retuned later.

Both new cases are deterministic and sleep-free, so they belong in the fast tier.

### Step 4 — `Docs/Changelog.md`

One entry: the budget is now bounded by the clock, the floor decays below ~250 ms remaining, and
sub-100 ms increments no longer forfeit. Note that 10+0.1 numbers are unchanged.

---

## Validation

1. `.\build.ps1 all -Config Debug` then `.\build.ps1 all` — warnings are errors.
2. `Run-Tests.ps1 "[time_mgr]"`, then the full fast tier.
3. `Validate-PrePR.ps1`.
4. fastchess, same binary both sides, ~30 games each at **5+0.05** and **2+0.02** — expect
   **zero time losses**. This is the issue's own acceptance criterion. Record the counts in the PR
   body, not in `Docs/EloLog.md`: these are forfeit checks, not strength measurements.

No 10+0.1 SPRT. The formula is provably identical above ~250 ms remaining and the divergence region
is exactly where the engine currently forfeits, so a match would resolve noise on unchanged code.

No specialised reviewer is due — the diff touches neither `Eval.cpp` nor
`AIPerplex`/`Sort`/`ThreadData`, so it falls outside both reviewer domains. This is not the
logging-only self-certification carve-out.

---

## Invariants After

1. `hard <= max(remaining - 50 ms, 0) / 2` for every input, including negative and zero `remaining`.
2. `hard >= soft >= 0`; `TimeManager`'s two-arg `start()` contract is unchanged.
3. Budgets are identical to the previous formula whenever `remaining > ~250 ms`.
4. A clock at or below `overhead` yields `{0, 0}`, and the search still returns a legal move via
   `handle_empty_move_emergency()`.
5. `movetime`, `depth` and `infinite` search paths are untouched.
6. Both `std::clamp` calls have ordered bounds — no UB at any input.

---

## Follow-Up

File an issue for a UCI `Move Overhead` option (default 50 ms) wired through `UCIHandler`,
`SearchLimits` and `game_settings.json`, so a contended CI runner can raise it without a rebuild.
Out of scope here: it is plumbing on top of a correctness fix, and #204's own risk is addressed by
the `usable/2` cap.
