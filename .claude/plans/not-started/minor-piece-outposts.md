# Knight / Bishop Outpost Bonus

**Issue**: #112 · **Epic**: #110 Tier 4 · **Depth**: design sketch · **Status**: not started
**Depends on**: #127 (EvalContext), #116 (forward-span masks + pawn attack sets)

> Sketch, not an executable plan.

## Goal

Reward a knight or bishop on an **outpost**: a square that no enemy pawn can ever attack, defended by a
friendly pawn. Today the `WHITE_KNIGHT`/`BLACK_KNIGHT` and `WHITE_BISHOP`/`BLACK_BISHOP` cases in the piece
switch are empty — minor pieces get material + PST and nothing else.

## Approach

Two conditions, both computable from masks #116 already introduces:

1. **Cannot be attacked by an enemy pawn, ever.** No enemy pawn on either adjacent file, ahead of the
   square. This is exactly the adjacent-file half of #116's passed-pawn span mask — so reuse
   `g_bbPassedMask*` rather than generating a second nearly-identical table. If the masks end up shaped
   awkwardly for this use, factor a shared generator rather than duplicating one.
2. **Defended by a friendly pawn.** The square is in the friendly pawn attack set (`EvalContext`, added
   by #116).

Restrict scoring to ranks 4-6 relative to the piece's color — an "outpost" on your own third rank is just
a piece standing on a square, and scoring it dilutes the term.

### Decisions that look settled

- **Knights score higher than bishops on outposts.** A knight's value comes disproportionately from a
  secure advanced square; a bishop on a long diagonal does not need the pawn support in the same way.
  Separate constants.
- **Reuse #116's masks, do not generate new ones.** Two near-identical mask tables that can drift apart is
  worse than a slightly awkward shared one.
- **Bonus scales with rank.** An outpost on the 6th is worth more than one on the 4th.

### Open questions

- **Whether the pawn-defended clause should be required or merely a bonus multiplier.** Some engines score
  an unattackable square as a partial outpost even without pawn support. Requiring it is stricter and
  easier to tune; start there.
- **Whether to also score the *square* rather than only an occupied square** (i.e. reward controlling an
  outpost square a piece can move to). Standard in strong engines, meaningfully more expensive, and out of
  scope for a first version. Note it for #117 / a follow-up.
- **Magnitude** — literature suggests 15-30 cp for a knight outpost. Unknown for this engine; #117's job.

## Measurement

10–20 Elo expected, i.e. **inside the ±25 Elo noise floor of a 500-game batch**. SPRT (#130) `Gain` preset,
or bundle with another Tier 4 term. See `eval-quick-win-terms.md` for the same problem stated at length —
the same rules apply here, including: do not record a fixed-batch result for this term as if it resolved
anything.

## Files likely touched

`StratEngine/Eval.h` (outpost constants, rank multipliers), `StratEngine/Eval.cpp` (term function),
`StratChessTests/EvalTests.cpp`, `Docs/TestDesign.md`, `Docs/Changelog.md`.

Ideally no `defines.h` change at all — that is the point of reusing #116's masks.

## Test ideas

- A knight on d5 supported by a c4 pawn, with no black pawn able to reach c6/e6, gets the bonus.
- The same knight loses the bonus once a black pawn appears on e7 (it can advance to e6 and attack).
- The same knight loses the bonus if the supporting c4 pawn is removed (the defended clause).
- A knight on d3 (own third rank) gets nothing, however well supported — the rank restriction.
- A bishop in the same configuration scores lower than the knight did.
- An outpost on the 6th scores higher than the same configuration on the 4th.
- #125's mirror-symmetry cases still pass — the rank restriction is the likely place to get the color
  orientation wrong, given the `a8 = 0` board layout means White's "advanced" is a *decreasing* index.
