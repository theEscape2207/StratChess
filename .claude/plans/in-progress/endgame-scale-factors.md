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

**Changed during implementation.** Part 1 was built with the field and then took it out again: the
classifier and the mop-up gate are both computed *inside* `BuildContext`, so they share one model
through locals and the context field had a single reader in the same function. It bought nothing and
made the per-call context larger. The classifier now reads the bitboards directly and counts only
what it needs, past its own early-out. The decision stands for the mop-up re-expression, which is
where a second consumer actually appears; the field comes back then.

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
- **Delta pruning stays sound.** `quiescence()` compares `stand_pat + DeltaGain(move) + margin` against
  alpha, mixing a scaled stand-pat with a raw material gain. A capture that leaves a scaled class does
  so by removing defender material, and the raw gain over-states its value relative to the scaled
  baseline, so such a move is never pruned for being too small. Asserted here as reasoning, tested by
  the equivalence and bench runs rather than by a unit test.

## Validation

- **Step 1 (refactor):** `Compare-SearchEquivalence.ps1 -After <exe>` — identical node counts and best
  moves at `Threads=1`. No Elo match: the diff cannot change a score.
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
