# Minor-piece outposts — Design

**Issue:** #112 (part of the #110 eval epic)

## Goal

Nothing in the evaluator pays a knight or bishop for standing on a square a friendly pawn defends and
no enemy pawn can advance to attack. The PST sees only piece type and square; safe mobility removes
squares enemy pawns cover *now*, and prices the squares a piece can move to rather than the one it
occupies; king-zone pressure is about the enemy king. So a supported white knight on d5 with no black
c/e-pawn and one a black e7-pawn can challenge with ...e6 score identically, although the first is a
long-term asset and the second is temporary. This adds an explicit, color-symmetric term for that
distinction.

## Scope

**This change will:**

- Add `Evaluator::eval_outposts(const EvalContext&, eColor) -> ScorePair`, scoring occupied knights
  and bishops on relative ranks 4–6 that are defended by a friendly pawn and have no enemy pawn on an
  adjacent file ahead of them.
- Wire it into `RawWhitePov`, `EvalBreakdown`, `Breakdown`, the UCI `eval` row and sum, and
  `BreakdownWhitePov` in the shared test fixture, in one change.
- Use the existing `g_bbPassedMask*` / `g_bbFileMask` tables and `ctx.pawn_attacks` — no new table,
  cache, attack generation or mutable state.

**This change will not:**

- Score empty outpost squares a minor could reach, unsupported minors, rooks, or reachability by an
  actual pawn move search.
- Add pawn hashing, king-safety interaction, bishop-specific restrictions (colour complexes,
  own-pawn blocking), or any tuning beyond the first-cut weights below (#117 owns tuning).
- Reuse `ComputePieceAggregates`' loop. That is a possible measured optimization later, not a
  prerequisite.

## Decisions

### D1: Detector definition — three geometric conditions

A minor on square `s` of colour `us` scores when all hold:

1. `RelativeRank(s, us)` is 4, 5 or 6.
2. `ctx.pawn_attacks[us] & (1 << s)` — a friendly pawn defends it now.
3. `ctx.pawns[them] & forward_span(s, us) & ~g_bbFileMask[File(s)]` is empty.

Condition 3 uses the **piece colour's** passed-pawn span, minus its own file. For a white piece on d5
that selects black pawns on c6–c8/e6–e8: exactly the pawns that can still step to c6/e6 and attack
d5. A black pawn on c5 or e4 is already past that chance and must not disqualify. A same-file pawn
can block the piece but can never attack it, hence the file exclusion.

Rejected: enumerating legal pawn advances, and clearing the bonus when a challenger is blocked or
pinned. Both cost more than the term is worth and neither is what the masks make cheap. What this
detector states is "no enemy pawn on an adjacent file ahead, under a same-file advance model" — a
structural proxy, not a proof of permanent safety. It ignores blockers, pins, tempo and captures that
change pawn files in either direction; a blocked adjacent pawn still disqualifies, and a pawn that
could later capture onto an adjacent file does not. `pawn_attacks` is likewise geometric, so a pinned
friendly pawn still counts as support.

### D2: Ranks 4–6 only

Relative rank 3 is usually still home territory and rank 7/8 minors are near-trapped and already
handled by PST/mobility; paying an outpost bonus there would reward a piece that is often about to be
rounded up. Rejected: rank 3+ (a supported knight on its own third rank is not an outpost) and
"anything the enemy cannot challenge" without a rank gate.

### D3: Phase-neutral first-cut weights

| Relative rank | Knight | Bishop |
|---|---:|---:|
| 4 | 15 cp | 8 cp |
| 5 | 20 cp | 12 cp |
| 6 | 25 cp | 16 cp |

Knights gain more from an unchallengeable advanced square than bishops, which act at range anyway.
`mg == eg` deliberately: a phase split is a second unmeasured axis, and one experiment should move
one thing. These are an untuned hypothesis in the range the historical sketch proposed, not values
with evidence behind them.

### D4: A standalone scan, not a hook in the aggregate loop

`eval_outposts` intersects the knight and bishop bitboards with `ctx.pawn_attacks[us]` *before*
scanning, so the loop body runs only for minors that are already pawn-defended — usually zero or one
per side. Folding it into `ComputePieceAggregates` would couple an untested term to the hot attack
pass for a saving that is not yet known to exist.

## Assumptions I cannot verify from the code

None outside the repository. Every input (`g_bbPassedMaskWhite/Black`, `g_bbFileMask`,
`ctx.pawn_attacks`, `RelativeRank`) is in-tree and covered by tests; the term's own behaviour is
asserted directly rather than inferred from totals.

The one unverifiable claim is a *forecast*: that the term is worth positive Elo. The epic's 5–20 Elo
figure is a roadmap hypothesis, not a measurement of this engine. Settled only by the match below.

## Invariants

- Colour symmetry: a mirrored FEN yields the mirrored score (`MirrorFen` cases must include a
  position where the term is actually active, or they prove nothing about it).
- Breakdown reconstruction: white-minus-black term rows + `endgame_adjustment`, then the
  side-to-move sign, equal `Evaluate()`. The new row is added before scaling, never after.
- `endgame_scale == 0` still returns `GameValues::Draw` without evaluating any term.
- No dependence on piece counts: the scan loops bitboards, so promoted or multiple minors all score.

## Validation

Engine tier. Fast + Debug tests, lint, `Validate-PrePR.ps1`, Linux Debug/sanitizer CI.

- Term-level tests in `EvalTermTests.cpp` for each condition, both boundary ranks, both adjacent
  files, edge files (no wrap), a same-file pawn, a pawn already past the span, a currently-attacking
  pawn (c6 vs Nd5), blocked/pinned challengers, pinned friendly support, and multiple minors.
- `EvalSymmetryTests.cpp` and the breakdown/UCI reporting tests get an outpost-positive FEN.
- Search equivalence does **not** apply: this changes the evaluation on purpose.
- nps: repeated interleaved same-toolchain Release `Run-Bench.ps1` at `Threads=1`, reporting
  per-position and aggregate nps plus spread. Changed weights change the search's workload, so
  aggregate nps alone does not isolate detector cost. <1% is an investigation threshold near timing
  noise, not a proven bound.
- Elo: the 19,980-game CI strength lab against the pre-change reference, same toolchain both sides,
  recorded in `Measurements/ci-per-change.md`. Requires the owner's go-ahead (~3 h, 18/20 CI slots).
  Ship on a 95% interval wholly above zero; an interval overlapping zero is inconclusive, not proof
  of neutrality — hold, ablate (knight-only, its own budget) or retune.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 mask orientation, same-file exclusion, and what the proxy does *not* mean | source comment on `eval_outposts` |
| D2 rank gate rationale | source comment on the weight table |
| D3 weights are an untuned hypothesis | source comment on the weight table |
| Measured nps and Elo | `Measurements/ci-per-change.md`, `Docs/Changelog.md`, PR body |
| New term row + tests | `Docs/TestDesign.md`, `Docs/Changelog.md` |
