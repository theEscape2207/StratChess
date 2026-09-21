# KBN vs K: drive the losing king to a corner of the bishop's colour — Design

**Issue:** #572

## Goal

In a 19,980-game lab corpus, 42 games reached K+B+N vs K and none ended in a delivered mate; of the
18 not adjudicated away, all 18 were drawn. `eval_mopup` is the only term that converts a won
pawnless ending, and it rewards driving the losing king toward **any** corner
(`MOPUP_CMD_WEIGHT * CenterManhattanDistance(loserKingSq)`). That is correct for K+Q vs K and
K+R vs K, where every corner mates, but KBN vs K mates only in a corner of the **bishop's colour**,
so in half of all positions the term steers the conversion at the one corner where no mate exists
and the fifty-move clock arrives first. Two of the drawn games are confirmed `cursed-win` by the
Lichess tablebase — won, but no longer inside the fifty-move rule — which is the defect stated by an
exact oracle rather than inferred from an outcome. The fix is knowledge, not a weight: the term needs
a bishop-colour-aware corner target for this one material class.

## Scope

**This change will:**

- Detect the exactly-bishop-and-knight-versus-bare-king class inside `eval_mopup` and, for it,
  replace the centre-distance component with a corner-distance component keyed on the bishop's square
  colour. The king-distance component is unchanged.
- Add one weight constant, `MOPUP_KBN_CORNER_WEIGHT`, in `Eval.h` beside the existing mop-up weights.
- Add term-level `[eval]` tests (correct corner beats wrong corner; the class is recognised exactly;
  colour symmetry) and one `[slow]` conversion test that plays KBN vs K out with the production
  search and requires a mate inside the fifty-move limit.
- Add the conversion test and its rationale to `Docs/TestDesign.md`.

**This change will not:**

- Add any tiebreak that prefers keeping material among equally-drawn moves. The 7 games that lost the
  bishop or knight sit at halfmove clock 95–98 where every continuation is correctly drawn; the
  evaluation is right there and a tiebreak would hide the conversion failure rather than fix it
  (#572, comment of 2026-09-21). Those positions stay in the issue as evidence.
- Change `MOPUP_CMD_WEIGHT`, `MOPUP_KINGDIST_WEIGHT`, `MOPUP_MATERIAL_THRESHOLD` or the mop-up gate.
  Every other mop-up class must score exactly as it does today.
- Touch `EndgameScale()`. K+B+N vs K already scales at `ENDGAME_SCALE_MAX`, and K+minor vs K already
  scales to 0.
- Add mating-net knowledge for any other class (two bishops, three minors), or endgame tablebases.

## Decisions

### D1: Replace the centre-distance component for this class, do not add to it

For exactly B+N, `MOPUP_CMD_WEIGHT * CenterManhattanDistance(loserKingSq)` is dropped and the corner
component takes its place.

Rejected: keeping both. `CenterManhattanDistance` is 6 at all four corners, so a wrong corner keeps a
full centre-distance bonus while the corner component pays 0 there — a local maximum the search can
settle into. With both at weight 10 and a dark-squared bishop, the Black king on a8 scores 60 while
b7 scores 40: the term would still reward parking the king in the corner where no mate exists. A
plain replacement makes the wrong corner worth the same as the centre, which is what it is worth.

Rejected: re-weighting `MOPUP_CMD_WEIGHT`/`MOPUP_KINGDIST_WEIGHT`. Neither knob can express bishop
colour, and raising them drives the king to the wrong corner harder — this issue is the worked
example #593 cites for why retuning cannot reach it.

### D2: The corner metric is the Manhattan distance to the nearer correct corner, as one absolute difference

With `file = File(sq)` and `row = Rank(sq)` (row 0 = rank 8, so a8 = (0,0), h1 = (7,7)):

- light-squared bishop — mating corners a8 and h1: `AbsDiff(file + row, 7)`
- dark-squared bishop — mating corners a1 and h8: `AbsDiff(file, row)`

Each is exactly `7 - min(Manhattan distance to the two mating corners)`, so it peaks at 7 on the two
correct corners, is 0 on the two wrong ones, and has a nonzero gradient off the losing diagonal
everywhere else. Range 0..7 against `CenterManhattanDistance`'s 0..6, so the component it replaces
keeps a comparable span.

Rejected: a 64-entry corner-proximity table. Two absolute differences need no table, no mirroring
logic and no initialisation, and the identity above is what makes the expression readable.

The wrong corners and the board centre share the floor value of 0. That is accepted: the maxima are
the two squares that matter, and the king-distance component plus the winner's own pursuit supply the
rest. This is the same shape Stockfish used for its KBNK specialisation.

### D3: `MOPUP_KBN_CORNER_WEIGHT = 10`, a new constant rather than a reuse of `MOPUP_CMD_WEIGHT`

10 matches the weight of the component it replaces, so this class's mop-up score stays in the band it
occupies today (0..70 against 0..60) instead of introducing a new magnitude into a term other things
are calibrated against. A distinct name records that the two are not the same knob: a later
centre-distance retune must not silently move the corner target.

Rejected: a much larger weight in the style of Stockfish's KBNK function. That value sits inside a
known-win score, not inside a normally-assembled evaluation, so it does not transfer. If the
conversion test in Validation fails at 10, this weight is the first thing to raise, and this document
records that as the expected direction.

### D4: Detect the class inside `eval_mopup`, not in `BuildContext`

`eval_mopup` already returns immediately unless `ctx.mopup_active[color]`, and it has `ctx.boards`.
Putting the piece-count test behind that early-out costs nothing on the overwhelming majority of
nodes, which never reach a mop-up position at all.

Rejected: a new `EvalContext` field. `BuildContext` runs on every evaluated node, so the popcounts
would be paid everywhere to serve a vanishing fraction of positions — the hot-path trap recorded for
this engine, where a live per-node check has measured ~2% nps. Nothing outside `eval_mopup` needs the
answer; `eval_pst` keys on `mopup_active` alone and is unaffected.

### D5: The class test is the winner's exact piece set, plus an explicit bare-loser check

Active only when the winning colour has exactly one knight, exactly one bishop, no rook and no queen,
**and** the losing colour has nothing but its king.

The loser check is redundant today — with B+N worth 600 and `MOPUP_MATERIAL_THRESHOLD` at 400, a
defender holding any piece (a minor is 300) fails the gate, and pawns are already excluded — but it is
the condition the corner target actually depends on: a defender with a piece of its own is not a
basic mate and its king is not the thing to corner. Deriving it from the threshold would make the
term wrong the day that threshold moves, in the same way the kingless guard in `BuildContext` is
written out rather than inferred.

## Assumptions I cannot verify from the code

- **The bishop's square colour determines the mating corner, and KBN vs K is a forced mate within 50
  moves from every position.** Chess theory, not a repository fact. Verified by the conversion test in
  Validation, which mates from starting positions of both bishop colours.
- **The two tablebase-confirmed positions from the issue are not usable as mate-in-N oracles.** Both
  stand at halfmove clock 94 with DTZ 46 and 44, so under the fifty-move rule even perfect play draws
  them; `cursed-win` says exactly that. The conversion test therefore starts from positions with a
  zeroed clock, and those two FENs are kept as evidence only. This follows from the DTZ figures in the
  issue and needs no further verification, but it contradicts the issue comment that offers them as
  regression positions, so it is called out rather than left implicit.

Nothing else here depends on behaviour outside this repository.

## Invariants

- Every mop-up position that is not exactly B+N versus a bare king scores bit-identically to today —
  K+Q vs K, K+R vs K, K+Q+Q vs K, and the defended classes the gate admits.
- No position outside the mop-up gate changes at all, so a search over positions with pawns visits
  identical nodes.
- The term is colour-symmetric: a colour mirror flips both the bishop's square colour and the mating
  corners, and `AbsDiff(file + row, 7)` maps onto `AbsDiff(file, row)` under `row -> 7 - row`, so the
  mirrored position must score the same.
- Only the winning colour receives a nonzero mop-up contribution, unchanged.

## Validation

Change tier: engine. `Validate-PrePR.ps1` scopes itself.

- **Term-level `[eval]` tests.** One variable moved per case: with both kings and the knight fixed and
  the losing king on a8, `Mopup()` is higher when the bishop stands on a light square than on a dark
  one, and the reverse with the losing king on a1. Holding the kings still keeps the king-distance
  component out of the comparison. Then: a K+Q vs K and a K+R vs K position keep their current values;
  a K+B+N vs K+B position (gate closed by the material threshold) and a K+Q+B+N vs K position (queen
  present) get no corner component.
- **Colour symmetry.** A KBN FEN added to `kSymmetryFens` in `EvalTestFixture.h`; the existing mirror
  test then covers the invariant above. This FEN is the one case in that list whose mirror changes the
  bishop's square colour, which is exactly what a sign error in D2 would break.
- **Conversion test, `[slow]`.** From four KBN vs K positions with halfmove clock 0 — both bishop
  colours, and a defending king starting nearer the wrong corner than the right one — play the
  position out with `AIPerplex` at a fixed depth and `Threads=1`, applying each returned best move,
  and require `game_state` to report a mate before the halfmove clock reaches
  `HALFMOVE_CLOCK_LIMIT`. This is the test that fails on `origin/main` and is the arbiter of whether
  the term is strong enough (D3). Falsified the standard way: revert the corner component and watch it
  fail.

  The fixed depth is not a free parameter: it is the lowest that converts, measured during
  implementation and recorded in the test's comment together with its runtime. The order of recourse
  if it does not converge is fixed here so it is not improvised later — raise
  `MOPUP_KBN_CORNER_WEIGHT` first (D3), then the test depth, and if neither converts, the corner
  target alone is not sufficient knowledge for this mate and that is the finding to report rather
  than a term to ship.
- **Confinement.** `Compare-SearchEquivalence.ps1 -After <built exe>` over its default positions,
  which all have pawns and cannot reach a pawnless B+N leaf at the script's depth: identical node
  counts and best moves, showing the eval change is confined to the gated class. A node difference
  there means the class test admits more than it should.
- **Bench pass.** `Run-Bench.ps1` before and after. The added work sits behind an early-out that the
  bench positions never pass, so the expectation is no nps change outside noise; a measured drop would
  mean the detection was compiled in front of the early-out.
- **No Elo match.** The evaluation changes only in pawnless B+N-versus-bare-king positions, which
  occurred in 42 of 19,980 lab games and were adjudicated away in 24 of them. An SPRT cannot resolve a
  class that rare and would spend hours to report nothing; the conversion test is the direct
  instrument for the defect, and confinement plus the bench pass close the risk of harm elsewhere.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the corner component replaces rather than adds to centre distance, with the wrong-corner local maximum (D1) | source comment in `eval_mopup` |
| The `7 - Manhattan-to-nearer-correct-corner` identity behind the two absolute differences (D2) | source comment on the new helper in `Eval.h` |
| Why the corner weight is a separate constant from `MOPUP_CMD_WEIGHT` (D3) | comment beside `MOPUP_KBN_CORNER_WEIGHT` in `Eval.h` |
| Why the class test is inside `eval_mopup` and not in `BuildContext` (D4) | source comment in `eval_mopup` |
| Why the bare-loser check is written out rather than derived from the material threshold (D5) | source comment in `eval_mopup` |
| That the two `cursed-win` FENs are evidence, not mate-in-N oracles | comment on #572, and the conversion test's comment |
| The conversion test and what it costs | `Docs/TestDesign.md` |
| Outcome of the conversion test, before and after | PR body, `Docs/Changelog.md` |
