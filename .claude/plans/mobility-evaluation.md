# Mobility Evaluation (+ Queen Activity)

**Issues**: #98, #113 · **Epic**: #110 Tier 2 · **Status**: ready to execute
**Supersedes**: the design sketch of 2026-07-24, whose measurement section predates the CI strength lab

## Goal

Score each piece by how many squares it can move to, weighted per piece type and per phase. This is
historically one of the largest single Elo contributors in engine evaluation, and it is the last
Tier 2 item in epic #110.

#113 (queen activity) is delivered here rather than separately. It exists as its own issue only to
guarantee the queen is not skipped if mobility scoped down to "cheap pieces first"; this plan covers
the queen from the start, so that risk is retired and #113 closes with #98.

## Scope limits

- **Knight, bishop, rook, queen only.** Not the king — king mobility is a king-safety signal and
  belongs to #97, where it can be weighed against attacker counts rather than paid as a flat bonus.
  Not pawns — pawn structure is `eval_pawns`.
- **No weight tuning.** Land conservative literature-standard weights, measure, and let #117 tune the
  full term set in one pass. Do not spend days hand-tuning; that is what #117 is for.
- **No king-safety groundwork.** See D7.

## What changed since the sketch

Verified against `main` at `8bcdd28` rather than assumed.

| Sketch assumption | Reality now |
|---|---|
| "Depends on #127 (EvalContext)" | **Closed and in the code.** `BuildContext`, `EvalContext`, per-term `ScorePair` functions all exist |
| "Depends on #99 (phase-weighted)" | **Closed.** Terms return `ScorePair{mg, eg}`; `Evaluate()` blends per term |
| "sequence after #116 if convenient" | **#116 is still open**, so this work adds the pawn attack sets itself (D2) |
| "use SPRT (#130) `Gain` preset if available" | **Obsolete.** The CI lab resolves ±4.15 at 20,000 games; see Measurement |
| "plausibly resolvable by a 500-game batch" | **Wrong.** 500 games locally is ±25-26 and cannot resolve this |
| "#118 item 3 should land with this work" | **#118 was closed as COMPLETED** on 2026-07-29 with item 3 explicitly deferred to post-#98 — see Carried Work |

**The groundwork was laid deliberately.** `EvalContext::occupied[NUM_COLORS]` and `all_pieces` are
already populated and carry the comment *"Unread by any term today … both are populated because issue
#98 (Mobility) needs per-color occupancy to mask off blocked squares. Do not remove as dead code."*
This term is what makes them live.

## Design decisions

**D1 — Pseudo-legal, not legal, mobility.** Filtering for check-legality needs move generation per
piece per node. Every strong engine counts pseudo-legal squares here; the distinction is noise
relative to the term's weight and unaffordable at eval frequency.

**D2 — Safe mobility: exclude squares attacked by enemy pawns.** A square a pawn covers is not a
square a knight can usefully occupy. #116 would have put pawn attack sets in `EvalContext`; it is
still open, so add them here. `MoveGenerator::GeneratePawnCaptures` (`MoveGenerator.cpp:64-72`)
already computes exactly these sets — mirror its file-masked shifts rather than inventing new ones,
so the two cannot disagree about edge files.

**D3 — Count squares occupied by enemy pieces.** The mask is `attacks & ~ctx.occupied[own]`, so a
capture target counts as mobility. Both conventions exist in the literature; this one is chosen
because it needs one mask rather than two, and because a piece that can capture is genuinely active.
**Comment the choice at the mask** — mixing conventions between piece types is the failure mode.

**D4 — Per-piece-type weights, phase-split.** A rook's extra square is worth less than a knight's,
and mobility generally matters more in the midgame. Weights are `(mg, eg)` pairs per piece type,
returned as a `ScorePair`.

**D5 — Do not double-count with the PSTs.** Mobility and the existing PSTs both reward central
placement. Expect a smaller gain than the literature suggests, and treat it as a #117 retuning input
rather than a bug in this term.

**D6 — One term function, all four piece types.** `eval_mobility(ctx, color)` matching the existing
`eval_*` shape, rather than four functions. The attack-set computation is the shared cost; splitting
it would recompute occupancy masks per piece type and add four rows to the breakdown for one concept.

**D7 — Do NOT accumulate an attack-set union into `EvalContext` for #97.** The sketch suggested this
"as a side effect". It is incompatible with the current structure: term functions take
`const EvalContext&` and return a `ScorePair` — they cannot mutate the context, and making them able
to would break the purity that lets `Evaluate()` be `const` and thread-safe under Lazy SMP. When #97
needs the sets, the right move is for `BuildContext` to compute them, not for a term to leave them
behind. Note it there; build nothing now.

## Carried work

**#118 item 3** (mop-up has no stalemate/mobility awareness for the losing king) was deferred until a
mobility count existed, but #118 closed as COMPLETED, leaving the follow-up tracked nowhere. It is
now **#234**, filed separately on purpose: it is mop-up-scoped rather than mobility-scoped, so
bundling it would widen this change's diff for an unrelated reason.

Nothing here blocks on #234. The dependency runs the other way — #234 becomes implementable once this
term makes a king's safe-square count available.

## Files changed

| File | Change |
|---|---|
| `StratEngine/Eval.h` | `MOBILITY_*` weight tables; `pawn_attacks[NUM_COLORS]` in `EvalContext`; `mobility` field in `EvalBreakdown`; `eval_mobility` declaration |
| `StratEngine/Eval.cpp` | `eval_mobility()`; pawn attacks in `BuildContext()`; wire into `Evaluate()` and `Breakdown()` |
| `StratChessTests/EvalTests.cpp` | Term tests (see Validation) |
| `Docs/TestDesign.md` | Coverage map entry |
| `Docs/Changelog.md` | Dated entry |

## Steps

1. **Pawn attack sets.** Add `BITBOARD pawn_attacks[NUM_COLORS]` to `EvalContext`; populate in
   `BuildContext()` from `ctx.pawns[]`, mirroring `MoveGenerator.cpp:64-72`. Comment that #116 will
   want these too.
2. **Weights.** Add `MOBILITY_KNIGHT_MG/EG`, `..._BISHOP_*`, `..._ROOK_*`, `..._QUEEN_*` to `Eval.h`
   beside the existing term constants. Conservative starting values; comment that #117 owns tuning.
3. **`eval_mobility(ctx, color)`.** For each of the four piece types, iterate that piece's bitboard;
   attacks from `g_bbKnightMoves[sq]` (knight), `BishopAttacks(sq, ctx.all_pieces)`,
   `RookAttacks(sq, …)`, or their union (queen); mask with
   `~ctx.occupied[own] & ~ctx.pawn_attacks[enemy]`; `std::popcount`; accumulate weighted.
   `Magic.h` is already included in `Eval.cpp` (for #114).
4. **Wire into `Evaluate()`** — add `+ BlendPhase(eval_mobility(ctx, c), ctx.phase)` to the blend loop.
5. **Wire into `Breakdown()`** and add the `EvalBreakdown::mobility` field. **Not optional**: the
   breakdown rows summing to `total` is an asserted invariant (#129).
6. **Tests**, then docs.

## Validation

**Correctness**

- Knight on a central square scores above the same knight in a corner (the canonical case).
- Rook on an open file scores above a rook boxed in behind its own pawns.
- Adding an enemy pawn covering a reachable square lowers the knight's score (proves D2).
- Blocking a bishop's diagonal with an own piece lowers its score; with an enemy piece it does not
  (proves D3's convention).
- Queen scores strictly above a rook on the same square with the same occupancy (proves #113).
- **#125's mirror-symmetry cases still pass** — a mirrored position must score exactly negated.
- The kingless-board regression still passes.
- **Run the Debug build**: Release silently tolerates the out-of-bounds reads this kind of table and
  bitboard indexing can introduce.

**Speed** — this is the first term that adds real per-node work, and the actual risk here is not
correctness.

- `Run-Bench.ps1` before and after, same compiler, repeat runs before quoting a delta.
- Per `CLAUDE.md`, a measured slowdown needs a stated benefit that outweighs it. A neutral Elo result
  with a throughput loss is a **net regression being masked**; a positive Elo result with one means
  the term is stronger than it looks.

**Strength**

- CI strength lab, **20,000 games**, reference left at its `merge-base` default. Per #230, epic #110
  terms **skip the screen** and go straight to a full batch — screening a small positive change costs
  55 minutes to be told "inconclusive" four times in five.
- ~3 h, 18 shards, two CI slots stay free. The result posts to the PR automatically.
- Record in `Docs/EloLog.md`'s **Linux CI — per-change measurements** table. That table's rows are
  not comparable with each other and must never be summed.

**Review** — `Eval.cpp` is in scope for `eval-reviewer`; dispatch it per the pre-PR checklist.

## Invariants that must hold afterwards

1. **`Breakdown()` rows sum to `total`.** Asserted; the honesty invariant behind #129.
2. **`Evaluate()` stays a pure function of the position** — no history, no move-order dependence, or
   two paths to one position disagree while sharing a TT entry.
3. **Colour symmetry**: mirroring a position negates the score exactly (#125).
4. **Thread safety**: nothing stored on `EvalManager`/`EvalComplex`; `EvalContext` remains a stack
   local built per call. Lazy SMP shares the evaluator across threads.
5. **No per-node allocation** — bitboards and `popcount` only.
6. `EvalContext::occupied[]` and `all_pieces` become genuinely read; their "do not remove as dead
   code" comments can go.
