# Bishop pair, connected rooks, castling-done (#111, #114, #115)

## Goal

Add the three low-hanging evaluation terms from epic #110, measured together as one batch under a
single SPRT (`Gain [0, 10]`), per the agreed measurement budget. Attribution to the individual terms
is deliberately given up in exchange for one measurement instead of three; if the batch comes back
negative, bisect then.

Scope limit: no tuning. Every constant below is a defensible standard value, not a fitted one —
#117 owns fitting. No king-safety work (#97) and no mobility work (#98) creeps in here.

## Design decisions

### D1 — Bishop pair requires opposite-coloured bishops, not just a count of two

`popcount(bishops) >= 2` is the common shortcut, but two same-coloured bishops are not a bishop
pair — the term exists because the pair covers both square colours. Same-coloured pairs are
reachable by underpromotion, and are rare but real.

Testing square colour is one mask and one comparison, so correctness costs nothing here. Defining
`LIGHT_SQUARES` locally in `Eval.cpp` rather than in `defines.h`: nothing else needs it yet, and
`defines.h` is already crowded.

### D2 — Castling-done is derived from the position, never from history

**The issue's framing — "track whether each side has completed castling" — cannot be implemented
without breaking evaluation purity, so it is not implemented that way.**

`Board` records castling *rights*, not whether castling *happened*. A `hasCastled[2]` flag would
have to be set in `UpdateCastlingState` and would then be state the evaluator reads that is **not
recoverable from a FEN**: `SetupFromFEN` could not restore it. Consequences:

- The same position would evaluate differently depending on how it was reached.
- The transposition table keys on the Zobrist position hash, so two paths to one position would
  disagree about its score while sharing a TT entry.
- #117's tuner scores a corpus of FEN strings, so every corpus position would take the default.

Instead the term reads `gameInfo_.castlingRights`, which **is** a FEN field, and combines it with
the king's square:

| Rights for this side | King on a castled square | Score |
|---|---|---|
| any remaining | — | `0` — still flexible, nothing decided |
| none | yes | `+CASTLING_DONE_BONUS` |
| none | no | `-CASTLING_LOST_PENALTY` |

"Castled square" = the home rank, files b/c (queenside) or g/h (kingside). Files rather than exact
squares because a castled king routinely steps to h1/b1 afterwards, and the term should not evaporate
when it does.

This is exactly the issue's own parenthetical — *"or has irrevocably lost the right without
castling"* — which is position-derivable. Only the first half of its phrasing was not.

**Middlegame-only**: `ScorePair{bonus, 0}`. King shelter is a middlegame concern; in an endgame the
king wants the centre, and `eval_pst`'s endgame king table already says so. A flat bonus would fight
it.

### D3 — Connected rooks via magic attacks, not a between-mask

`RookAttacks(sq, ctx.all_pieces) & otherRook` answers "same rank or file with nothing between" in
one lookup, reusing the PEXT tables from #108. No new between-mask table, and it is automatically
correct about blockers.

Scored per connected **pair**, so three rooks pairwise connected score three times — rare enough not
to warrant special-casing, and arguably correct.

Middlegame-weighted (`ScorePair{full, half}`): connected rooks matter most with a full board; the
existing 7th-rank bonus already covers the endgame case where rooks dominate.

### D4 — New terms are separate functions with their own Breakdown rows

`EvalBreakdown` carries a per-term row and `StratChessTests` asserts
`material + Σ terms == total`. Adding terms without adding rows would break that identity, so
`bishops` and `castling` become first-class rows, visible in the UCI `eval` output.

Connected rooks folds into the existing `eval_rooks` rather than becoming a fourth row — it is a
rook term, and the `rooks` row already exists.

### D5 — Constant values

| Constant | mg | eg | Rationale |
|---|---|---|---|
| `BISHOP_PAIR_BONUS` | 30 | 45 | Issue suggests 30-50. Worth more in the endgame as the board opens — standard. |
| `CONNECTED_ROOKS_BONUS` | 15 | 8 | Standard range 10-20; halved in the endgame where the 7th-rank term already pays. |
| `CASTLING_DONE_BONUS` | 25 | 0 | Middlegame-only per D2. |
| `CASTLING_LOST_PENALTY` | 20 | 0 | Slightly under the bonus: losing rights is bad, but less decisive than being safely castled. |

All four are untuned starting points. #117 owns fitting them.

## Files changed

- `StratEngine/Eval.h` — four constants; `EvalContext.castling_rights`; `EvalBreakdown.bishops` and
  `.castling`; declarations for `eval_bishops` and `eval_castling`.
- `StratEngine/Eval.cpp` — `eval_bishops` (new), `eval_castling` (new), connected-rooks block in
  `eval_rooks`, `BuildContext` populates `castling_rights`, `Evaluate` and `Breakdown` sum the two
  new terms.
- `StratChessTests/EvalTests.cpp` — `[eval]` coverage for all three terms plus the sum identity.

## Validation plan

1. `build.ps1 all` — `/WX`, zero warnings.
2. `StratChessTests.exe "[eval]"` then the full fast tier.
3. `Validate-PrePR.ps1` — Engine tier (build + extended + tactical + self-play).
4. Dispatch `eval-reviewer` (CLAUDE.md step 3 — `Eval.cpp` changed).
5. `Run-EloMatch.ps1 -Sprt Gain` vs `elo-reference-v1`, ~3 h budget, chunked to survive the
   ~750-game background cap. Report inconclusive as inconclusive.

## Key correctness properties

- **Colour symmetry.** A position and its colour mirror must evaluate to the same magnitude with
  opposite sign. #125 fixed the PST mirroring precisely so this holds; all three new terms are
  written per-colour and must not break it. Test with the existing `MirrorFen` helper.
- **Purity.** `Evaluate` remains a function of the position alone (D2). No term may read anything
  absent from a FEN.
- **Kingless-board safety.** `eval_castling` must check `king_sq != NO_SQUARE`, matching `eval_pst`
  and `eval_mopup`. A default-constructed `Board` has no king and `GetFirstPiece` on an empty
  bitboard reads out of bounds in Release.
- **Breakdown identity.** `material + pawns + rooks + pst + mopup + bishops + castling == total`,
  up to the side-to-move sign.
