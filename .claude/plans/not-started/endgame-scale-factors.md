# Drawish-Material Recognition / Endgame Scale Factors

**Issue**: #128 (carries #118 items 2 and 5) · **Epic**: #110 Tier 4 · **Depth**: design sketch · **Status**: not started
**Depends on**: #125 (color symmetry), #127 (EvalContext)

> Sketch, not an executable plan.

## Goal

Stop the evaluator claiming a win in positions that are drawn by material. Today:

| Position | Current score | Truth |
|---|---|---|
| KN vs K | ≈ +300 | dead draw (insufficient material) |
| KB vs K | ≈ +300 | dead draw |
| KNN vs K | ≈ +600 | draw against any defence |
| K + wrong-coloured B + rook pawn vs K | clear win | draw |
| KR vs KB / KR vs KN | ≈ +200 | usually held by the defender |

Every one is reachable by simplifying from a genuine advantage — precisely the moment when a wrong score
converts a win into a half-point, or a draw into a loss.

## Approach

A classifier at the tail of `Evaluate()`, after all terms are summed:

```
classify(material configuration) ->  DRAWN            -> return draw score
                                     SCALED(factor)   -> score = score * factor / SCALE_MAX
                                     NORMAL           -> score unchanged
```

Cheap: keyed on per-color piece counts, all available as popcounts over the `EvalContext` bitboards. No
table lookup, no new data structure.

### Decisions that look settled

- **Not a tablebase.** Syzygy support is #101, separately tracked and far larger. A piece-count classifier
  covers every case above at essentially zero cost.
- **Scale, don't clamp.** For "usually drawn but not certainly" endings (KR vs KB), multiply the score by a
  factor rather than forcing 0 — the stronger side still has practical chances and the search should still
  prefer the better version of the ending.
- **Exact draw only for genuinely insufficient material** (no pawns, and no mating force at all).
- **Wrong-coloured bishop** detection needs the bishop's square color and the rook pawn's file, both
  trivial from existing bitboards. This also resolves **#118 item 2**: `CenterManhattanDistance` treats all
  four corners equally, but a KBN mate can only be forced into the corner matching the bishop's color — the
  same square-color logic serves both, so implement it once here and have mop-up consume it.
- **Re-express the mop-up gate in terms of the defender's remaining force** — **#118 item 5**. The gate
  currently keys on the material *lead* (`MOPUP_MATERIAL_THRESHOLD`), so pawnless Q+R vs Q clears 400 cp
  and gets king-chasing scoring even though an enemy queen makes king-hunting a perpetual-check hazard.
  A "what can the defender still hold?" classifier answers both questions with one piece of logic; growing
  a second material test inside mop-up would not.

### Open questions

- **Where the classifier lives.** Naturally an eval concern, but a *search* that knows the position is
  drawn could also cut off early. Keep it in eval for now — a search-side draw claim interacts with the TT
  and repetition detection and is a much bigger change.
- **Whether to also recognise fortress-ish pawn endings** (KPK with the defending king in front). Real
  strength there, but it needs actual KPK knowledge, not piece counts. Out of scope; note as a follow-up.
- **Scale factor magnitudes** — #117's job.
- **Interaction with `Mate_Threshold`.** Scaling must never touch mate scores. A found mate is not subject
  to a drawish-material discount; guard explicitly on `abs(score) >= Mate_Threshold` before scaling, or a
  mate in a KR-vs-KB position gets silently scaled down.

## Measurement

Weak in a standard match — these endings are rare in an 8-move-book batch (the same reason mop-up measured
≈0). Do not expect an ELO result and do not run a 500-game batch expecting one.

**The real validation is targeted regression positions**: assert a near-zero score for each drawn
configuration in `[eval]`. That is a direct, unambiguous check of exactly the property being added, and it
is stronger evidence than any match at this budget. Consider a small set of tactical/endgame positions
where the engine currently walks into a drawn ending and should now avoid it.

Note the asymmetry that makes this worth doing anyway despite being unmeasurable: it can only *prevent*
half-point losses the engine currently walks into. There is no plausible mechanism by which correctly
scoring a dead draw as a draw loses games.

## Files likely touched

`StratEngine/Eval.h` (classifier enum, scale constants, square-color helpers), `StratEngine/Eval.cpp`
(classifier + application; mop-up gate rephrased), `StratChessTests/EvalTests.cpp`, `Docs/TestDesign.md`,
`Docs/Changelog.md`. Possibly `defines.h` for light/dark square masks — shared with #111, so whichever
lands first adds them.

## Test ideas

- KN vs K, KB vs K, KNN vs K, and bare KK all score ≈ 0 regardless of king placement.
- KB vs K with the bishop's color and a rook pawn present (wrong-coloured-bishop configuration) scores ≈ 0.
- KR vs KB scores positive but materially discounted relative to the unscaled term sum.
- KQ vs KR (the #70 mop-up case) is **not** scaled — it is genuinely won, and scaling it would undo #70.
- A mate score in a would-be-scaled configuration is not scaled (the `Mate_Threshold` guard).
- Pawnless Q+R vs Q no longer triggers mop-up king-chasing (#118 item 5).
- #125's mirror cases still pass — a classifier that treats White and Black differently is the obvious
  failure mode here.
