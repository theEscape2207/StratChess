# Quiet-move SEE pruning in main search — Design

**Issue:** #399

## Goal

`pvs()` has eval-margin pruning for quiets (reverse futility, frontier futility) and index-based
pruning (LMP), but nothing that asks whether a late quiet simply loses material on its destination
square. About 11% of the fixed-depth tree lies under such moves, and about 1 in 2,000 of them beats
alpha. This adds a default-off guard so the strength lab can decide whether that is worth having.
The screen's result ends in a one-line default flip or in deleting the guard.

## Review focus

- **D3, storing UPPER.** The issue's 2026-10-22 re-triage called "use LMP's `lmp_skipped` handling"
  settled. This document reverses it on a count the re-triage did not have. The riskiest question is
  whether an UPPER at alpha resting on skipped moves does more harm than losing 97% of the stores at
  depth 3.
- **D4, screen order.** This document also replaces the re-triage's "B first, A only if B is
  positive" with A first, B only if A fails, because depth 1 holds two thirds of the headroom.
- **Assumption 1:** the guard's SEE cost is recovered. Only the lab answers it; the bench bounds it.

## Scope

**This change will:**

- allow quiet moves in `See::see_ge`, except castling;
- add the guard to `pvs()` behind `QuietSeePruning` (default off) with min depth, max depth and
  margin knobs, plus a skip counter;
- add SEE cases and a falsified test per guard.

**This change will not:**

- change quiet ordering (#636), add capture SEE pruning to `pvs()`, or model pins in SEE;
- scale the threshold by LMR-reduced depth or extend the band past depth 3 by default. The knobs
  reach deeper, but a deeper arm is a follow-up, so that one screen varies one thing;
- change LMP's or frontier futility's own return and store rules;
- flip the default. That is a follow-up PR after the screen, which is the owner's call.

## Decisions

### D1: `see_ge` takes quiets; castling is a caller-side exclusion

Quiets already work arithmetically: `gain = 0`, and the swap loop asks whether the opponent can
profitably take the piece on `to`. Only the assert and the contract in `See.h` forbid it.

Castling stays out through a precondition and an assert (`from`/`to` are the king's and the rook
move is invisible), and the guard excludes it with `MoveHelper::AsType`. Rejected: returning `true`
inside `see_ge`, as the re-triage suggested. That adds a branch to a function that qsearch and
`ScoreMoves` call on every capture, to serve one caller that can test it for free.

Two approximations are accepted and documented in `See.h`: a pinned attacker or defender still
counts, and a double push's en-passant recapture is not seen, since that capture does not land on
`to`.

### D2: Guard shape

Threshold `-margin * depth * depth`, using the node's `depth`.

- **Node level** (`quiet_see_pruning_eligible`, tested directly, like its neighbours): flag on, not
  PV, not in check, not an exclusion frame, `min_depth <= depth <= max_depth`,
  `abs(alpha) < Mate_Threshold`, `zugzwang_safe`.
- **Before the move:** `legal_moves_searched >= 1`; `best_value > -Mate_Threshold`; quiet, not
  castling, not a killer, not the hash move; neither `frontier_candidate` nor `lmp_candidate` (those skip it under the same post-move
  gates, so the SEE call would be wasted); then `!see_ge(...)`, last because it is the expensive term.
- **After the move:** no checking move and no immediate draw, exactly as frontier futility and LMP.
- The skip comes before every PV, killer, history and TT write, and increments
  `telemetry.quiet_see.skips`, printed as `info string quietsee skips N` when non-zero.

Frontier futility needs a 200 cp eval deficit and LMP needs the ninth legal move. This guard has
neither, so two gates cover what it would otherwise newly reach. Both are free, and the split probe
(margin 20) shows they give up almost nothing:

| Class | Headroom, depths 1-3 | Beat alpha | Why it can be wrong |
|---|---|---|---|
| Fewer than two non-pawn pieces (`zugzwang_safe` false) | 0.16% of tree | 0.159% at depth 1, 3.5x the rest | the key move is often a SEE-negative push or deflection |
| A searched move already returned a mated score | 0.00% (110 prunes) | 6.4% at depth 2 (5 of 78), 48x the rest | the only defence is often a sacrificial block on an attacked square |

The first class's other rates are at or below the rest, the set holds only 24 endgames, and the second
class is 5 moves, so neither gate is a measured strength gain. They are kept because they cost nothing.

Rejected: requiring `beta == alpha + 1` as LMP does. LMP needs it for its return rule, and D3's floor
does not: it can only raise a value already at or below alpha.

### D3: A completed fail-low returns alpha and stores UPPER at alpha

After the loop, beside the frontier floor:

```cpp
if (quiet_see_skipped)
    best_value = std::max(best_value, original_alpha);
```

The node then takes the ordinary classify-and-store path. A searched cutoff after a skip is
untouched and stores LOWER.

Options considered:

| Option | Return | Store | Verdict |
|---|---|---|---|
| LMP's handling | alpha | none | rejected, see below |
| Frontier's floor | `eval + margin` | UPPER | rejected: SEE gives no eval bound, so the floor means nothing |
| Plain fail-soft | best searched | UPPER below alpha | rejected: the searched subset proves no value below alpha |
| **Chosen** | alpha | UPPER at alpha | |

LMP's handling was the re-triage's choice, and it looks harmless only because of when LMP fires: from
the ninth legal move. This guard fires from the second. Counted on `aad46d8c` (setup in Validation):

| Depth | Completed non-PV fail-lows | Already lose their store to LMP | Would newly lose it |
|---|---|---|---|
| 1 | 5.31M | 0 | 2.23M (41.9%) |
| 2 | 2.29M | 2.26M (98.9%) | 8.8k (0.4%) |
| 3 | 0.97M | 0 | 0.94M (97.0%) |

LMP's handling would therefore remove nearly all UPPER entries at depth 3 and 42% at depth 1, a cost
that falls on the guard's whole result and that the arms could not separate from the pruning itself.

The stored claim is the one the node already returns to its parent, that the value is at most alpha.
It is selective, and so is null move's stored bound and frontier's (`AIPerplex.cpp`, comment above
the floor). It is also tighter than frontier's: the threshold grows with depth and SEE depends only on
the position, so a move skipped at depth d is also SEE-losing at every shallower depth an entry can
answer. Only killers and the first legal move differ.

Dropping the stores would also starve IIR, a lab-gated feature. A fail-low stores a non-null best move,
and that move is what stops a depth-4+ visit from reducing again.

A node where LMP also skipped still takes LMP's return, because that check follows the floor. So LMP's
behaviour does not change, and depth 2 stays where it is today.

### D4: Screen arms, A first

Re-probed on `aad46d8c`, with D2's gates. `Tree` is 326.2M nodes (376.8M on the issue's `f9fcf8a`).
Margin 20.

| Depth | Threshold | SEE calls | Prunes | Subtree share of tree | Nodes per prune | Nodes past the edge, per SEE call | Beat alpha |
|---|---|---|---|---|---|---|---|
| 1 | -20 | 75.0M | 18.44M | 7.30% | 1.29 | 0.07 | 0.045% |
| 2 | -80 | 9.7M | 1.85M | 1.07% | 1.88 | 0.17 | 0.134% |
| 3 | -180 | 29.5M | 5.71M | 2.81% | 1.61 | 0.12 | 0.011% |

The guard still pays make, check test and unmake on a skipped edge, so it saves in full only the nodes
past the edge, plus a child frame per edge. That makes the second-to-last column the fair one. Depth 1
is the weakest band per SEE call, as the re-triage argued, but it holds two thirds of the headroom.

SEE values sit on a 100 cp lattice, so at margin 20 depths 1 and 2 are the same test (loses at least a
pawn) and depth 3 means loses at least 200.

- **Arm A:** depth 1-3, margin 20. Headroom 11.18%, 114.2M SEE calls.
- **Arm B:** depth 2-3, margin 20. Headroom 3.88%, 39.2M SEE calls.

They differ in one variable.

**Decision rule, stated before the run:**

1. Run A. If its Elo minus its error bar is above 0, flip the default in a follow-up.
2. Otherwise run B, under the same rule.
3. Otherwise delete the guard, its knobs and the `see_ge` relaxation, unless something else has come to
   use it. Margin 50, which changes only depths 2-3 and so addresses depth 2's high beat-alpha rate, is
   the owner's option before deleting.

Rejected: B first (the re-triage), which could park the idea with its largest band untried. Also
rejected: both arms at once, which costs a second run when A wins and adds selection bias, since tied
arms lose about 0.6-0.85 sigma on confirmation.

No deeper arm. Depths 4-6 at this threshold shape hold only 0.56% of the tree for 21M SEE calls, with
1.4-2 nodes per prune, so the larger subtrees expected there are not on this tree. A deletion records
that depth 4+ was counted but never lab-tested, and that a shallower threshold was not counted.

## Assumptions I cannot verify from the code

1. **The SEE cost is recovered.** Arm A runs about 114M calls on a 326M-node tree. Not verified. A
   bench's wall time to fixed depth, flag on against flag off in one binary, bounds the net cost; nps
   cannot, because it falls even when the guard wins (skipped edges are paid for and not counted). The
   lab settles it. Node screens have not predicted the lab on this engine (#634, #664), so no bench number
   parks this on its own.
2. **Probe counts carry over.** They come from the unpruned tree at depth 12, which pruning reshapes
   through the TT, ordering and LMR. They choose arms and do not forecast Elo.
3. **The UPPER store helps more than it harms (D3).** Not verified, and not separately measured: a
   store-versus-no-store arm would be a third lab run. If both arms park, this assumption is one
   reason not excluded, and that is recorded with the result.

## Invariants

- `QuietSeePruning=false` is node-identical to the merge base.
- A skip never leaves a node without a searched move, so it cannot fabricate mate or stalemate.
- Captures, promotions, castling, killers, the hash move, checking moves and immediate draws are never
  skipped, nor is any move at a node without two non-pawn pieces or after a searched move returned a
  mated score.
- An aborted frame keeps no results: the skip runs no child search and precedes every persistent write.
- `see_ge`'s results for captures and promotions do not change.

## Validation

Engine tier. No Elo gate for the PR itself, since the flag is off. The PR is the lab vehicle.

- **Equivalence:** `Compare-SearchEquivalence.ps1` against the merge base with the flag off: identical
  nodes and best moves at `Threads=1`.
- **SEE:** hand-computed bracketed cases for a hanging quiet, an attacked-but-safe destination, a quiet
  interposition, and the pinned-defender limitation, pinned as today's answer.
- **Guards:** one test per node-level and move-level gate, in a new `SearchQuietSeePruningTests.cpp`,
  each falsified by removing the gate and watching it fail. The hash-move term is the one exception
  and is stated as such, as in LMP's tests: it sorts first, so it never reaches `legal_moves_searched >= 1`.
- **Return rule:** a skipped fail-low returns alpha and stores UPPER at alpha; a cutoff after a skip
  stores LOWER; a node where LMP also skipped stores nothing; an aborted search unwinds to the root board.
- **Speed:** `Run-Bench.ps1` under `measure-strength`'s rules: nps with the flag off against the merge
  base (the added branch), and wall time to fixed depth with the flag on against off, for each arm.
- **Strength:** the D4 sequence at 10+0.1, `Threads=1`, one binary, `QuietSeePruning` on against off.
  Owner's call. Option delivery is proved by the PGN or `quietsee skips` line, since a ~0 result may
  mean the option never applied.

**Probe setup, for every table:** throwaway diff from the issue's 2026-10-11 comment, ported to
`aad46d8c`, clang-cl Release, `Threads=1`, `go depth 12`, `ucinewgame` before each of the 200 positions
in `Tests/profile-screen.fen`. Every move was still searched. One run each; node counts at `Threads=1`
are deterministic. The D3 table was counted before D2's two gates, which remove under 2% of the
would-prunes.

## Cost

- **Size:** 50-200 engine and doc lines across about 8 files (`See.{h,cpp}`, `AIPerplex.{h,cpp}`,
  `SearchTuning.def`, `SearchTelemetry.h`, `Docs/EngineContracts.md`, `Docs/EngineGuide.md`), plus
  roughly 300 test lines.
- **Blast radius:** engine tier; behaviour unchanged with the flag off. `Compare-SearchProfile.ps1`
  gains the optional `quietsee` line.
- **Review:** one code review plus `search-reviewer`.
- **Paid measurement:** one lab run if A wins, two otherwise, about 2-3.5 h each. Owner's call.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 quiet contract, castling precondition | `See.h`, `Docs/EngineContracts.md` |
| D2 gates | source comments at the guard; `Docs/TestDesign.md` coverage row; `Docs/EngineGuide.md` for the counter and options |
| D3 why UPPER at alpha, with the depth-3 count | source comment at the floor; `Docs/Changelog.md` |
| D4 arms, probe tables, decision rule | issue comment; lab result in `Measurements/ci-per-change.md` |
| LMP costs 98.9% of depth-2 fail-lows their store, so try LMP storing UPPER at alpha | new (idle-lab candidate) issue |
