# Mobility Evaluation (+ Queen Activity)

**Issues**: #98, #113 · **Epic**: #110 Tier 2 · **Depth**: design sketch · **Status**: not started
**Depends on**: #127 (EvalContext), #99 (phase-weighted mobility)

> Sketch, not an executable plan. Written to capture the design decisions that are already settled and
> the questions that are not, so this can be turned into a full plan when it comes up in the sequence.
> Do not treat the numbers here as chosen.

## Goal

Score each piece by the number of squares it can move to, weighted per piece type and per phase.
Historically one of the largest single ELO contributors in engine-evaluation history. #113 (queen
activity) is folded in here rather than tracked separately — it exists as its own issue only to guarantee
the queen is not skipped if mobility scoped down to "cheap pieces first", and this sketch covers the
queen from the start, so that risk is retired.

## Approach

**The expensive part is already done.** The original roadmap note flagged mobility as "expensive to
compute, maybe cache". PEXT magic bitboards (#108) made that largely obsolete: `RookAttacks(sq, occ)` and
`BishopAttacks(sq, occ)` (`Magic.h`) return full sliding attack sets from a table lookup, so rook, bishop
and queen mobility is a `std::popcount` over a bitboard the engine can produce in a few instructions.
Knight mobility comes from the precomputed knight-move tables in `defines.h`.

Sketch of the per-piece computation:

```
attacks = <piece attack set given ctx.all_pieces>
safe    = attacks & ~ctx.occupied[own]        // cannot move onto own pieces
score  += MOBILITY_WEIGHT[pieceType] * popcount(safe)
```

### Decisions that look settled

- **Pseudo-legal, not legal, mobility.** Filtering for check-legality would require move generation per
  piece per node. Every strong engine uses pseudo-legal counts here; the distinction is noise relative to
  the term's weight.
- **Exclude squares attacked by enemy pawns** from the count (the standard "safe mobility" refinement).
  A square a pawn covers is not a square a knight can usefully occupy. Needs the pawn attack sets that
  #116's backwards-pawn term already puts in `EvalContext` — so sequence after #116 if convenient, or add
  them here.
- **Per-piece-type weights, phase-split.** Weights differ substantially by piece (a rook's extra square is
  worth less than a knight's), and mobility generally matters more in the midgame. Return `(mg, eg)` pairs
  per #99's convention.
- **Do not double-count with PSTs.** Mobility and the existing PSTs both reward central placement; adding
  a strong mobility term on top of unchanged PSTs can overweight centralization. Expect this to show up
  as a smaller-than-hoped ELO gain and treat it as a #117 retuning input, not a bug.

### Open questions

- **Weight magnitudes** — genuinely unknown for this engine, and this term is unusually sensitive to them.
  Candidate approach: land with conservative literature-standard weights, measure, then let #117 tune. Do
  not spend days hand-tuning; that is what #117 exists for.
- **Whether to count squares occupied by enemy pieces** (capture targets) as mobility. Both conventions
  exist. Pick one, comment it, and don't mix.
- **Cost.** This is the first term that adds real per-node work. `Evaluate()` is only called from
  `AIPerplex::quiescence`, which bounds the damage, but measure nodes/second before and after at fixed
  depth. If throughput drops enough to cost search depth, the term can be net-negative even if the scores
  are better — that is the actual risk here, not correctness.

## Measurement

Expected to be one of the larger gains in the epic, so plausibly resolvable by a 500-game batch — but use
SPRT (#130) `Gain` preset if available. **Report nodes/second alongside Elo**: a positive Elo result that
came with a 15% throughput loss means the term is stronger than it looks; a neutral result with a
throughput loss means it is a net regression being masked.

## Files likely touched

`StratEngine/Eval.h` (weight tables), `StratEngine/Eval.cpp` (term function),
`StratChessTests/EvalTests.cpp`, `Docs/TestDesign.md`, `Docs/Changelog.md`.

## Test ideas

- A knight on a central square scores higher than the same knight in a corner (the canonical case).
- A rook on an open file scores higher than a rook boxed in behind its own pawns.
- Adding an enemy pawn that covers a square the knight could reach reduces the knight's mobility score
  (the safe-mobility refinement).
- Blocking a bishop's diagonal with an own piece reduces its score; blocking with an enemy piece behaves
  per whichever capture-target convention was chosen.
- #125's mirror-symmetry cases still pass.

## Notes for later issues

- #97 (King Safety) will reuse the per-piece attack sets computed here to count attackers in the king
  zone. Structure the term so those sets are available to the king-safety term rather than computed twice
  — probably by accumulating a per-color attack-set union into `EvalContext` as a side effect.
- #118 item 3 (mop-up has no stalemate/mobility awareness for the losing king) becomes implementable once
  a mobility count exists; the regression positions that issue asks for should land with this work.
