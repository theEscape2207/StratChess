# Drawish-Material Recognition / Endgame Scale Factors — Design

**Issue:** #128 (carries #118 item 5) · **Epic:** #110

## Goal

`EvalComplex::Evaluate()` has no concept of drawish material. It sums material plus positional terms
and returns the result, so a bare minor against a lone king scores about +320 and a pawnless rook and
minor against a rook scores a full piece up. Measured over 19,980 games (run `33215162562`,
`Docs/MoveQuality.md`): 656 games (3.3%) reach K+minor vs K with the stronger side reporting >= +250,
and every one is drawn; 262 (1.3%) reach pawnless KR+minor vs KR at >= +250 and score 0.645. In 7.0%
of games the result is a draw by insufficient material, and in 665 of those the better side's *last*
reported score was >= +150 — with a pawn still on the board six plies earlier in 44.1% of them. The
engine is trading its last pawn into a dead draw while reporting +3.4. This is the largest located
evaluation defect in the corpus, and the eval calibration curve is flat in every phase *except* when
pawns are gone, which is what points at material classes rather than at tapering.

## Scope

**This change will:**

- classify the position's material into one of: exact draw, scaled, or normal, from per-color piece
  counts, once per evaluation;
- return exactly `GameValues::Draw` for KK, K+B vs K, K+N vs K, K+NN vs K (either orientation) and
  for the canonical wrong-coloured-bishop rook-pawn fortress;
- apply a fractional scale to pawnless KR+minor vs KR and pawnless KR vs K+minor;
- re-express the mop-up gate in terms of the defender's remaining force instead of its phase (#118
  item 5), from the same piece counts;
- expose the applied scale in `EvalBreakdown` and the UCI `eval` table.

**This change will not:**

- scale opposite-coloured-bishop endings. The measurement is the reason: OCB at >= +250 converts at
  0.881 [0.846, 0.916], close enough to the pawn-rich calibration curve that scaling it toward zero
  is the one change in this area with a measured expectation of *losing* Elo.
- add a search-side draw rule, touch `ThreadData::check_draws()`, the TT or repetition detection;
- solve KPK or fortresses generally, or approach Syzygy (#101);
- retune unrelated evaluation terms (#117);
- recognise two same-coloured bishops against a bare king, which also cannot mate. It is the same
  kind of rule as K+NN vs K and `eval_bishops` already has the square-colour machinery, but it is
  reachable only by promotion — frequency near zero, so it is a follow-up rather than part of the
  first cut;
- teach mop-up which corner a KBN mate must be driven into (#118 item 2). The epic index attributes
  it to this plan because the bishop's square colour is the shared ingredient, but it is a mating
  *technique* improvement, not drawish-material recognition, and nothing here needs it. It stays
  open against #118's register.

## Decisions

### D1: The classifier lives in eval, not search

An exact scale of zero at an evaluated leaf already produces `GameValues::Draw`, which is what a
search-side "this is a drawn ending" rule would produce anyway. Rejected: a terminal draw rule in
`check_draws()`. It would interact with the TT (a drawn-by-material score is not depth-bounded the
way a repetition score is) and with repetition detection, for no gain over the eval path.

### D2: `Evaluate()` is refactored to assemble one white-POV score before any scaling

Today `Evaluate()` ends in two mirrored return statements, one per side to move; there is no single
place holding "the score of this position". A scale applied in two branches is two chances to get the
sign wrong, and #125's mirror property is exactly what would break. So: sum material and blended terms
into one white-POV value, apply the scale to that with sign-symmetric integer arithmetic, then apply
the side-to-move sign. Rejected: scaling inside both branches; rejected: scaling the side-to-move
value, whose sign depends on whose turn it is rather than on the position.

Because integer division truncates toward zero, `(-n)/d == -(n/d)`, so a single multiply-then-divide
on the white-POV value is odd-symmetric and preserves the mirror property by construction rather than
by test.

### D3: Piece counts move into `EvalContext` — deferred to the mop-up step

`BuildContext()` already popcounts knights, bishops, rooks and queens per color to compute the game
phase, and throws the counts away. Storing them gives the classifier and the re-expressed mop-up gate
a *single* material model, which is what the issue asks for — the alternative is a second material
test growing inside the mop-up gate, which is how the current phase-keyed gate came to disagree with
what "the defender can still hold" means.

**Changed during implementation, and then rejected.** Part 1 was built with the field and then took
it out again: the classifier and the mop-up gate are both computed *inside* `BuildContext`, so they
share one model through locals and the context field had a single reader in the same function. It
bought nothing and made the per-call context larger. The classifier now reads the bitboards directly
and counts only what it needs, past its own early-out.

The field was expected back with the mop-up re-expression, as the second consumer. It was not
needed: "the loser holds no heavy piece" is `(loser queens | loser rooks) == 0`, two bitboards
`BuildContext` has already loaded, and no count at all. The decision's premise — that a second
material test would grow inside the gate — was right; its conclusion, that the shared model has to
be a stored count, was not. There is no `EvalContext` piece-count field, and nothing now wants one.

### D4: No `Mate_Threshold` guard

The design sketch this file replaces called for `abs(score) >= Mate_Threshold` before scaling. That is
wrong here. `Evaluate()` only ever produces a static centipawn score from material plus positional
terms; mate scores are constructed by the search around it and never pass through this function. The
guard would be dead code asserting a false premise.

### D5: Exact zero for insufficient material is a correctness rule, the rook factors are parameters

K+minor vs K is drawn by rule, not by tendency — 656 games, 0.500 [0.500, 0.500]. It ships on the
strength of the `[eval]` regression tests and must not be made conditional on a favourable SPRT. The
two nonzero rook-ending factors are ordinary strength parameters and are retained, adjusted or dropped
on match and calibration evidence.

### D6: KR vs K+minor is scaled, not drawn

Cumulatively (>= +100) it scores 0.720 [0.684, 0.751], but the >= +250 subset scores 0.801 — that
ending is often genuinely won. Clamping it to zero would throw away real winning chances; a factor
keeps the search preferring the better version of the ending, which is the whole argument for scaling
over clamping.

### D7: Wrong-coloured bishop is a fortress condition, not a piece count

The safe first cut is the canonical case only: no attacker material besides the bishop and rook
pawn(s), and the defending king already standing in the promotion corner. The same material with the
defending king outside that corner is winning, so a piece-count-only rule would clamp won positions to
zero. This is also the class with no frequency measurement behind it, so it lands last and separately.

**Settled during implementation.** "In the promotion corner" is `KingDistance(defender, promotion
square) <= 1`. From any of those four squares the defender can always step back onto the promotion
square, and nothing the attacker holds can take it away, so the "no defence loses" bar is met; the
promotion square alone would have been safe too but would fire in one placement only, which is
almost no guidance for the search. Doubled rook pawns are still one promotion square and stay in the
class; pawns on *both* rook files are not, since their promotion squares are opposite colours.

### D9: The mop-up gate keys on the defending QUEEN, and nothing else

`#118` item 5 asks for the defender's force. The rule is that it holds no queen. Mop-up pays for
driving the losing king to the edge; a queen is the one piece that can check the winner's king away
from the corner indefinitely and leave that plan permanently unfinished. Rejected: a
defender-material threshold, which is the phase gate's mistake in another unit.

**Also rejected, after being built and measured: "no queen and no rook."** It is the more natural
reading of "what force the defender has", and it is wrong, because switching mop-up off is not a
neutral withdrawal of a bonus. `eval_pst` suppresses the winner's king PST exactly when mop-up is
active (item 4), so every class removed from the gate gets its **centralizing** endgame king table
back. On K+Q vs K+R, walking the winning king toward the cornered loser went from +4 cp to -8 cp — a
12 cp swing into a disincentive, which is the item 4 defect reinstated for a family of endings whose
winning method is precisely that walk (K+R+R vs K+R, K+Q+R vs K+R and K+R+B+N vs K+R go with it).
A rook cannot check a queen away from the corner, and those endings are won by cornering, so they
keep their mop-up.

The general lesson, worth more than the rule: **the mop-up gate has two consumers and they are not
symmetric.** Narrowing it withdraws a bonus *and* re-enables a term pulling the other way, so any
future change to it has to be argued at both call sites, not just at `eval_mopup`.

### D8: The breakdown gets a scale row, not redistributed rows

`EvalBreakdown`'s fields are per-color; a final scale is not — it applies to the white-minus-black
difference. Distributing it across the per-color rows would make each row a number no part of the
evaluator computes. Instead the breakdown carries the scale itself plus the net adjustment it caused,
so the printed table still reconstructs `total` exactly (the #129 honesty invariant, asserted in
`StratChessTests`).

## Assumptions I cannot verify from the code

- **The measured class frequencies and observed scores** come from `Scripts/analyze_move_quality.py`
  over strength run `33215162562`, not from anything checked here. They are reproducible: re-running
  the tool over the same run's PGNs reproduces every figure quoted above. Not re-run for this
  document.
- **That correcting these scores gains Elo rather than merely correcting a display.** The corpus
  bounds the prize (5.6% of games reach a mis-scored class with the stronger side claiming a real
  advantage) but does not measure it — the error costs a half point only where it made the engine
  *prefer* the drawn simplification. Settled by SPRT plus a re-scan of the resulting PGNs against the
  four baseline rows. Not done yet; it is step 5 below.

Verified from the code and not assumed: `pvs()` never calls `Evaluate()` — the only consumers are the
three sites in `AIPerplex::quiescence()` (budget exhaustion, the `MAX_PLY` backstop, and stand-pat)
plus `PlayerAI.cpp`. There is no static-eval futility or razoring path for a scaled score to leak
into, which bounds the blast radius of this change to qsearch leaves and the stand-pat cutoff.

## Invariants

- **Mirror symmetry (#125) survives.** A position and its color mirror evaluate equal; the classifier
  is orientation-free.
- **The breakdown reconstructs the total (#129).** Material plus the per-term rows plus the endgame
  adjustment equals `total` up to the side-to-move sign.
- **Node counts are unchanged by the refactor step alone.** Step 1 is behaviour-preserving and is
  gated on `Compare-SearchEquivalence.ps1` reporting identical nodes at `Threads=1`.
- **Won endings stay won.** K+B+N vs K, K+R vs K and K+Q vs K+R are not scaled; K+Q+R vs K+Q loses its
  mop-up term but not its material.
- **Delta pruning is NOT sound once a scale is fractional, and this document said the opposite.** The
  claim was that a capture leaving a scaled class removes defender material, so the raw gain
  over-states its value against the scaled baseline and such a move is never pruned for being too
  small. That is backwards. `quiescence()` compares `stand_pat + DeltaGain(move) + margin` against
  alpha, mixing a **scaled** stand-pat with a **raw** material gain. Leaving a scaled class raises the
  multiplier, so the child's true value *exceeds* the estimate and the raw gain **under**-states it.
  The deficit is `raw * (1 - scale/ENDGAME_SCALE_MAX)` — in K+R+minor vs K+R at 4/16 that is three
  quarters of the raw score, larger than the whole delta margin, so a winning capture of the
  defender's rook can be pruned when alpha already sits near the won value.

  The hole is new with the fractional scales: every scale-0 class has a bare defender, so no
  class-exiting capture exists to mispredict.

  Left unfixed deliberately, with the bound stated rather than a guard added. `pvs()` never calls
  `Evaluate()` and prunes no captures, so the error is confined to the deepest ply — the same capture
  is searched normally one ply up — and it needs alpha inside a window roughly one delta margin wide
  in a class reaching ~1-3% of games. A runtime guard would cost a classifier call at every quiescence
  node to close it. Tracked as a follow-up rather than paid for here.

## Validation

- **Step 1 (refactor):** `Compare-SearchEquivalence.ps1 -After <exe>` — identical node counts and best
  moves at `Threads=1`. No Elo match: the diff cannot change a score.
- **Cost, measured in part 1:** the classifier is free (+0.25% nps, seven interleaved pairs) once it
  reads the bitboards and counts nothing until past its own early-out. The first version cost 1.0–1.3%
  by materialising two count structs at every leaf. Worth knowing before parts 2 and 3 add work here:
  in this function, *what gets built per call* dominates *what gets compared*.
- **Step 2 (exact-draw classes):** `[eval]` regression cases for both colors and both sides to move,
  covering every exact-zero class under varying king/PST placement; the won-ending cases above; the
  mirror cases; and the UCI `eval` table reproducing `total`. Plus `Run-Bench.ps1` — the classifier
  runs at every qsearch leaf, so its cost is measured, not argued. No SPRT gate (D5).
- **Step 3 (scaled classes + mop-up gate):** the above, plus a bounded sweep over the two factors and
  an SPRT through `Run-EloMatch.ps1`, then `Scripts/analyze_move_quality.py` over the resulting PGNs
  compared against the K+minor/K, KR+minor/KR and KR/minor rows quoted in the Goal.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the classifier is in eval and not a search draw rule (D1) | source comment at the classifier |
| Why one white-POV assembly, and the odd-symmetry argument (D2) | source comment in `Evaluate()` |
| Why piece counts live in `EvalContext` (D3) | deferred with the field — nothing to harvest in part 1 |
| Why there is no `Mate_Threshold` guard (D4) | source comment at the scale application |
| Exact-draw vs. parameter split (D5, D6) | source comment on the scale constants |
| The wrong-bishop fortress condition (D7) | source comment at that classifier branch |
| Why the breakdown carries a scale row (D8) | source comment on the `EvalBreakdown` field |
| Measured class frequencies and the OCB negative | `Docs/Changelog.md`; already in #128 |
| Elo / calibration result of the scaled classes | `Docs/Changelog.md` and the PR body |
