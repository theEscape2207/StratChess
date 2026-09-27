# History malus and gravity-bounded updates — Design

**Issue:** #651 (child A1 of #636)

## Goal

Quiet moves are ordered by `ThreadData::history[side][from][to]`, and that table only ever rewards.
`update_history()` adds depth² to the quiet move that cut, then clamps at `HISTORY_MAX` (16,384).
Quiets searched before it that failed to cut lose nothing. So the table measures how often a move
cut, not how often it cut *when tried*: a move that is tried often and usually fails outranks one
that is tried rarely and usually cuts. The clamp also saturates, so good moves converge on the same
16,384 and fall back to generation order among themselves. In #636's baseline, late cuts spend 33%
of all nodes before the cutting move, and 35% of late cuts come from history-ordered quiets. The
table is also what A2, A3 and B2 build on, so its update rule has to be right first.

## Scope

**This change will:**

- Penalize, at a beta cutoff by a quiet move, every quiet move searched at that node before it.
- Replace add-and-clamp with a gravity update that bounds `|entry|` by `HISTORY_MAX`.
- Make per-iteration aging decay negative entries toward zero.

**This change will not:**

- Change the bonus magnitude (depth²), `HISTORY_MAX` or the aging cadence. Changing them would
  be a second variable (#636: each child is measured in isolation).
- Add countermove, continuation or capture history (A2 to A4), or feed history into LMR (B2).
- Add a UCI option or `SearchTuning` switch (D7).
- Touch quiescence, which reads the table but never writes to it.

## Decisions

### D1: The malus applies only when a quiet move cuts

When a capture or promotion cuts, the quiets tried before it are not penalized. They failed at a
node where a capture was the answer, which says little about the quiets themselves. This is also
the common convention (Stockfish among others). Rejected: penalizing quiets on every cutoff. It
penalizes a quiet for a capture's merit and couples the table to capture ordering, which is A4's
concern.

"Quiet" means `!IsCapture(move) && !IsPromote(move)`, the same predicate frontier futility and LMP
use, for both bonus and malus. Today `update_history()` rejects captures only, so a quiet promotion
that cuts gets a bonus. `ScoreMoves()` never reads a promotion's entry, but the table has no piece
index, so that bonus lands on the entry a rook or queen shares for the same from/to (a7a8, for
example). Aligning the bonus predicate removes that aliasing. It is a small part of this
behaviour-changing diff, not a separate equivalence step.

### D2: "Searched" means a child search ran

Penalized moves are those that reached `pvs()` for the child. That excludes moves rejected by
`DoMove()`, moves skipped by frontier futility or LMP, and the excluded move in a singular
verification frame. A skipped move was judged, not tried, and penalizing it would feed a pruning
decision back into ordering. A move enters the list after the child search returns and the
`IsAborted()` guard passes. The malus is applied at the cutoff, which is after the guard, so an
aborted frame writes nothing. Killers and a quiet hash move that failed are penalized too: their
entry matters as soon as they stop being a killer or the hash move.

### D3: Gravity update, bonus clamped to `HISTORY_MAX`

`entry += delta - entry * |delta| / HISTORY_MAX`, where `delta = +bonus` or `-bonus` and
`bonus = min(depth * depth, HISTORY_MAX)`. With `|delta| <= HISTORY_MAX` and
`|entry| <= HISTORY_MAX`, the result stays within `[-HISTORY_MAX, HISTORY_MAX]` without a clamp. An
entry near the bound moves less, so the table keeps ranking among good moves instead of
saturating. `entry * |delta|` is at most 2²⁸, which fits in `int32_t`. The bonus clamp binds only at
depth >= 128. It exists so the bound is a property of the formula, not of the depths that search
happens to reach.

Rejected: keep the clamp and add a clamp at `-HISTORY_MAX`. Saturation stays, and it is half of
the problem. Rejected: a linear bonus (Stockfish's `min(k*depth - c, cap)`). That is a second
variable, and it can follow as its own measured change.

### D4: The malus has the same magnitude as the bonus

`malus = bonus` for the node's depth. Rejected: a smaller malus. It is a tuning knob with no
evidence behind any particular value yet. The asymmetric form can be tried once the symmetric one
has a measured result to compare against.

### D5: Aging divides instead of shifting

`age_history()` uses `score /= 2` in place of `score >>= 1`. For non-negative scores the two are
identical. For negative scores `>>= 1` rounds toward minus infinity, so -1 would never decay. `/ 2`
decays both signs toward zero.

### D6: A fixed 64-entry list of quiets tried, per `pvs()` frame

`Move quiets_tried[64]` plus a count, on the stack (128 bytes). Quiets beyond the 64th go
unpenalized: they are the lowest-ordered moves of an unusually wide node, and dropping their malus
changes nothing that matters. Rejected: `MAX_MOVES` (218) entries, which adds 436 bytes per frame
across a 256-ply recursion to cover a case that does not occur in practice.

### D7: No runtime switch

The change ships on, with no `SearchTuning` bool or UCI option. The lab measures it against the
merge base, so a default-off switch is not needed to attribute it. A switch nobody turns off is a
branch on every cutoff plus dead configuration (standing preference: the end state is on or
deleted). If the gate fails, the change is not merged.

### D8: API

`update_history(side, move, depth)` keeps its signature and gains gravity. It is now the bonus, and
the test fixture's `seed_history()` and `poke_history()` keep working unchanged. A new
`penalize_history(side, move, depth)` applies the malus. Both call one private
`apply_history(int32_t& entry, int32_t delta)` that holds the formula.

## Assumptions I cannot verify from the code

- **That malus plus gravity gains Elo in this engine.** It is standard in strong engines, but this
  engine's tree differs, and #634 showed that node screens do not predict wall clock. Settled only
  by the gate and the strength lab under Validation.
- **That #636's baseline table still holds on `origin/main`.** It came from a throwaway probe on
  `af2f996`. It is re-measured with `Compare-SearchProfile.ps1` on the merge base before comparing.

Verified from the code: `history` is read only by `MoveSorter::ScoreMoves()`, in `pvs()` and in
in-check quiescence. LMR, LMP and frontier futility never read it. So the change affects ordering
only, and quiet scores in `[-16384, 16384]` stay below every other tier (losing captures at
700,000).

## Invariants

- `|history[s][f][t]| <= HISTORY_MAX` after any sequence of updates and agings.
- No history write from an aborted frame, and no malus on a move whose child search did not run.
- Ordering tiers other than quiet history are unchanged: hash move, captures, killers.
- Deterministic at `Threads=1`. The table stays per-thread, so there are no shared writes under
  Lazy SMP.
- Node counts change on purpose. This is not an equivalence change.

## Validation

**Tier:** Engine, behaviour-changing.

- **Unit tests (`SortTests.cpp` or a new `SearchHistoryTests.cpp`):**
  - Repeated bonuses never exceed `HISTORY_MAX`, and repeated maluses never fall below
    `-HISTORY_MAX`.
  - A malus lowers the entry.
  - Aging takes -1 to 0.
  - After a fixed search, at least one entry is negative (the malus is wired in) and every entry is
    within bounds.
  - Each assertion is falsified once by reverting its piece.
- **Tree direction:** `Compare-SearchProfile.ps1 -Before <merge base> -After <candidate>`, both
  `STRAT_SEARCH_PROFILE=1` builds. Expected: nodes before the cutting move fall from about 33%, and
  the history share of late cuts drops. This is direction only, not a verdict.
- **Speed:** `Run-Bench.ps1` nps, paired against the merge base. The per-node cost is one store per
  quiet searched, plus a loop at cutoffs.
- **Gate (#522 method):** interleaved fixed-depth wall clock, `Threads=1`, against the exact merge
  base. Passes at median <= -3% with >= 8/9 rounds faster.
- **Strength:** a CI strength-lab run against `merge-base`, on the owner's decision (~3 h, 18 of 20
  CI slots). It is needed because an ordering change can shrink the tree and still lose accuracy.
- Linux Debug with sanitizers runs in CI as usual.

## Cost

- **Size:** 50-200 lines in about 4 files: `ThreadData.h`, `AIPerplex.cpp`, one test file and the
  Changelog.
- **Blast radius:** Engine tier. No gate, skill or script changes.
- **Review:** the code review, a `search-reviewer` dispatch, and the cross-agent round.
- **Measurement:** the wall-clock gate (9 interleaved rounds) and, if the owner approves,
  one lab run.
- **Optional parts:** none.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Gravity formula and why the bonus is clamped (D3) | comment on `apply_history()` |
| Malus scope: quiet cutters only, searched moves only (D1, D2) | comment at the cutoff in `pvs()` |
| Why aging divides instead of shifting (D5) | comment on `age_history()` |
| History can be negative, and is bounded by `HISTORY_MAX` | comment on the `history` member |
| Profile deltas, gate and lab result | PR body, `Docs/Changelog.md`, `Measurements/ci-per-change.md` |
