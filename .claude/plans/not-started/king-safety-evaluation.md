# King Safety Evaluation

**Issue**: #97 · **Epic**: #110 Tier 2 · **Depth**: design sketch · **Status**: not started
**Depends on**: #127 (EvalContext), #99 (phase weighting — mandatory here), #98 (attacker sets)

> Sketch, not an executable plan. Highest ceiling in the epic and the highest regression risk; this
> captures the design shape and the risks so the eventual full plan starts from the right place.

## Goal

Score how exposed each king is: pawn shield, open/half-open files near the king, and enemy attack
pressure on the squares around it. Currently entirely absent — the king contributes one PST value and
nothing else.

## Why it is sequenced last in Tier 2

Two reasons, both structural rather than about effort:

- **It needs a real phase.** King safety must fade to near-nothing in the endgame, where the king becomes
  an active piece and centralization is correct. Bolted onto the current binary stage switch, it would
  either apply full king-safety pressure in a rook endgame or vanish abruptly at the threshold. #99 is a
  hard prerequisite, not a convenience.
- **It reuses #98's attack sets.** Counting attackers in the king zone means asking "which enemy pieces
  attack these squares" — the same per-piece attack sets mobility already computes. Doing #97 first means
  computing them twice, then deduplicating later.

## Approach

Three sub-terms, conventionally combined:

**1. Pawn shield.** Count friendly pawns on the three files in front of the king, weighted by how far
advanced they are (a pawn still on its start square shields better than one pushed two squares). Missing
shield pawns are penalised. Needs the king-zone file masks; reuses `g_bbFileMask` plus the forward-span
masks #116 adds.

**2. King-zone open files.** An open or half-open file adjacent to the king is a penalty — the mirror of
the rook's open-file *bonus*, and it should use the same corrected definition #126 establishes (pawns
only, not all pieces). Worth reusing the same helper so the two definitions cannot drift apart.

**3. Attack-square scoring.** Define a king zone (the king's square plus its 8 neighbours, commonly
extended a rank forward). For each enemy piece attacking a zone square, accumulate a weight by piece type,
then map the accumulated count through a **non-linear** table: two attackers are far more than twice as
dangerous as one. This non-linearity is the part that produces the real strength gain and also the part
most likely to be mis-tuned.

## Risks — the reason this needs care rather than just effort

- **Non-linear terms can dominate.** An attack-weight table that saturates too aggressively produces an
  engine that sees phantom attacks and sacrifices material to prevent them. Cap the term's total
  contribution explicitly, and treat that cap as a first-class tunable rather than an afterthought.
- **Double-counting with the king PST.** The midgame king PST is already a blunt safety proxy (uniformly
  −40 on ranks 8-3, −20 on rank 2, 0 on rank 1 — i.e. "stay home"). A real king-safety term makes that
  crude table redundant and possibly harmful. Expect to flatten or remove the midgame king PST as part of
  this work, and say so in the plan rather than discovering it via a confusing measurement.
- **Asymmetric tables arrive here.** Castling-side-specific shield weighting is file-asymmetric — which is
  precisely the case #125's mirroring fix exists to make safe. #125 is a hard prerequisite; verify its
  mirror test still passes with any asymmetric table introduced.
- **Interaction with #115 (castling-done).** #115 is described in the epic as a stepping stone to this
  work. Once #97 lands, revisit whether #115's term still earns its place, rather than leaving two
  overlapping king-placement terms pulling against each other.
- **Cost.** Adds per-node work on top of #98's. Measure nodes/second.

## Measurement

Expected to be among the largest gains in the epic, and also the most likely to regress if mis-tuned.
Use SPRT (#130). Run **both** presets in sequence if budget allows: `NonRegression` first (does it hurt?),
then `Gain`. Self-play is unusually informative here — an over-weighted king-safety term produces visibly
paranoid play, and an under-weighted one produces kings that get mated in the middlegame.

Consider landing the three sub-terms as separate PRs (shield → open files → attack scoring), each measured.
Attribution matters more than usual because the attack-scoring sub-term carries nearly all the tuning risk.

## Files likely touched

`StratEngine/Eval.h` (zone masks, weight tables, caps), `StratEngine/Eval.cpp`, possibly `defines.h`
(king-zone mask generation following the `makeFileUpMask()` pattern),
`StratChessTests/EvalTests.cpp`, `Docs/TestDesign.md`, `Docs/Changelog.md`.

## Test ideas

- An intact three-pawn shield scores higher than the same position with the g-pawn pushed, and higher
  still than with it missing.
- A king next to an open file scores lower than the same king with a pawn on that file.
- One enemy piece attacking the king zone costs less than half what two cost (the non-linearity — assert
  the inequality, not exact values).
- The term shrinks toward zero as phase decreases (the #99 dependency, tested directly).
- The total contribution never exceeds the declared cap, however many attackers are piled on.
- #125's mirror cases still pass — including with any castling-side-asymmetric table.
