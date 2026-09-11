# Frontier futility pruning (depth 1) — Design

**Issue:** #504 (Stage 2 of #87)

## Goal

At a depth-1 non-PV node, a quiet move whose parent static evaluation stands well below alpha almost
never raises alpha, yet each one still costs a child quiescence search. Skipping those moves shrinks
the tree at the frontier. Reverse futility (the node-level beta cutoff) already ships; this is the
move-level alpha-side counterpart, measured on its own so that its Elo effect can be attributed.

## Scope

**This change will:**

- skip eligible quiet later moves at `depth == 1` behind a compile-time gate plus a runtime flag;
- compute the static evaluation at most once per node, shared by the reverse and frontier guards.

**This change will not:**

- touch depth 2 ("extended futility"). That is a separate experiment, opened only if depth 1 earns it.
- change reverse futility's margin or band (#502), or use quiet-move SEE as a signal (#399);
- add a history-score threshold to the protected-move set.

## Decisions

### D1: Guard shape and margin

A move is skipped when `static_eval + frontier_futility_margin <= alpha`, with the margin fixed at
**200 cp** and `depth == 1` only. 200 is the value `delta_pruning_margin` already trusts for one
move's positional swing; its comment derives a worst case near 110 cp. 100 cp (reverse futility's
per-ply slack) was rejected as the most tactically exposed option, and 300 cp (Heinz's minor-piece
margin) because it gives the smallest tree win. The margin is pre-registered and held fixed through
the wall-clock pass and any strength run.

### D2: Eligibility

Node level, computed once before the move loop: non-PV, not in check, not an exclusion frame,
`depth == 1`, and `|alpha| < Mate_Threshold`. Move level, only in the `move_number >= 1` branch after
`DoMove()`: not a capture, not a promotion, not either live killer, not the hash move, and not giving
check. `InCheck()` goes last because it is the expensive term. The board is restored with
`UndoMove(move)` before `continue`.

- **First legal move.** It is always searched, so pruning cannot fabricate mate or stalemate.
- **Exclusion frames.** Unreachable at depth 1 today, because verification depth is at least 3, but
  the gate is kept for the reason reverse futility's is: a verification search's fail-low grants the
  extension, and it must come from a search rather than from a margin.
- **Hash move.** Redundant while the hash move sorts first. It is kept as defence in depth, and no
  test claims it as an independent gate.
- **Protected moves.** The two live killers only. A history threshold would add a tuning dimension to
  a result meant to be attributable.
- **No zugzwang floor.** Null move and reverse futility need one because they assume the side to move
  has a useful move. Frontier futility assumes the opposite, that a quiet move gains little, and
  zugzwang only makes that assumption more conservative.

### D3: One lazy static evaluation per node

pvs() keeps a node-local evaluation that is filled on first use. The reverse guard reads it, and the
frontier guard reads it only when the first eligible move reaches the eval test, so a node with no
candidate move never pays for it. A depth-1 node that reaches both guards evaluates once, not twice.
The reverse-guard refactor must stay node-identical to the merge base, and a gate-0 build proves it.

### D4: What persists after a skip

The node **stores to the TT as usual**, but first lowers its claim: when a move was skipped,
`best_value = max(best_value, static_eval + margin)` before the return and the store. Suppressing the
store, as reverse futility does, was rejected. Reverse futility has no search evidence at all; here
at least one move was really searched. Suppression would also re-search cheap nodes and cut into the
tree win.

The floor makes the UPPER bound claim no more than the pruning supports. The floored value is still
`<= alpha`, so the bound type does not change. Any later probe that could cut on it has an alpha
`>= static_eval + margin`, and at that alpha the same guard would skip the same moves, so the TT
answers the same way the search would. A LOWER bound can only come from a searched move that failed
high, which is genuine whatever else was skipped. EXACT is unreachable because PV nodes are excluded.

A skipped move `continue`s before any PV, killer or history update, so no write is ever attributed to
it.

### D5: Gate shape

This follows the Stage 1 reverse-futility precedent. `STRAT_FRONTIER_FUTILITY` has three levels:

- 0 ships and compiles no code;
- 1 compiles the guard in with `SearchTuning::frontier_futility_enabled` off;
- 2 compiles it in with the flag on.

The test target compiles it in with the flag off, and the tests turn it on themselves. Level 1 exists
so that "flag off is node-identical" is checked on a binary that has the branch and does not take it.

## Assumptions I cannot verify from the code

- **The strength effect is unknown.** Reverse futility's +44.6 Elo is no forecast. This design settles
  only the tree-size precondition; the strength run is the project owner's decision.
- **Checking moves are 0.92% of depth-1 candidates.** This comes from #498's probe on the pre-reverse
  -futility tree. It is not re-measured and it decides nothing; it only explains why the check term
  goes last.

## Invariants

- A gate-0 build and a level-1 build are node-identical to the merge base (`Compare-SearchEquivalence`).
- The first legal move is never skipped, so no fabricated mate or stalemate is possible.
- The board is restored before every skip.
- No killer, history or PV write is made for a skipped move.
- A node that skipped a move never returns, or stores, a value below `static_eval + margin`.

## Validation

- **Engine tier.** Every live guard (PV, in check, depth, mate-range alpha, capture, promotion,
  killer, gives check, first legal move) has a test proved by removing that guard and watching the
  test fail. Persistence has the same treatment: the floored UPPER store and no skipped-move writes.
- Run the full suite, and the tactical suite including stability mode, with level 2.
- Dispatch `search-reviewer`.
- **Gate: interleaved fixed-depth wall clock** against the exact merge-base binary, `Threads=1`,
  clang-cl. Down, or park. Main-tree and quiescence node counts explain the result but are not the
  verdict.
- **No Elo match in this PR.** The strength run is requested from the owner afterwards.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2 eligibility, D4 floor rationale | source comments at the guard |
| D5 gate levels | `CMakeLists.txt` option comment, `AIPerplex.h` |
| wall-clock result | `Docs/Changelog.md`, PR body |
| depth-2 follow-up | stays in #504 |
