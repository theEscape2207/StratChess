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
`depth == 1`, and `|alpha| < Mate_Threshold`. Move level, in two halves. Before `DoMove()`, on the
parent position: at least one legal move already searched, not a capture, not a promotion, not either
live killer, not the hash move, then the eval test. After `DoMove()`, before the `move_number == 0`
branch (which `legal_moves_searched >= 1` already rules out): the move does not give check, and does
not draw on the spot by repetition or the fifty-move rule (`check_draws(ply + 1)`). A cross-agent
review found the draw case: a side the margin calls lost may hold a draw only through quiet moves.
`InCheck()` is last because it is the expensive term, and it can only be asked of the child. The
board is restored with `UndoMove(move)` before `continue`.

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

The frontier test needs the **parent's** evaluation, but after `DoMove()` `td.board` holds the child,
so the value must be taken before the move is made. pvs() keeps a node-local evaluation that is filled
on first use, and only ever on the parent position:

- the reverse guard fills it where it evaluates today, before any move is made;
- in the move loop, it is filled **before `DoMove()`**, and only when the move passes the pre-move
  half of D2. A node with no quiet later candidate never pays for it.

The cost is at most one parent evaluation per node. It can be spent on a candidate that then turns
out to be illegal or to give check, and that waste is harmless. A depth-1 node that reaches both
guards evaluates once, not twice. An eager fill at every eligible node was rejected because it charges
nodes whose later moves are all captures, killers or illegal.

The reverse-guard refactor must stay node-identical to the merge base, and a gate-0 build proves it.
A unit test pins the parent-position requirement: a quiet move that swings the evaluation must be
pruned or searched according to the parent's evaluation, not the child's.

### D4: What persists after a skip

The node **stores to the TT as usual**, but first lowers its claim: when a move was skipped,
`best_value = max(best_value, static_eval + margin)` before the return and the store. Suppressing the
store, as reverse futility does, was rejected. Reverse futility has no search evidence at all; here
at least one move was really searched. Suppression would also re-search cheap nodes and cut into the
tree win.

**This is a selective-search heuristic, not a reproducibility guarantee, and the risk is accepted.**
The floored UPPER bound is only as good as the pruning decision behind it. That decision depends on
live state that can change: a skipped move can later become a killer at this ply, or become the hash
move. A later probe that cuts on the stored bound then skips a move that a fresh search would examine.

The engine already accepts this kind of risk. A null-move cutoff stores a full-depth LOWER bound from
a reduced search, and LMR results are stored as if searched at full depth. The exposure here is
narrower than either:

- the entry has depth 1, so only depth-1 probes can use it, plus quiescence, which also reads MAIN
  entries;
- it can only cut at an alpha at or above the floored value, and that is where the frontier guard
  would already be pruning quiet moves on the same static evaluation.

The floor binds less often than it looks. Quiescence fails high at exactly its beta, so at depth 1 a
fail-low child hands this node exactly alpha. `best_value` is then already alpha, and the floor
(`<= alpha`) changes nothing. It binds only when a searched child returns *below* alpha. Two cases do
that: a draw (repetition or fifty-move), and a TT hit whose stored value lies past the child's bound. There it stops the return value, and the store,
from claiming that the skipped moves score as low as the searched draws. It would become
load-bearing everywhere if quiescence went fail-soft. The floored value is still `<= alpha`, so the bound type
does not change. A LOWER bound can only come from a searched move that failed high, which is genuine
whatever else was skipped. EXACT is unreachable because PV nodes are excluded.

Suppressing the store is the fallback if a tactical-suite regression traces to a stored bound, or if
`search-reviewer` objects.

A skipped move `continue`s before any PV, killer or history update, so no write is ever attributed to
it.

### D5: Gate shape

This follows the Stage 1 reverse-futility precedent. `STRAT_FRONTIER_FUTILITY` has three levels:

- 0 ships and compiles no code;
- 1 compiles the guard in with `SearchTuning::frontier_futility_enabled` off;
- 2 compiles it in with the flag on.

The test target compiles it in with the flag off, and the tests turn it on themselves. Level 1 exists
so that "flag off is node-identical" is checked on a binary that has the branch and does not take it.

**Superseded once the strength lab measured level 2 at +23.39 +/- 3.46 Elo** (run `34596140552`).
As with reverse futility, the compile-time gate is removed and the guard is unconditional. Only
`SearchTuning::frontier_futility_enabled` survives, defaulting to `true`, so tests can turn it off.
A depth-2 experiment gets its own gate; these levels could not have switched it.

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
- The frontier test only ever uses the parent position's evaluation, never the child's.
- No killer, history or PV write is made for a skipped move.
- A node that skipped a move never returns, or stores, a value below `static_eval + margin`.

## Validation

- **Engine tier.** Every live guard (PV, in check, depth, mate-range alpha, capture, promotion,
  killer, gives check, first legal move) has a test proved by removing that guard and watching the
  test fail. Persistence has the same treatment: the floored UPPER store and no skipped-move writes.
- Run the full suite, and the tactical suite including stability mode, with level 2.
- Dispatch `search-reviewer`.
- **Gate: interleaved fixed-depth wall clock** against the exact merge-base binary, `Threads=1`,
  clang-cl, over the `Run-Bench.ps1` set. The shape follows the Stage 1 precedent: 9 interleaved
  rounds, compared as paired per-round ratios.
  - **Down** needs a median paired change of −3% or better *and* a decrease in at least 8 of the 9
    rounds.
  - Anything else **parks** the candidate.
  - This hardware has produced 12% single-run outliers. One outlier cannot flip the median, or more
    than one round's sign, so no single run decides.
  - Main-tree and quiescence node counts explain the result but are not the verdict.
- **No Elo match in this PR.** The PR opens as a draft and stays unmerged until the owner decides on
  the strength-lab run. A pass at the wall-clock gate earns the question, not the merge.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2 eligibility, D4 floor rationale | source comments at the guard |
| D5 gate levels, and their removal | `Docs/Changelog.md` |
| wall-clock result | `Docs/Changelog.md`, PR body |
| strength-lab result | `Measurements/ci-per-change.md`, `Docs/Changelog.md` |
| depth-2 follow-up | stays in #504 |
| floor binds only below-alpha children (quiescence fails high at exactly beta) — found in implementation | source comment at the floor, floor test comment |
