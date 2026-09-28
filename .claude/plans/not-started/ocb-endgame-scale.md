# Opposite-coloured-bishop endgame scale — Design

**Issue:** #599 (evidence: #618, #660)

## Goal

`EndgameScale()` gives full value to K+B+P(s) vs K+B with the bishops on opposite colours. The oracle
scores these endings as level, but our engine scores them at a full pawn or two. So the engine will trade
a won position into one of them and believe it is still winning. #618 found this happening
in lab games: in 4 games across two 19,980-game runs, the stronger side entered pure OCB with two
pawns against none, which the oracle scores at ~0. The oracle preferred an alternative worth
≥150 cp, and our engine at depth 18 (vs ~12 in play) still scores the entered ending **+227 to +402**
and picks the same move. The code comment at `Eval.cpp:921-925` justifies leaving OCB unscaled with
"convert often enough"; that holds for pawnful OCB, not for this narrow class.

## Scope

**This change will:**

- scale K+B+1–2P vs K+B (bishops on opposite colours, no other pieces, defender pawnless) to a
  fraction of its score, in both colour orientations;
- update the comments that enumerate scaled classes (`Eval.cpp:920-925`, the note on the scale
  constants in `Eval.h`, the `MATERIAL_PRUNING_MIN_PIECES` note at `AIPerplex.cpp:53-58`);
- add eval tests and the matching `Docs/TestDesign.md` bullet.

**This change will not:**

- scale OCB with more pawns, with a defender pawn, or with extra rooks/knights (#660: none repeats as
  an avoidable wrong move; see Follow-up);
- touch two-minors-vs-one (#660: 17 games scattered over many signatures, none reaching 3);
- change `MATERIAL_PRUNING_MIN_PIECES` or any search code;
- tune the scale values — the lab run measures one configuration.

## Decisions

### D1: The defender must be pawnless

The class is exactly: no queens, rooks or knights; one bishop each, on opposite colours; one side has
1 or 2 pawns and the other has none.

This keeps the invariant that quiescence pruning relies on (`Eval.h` scale-constant note,
`AIPerplex.cpp:53-58`): every scaled class leaves its smaller side at most two men. K+B is two
men. Allowing a defender pawn (K+B+P) makes three, which silently defeats
`MATERIAL_PRUNING_MIN_PIECES = 4`.

Rejected: including 1-v-1 and 2-v-1 and raising the threshold to 5. That is a search change with its
own nps and strength cost, and it covers one of the 5 #618 games (`4:962`, 1 v 1).

### D2: Two constants by pawn count, set by judgement

- `OPPOSITE_BISHOPS_ONE_PAWN_SCALE = 4`. K+B+P vs K+B is a textbook draw unless the defending side
  is out of play. In the lab, 22 of 22 entries at 100–249 cp drew. The 7 entries at ≥250 cp were
  all won, but those are positions where search already sees the pawn through, and a promotion leaves
  the class.
- `OPPOSITE_BISHOPS_TWO_PAWNS_SCALE = 8`. Two pawns win more often: widely split passers can
  overload the bishop. Entries at ≥250 cp went 8 W / 11 D; at 100–249 cp they went 2 W / 5 D.

Neither value is measured. Like the rook-class constants, they are strength parameters, kept or
dropped on match evidence.

Rejected:
- A single constant. It would weigh the near-certain 1-pawn draw the same as the 2-pawn class that
  sometimes converts.
- A zero. `Eval.cpp:924-925` requires that no defence loses before a class is scaled to zero, and
  2 v 0 does lose sometimes.

### D3: A new helper, called on the bishops-only pawn branch

`OppositeBishopsScale(boards)` is called in `EndgameScale()`'s pawn branch after the N/R exit and
before `WrongBishopFortress()`. The two classes are disjoint: the fortress needs a bare defending king.
The helper states exact counts in the style of `PawnlessRookScale()`, and it is orientation-free: the
side holding the pawns is the attacker.

Rejected: folding the test into `WrongBishopFortress()`. It is a different rule (piece count, not king
placement), and that function's comment says so.

## Assumptions I cannot verify from the code

- **The lab can measure this.** The expected effect is a few half-points in ~20k games, well
  inside the lab's ±3.5 Elo. The lab will not show a gain. It bounds a regression, as it did
  for #128. The evidence that the change does what it claims is the decision replay (Validation 3).
  This is not verifiable in advance, and the owner has accepted it: no SPRT.
- **Draw adjudication does not hide the change.** `strength.yml` adjudicates a draw at |score| ≤ 10
  for 8 moves after move 40. A scaled +300 becomes +75 to +150, so this does not trigger
  adjudication early. Verified by reading `strength.yml:465`.
- **The 5 FENs carry the #618 oracle's verdict** (Stockfish d20, no tablebases). They were not
  tablebase-checked. The class is ≤6 men, so a Syzygy probe would settle them, but none is installed
  here, and adding one is the owner's call. The verdicts are taken as given.

## Invariants

- Every scaled class leaves its smaller side at most two men (D1).
- Scale 16/16 on everything outside the class: same-coloured bishops, a defender pawn, 3+ pawns, any
  extra piece.
- Colour symmetry: identical results for the mirrored position and for either side to move.
- The #129 honesty invariant: `Breakdown()` rows plus `endgame_adjustment` reproduce `total`.

## Validation

1. **Tests** (`StratChessTests/EvalEndgameTests.cpp`, `[eval]`):
   - Each class is checked four ways (FEN × `MirrorFen`, both sides to move). It must equal the
     unscaled score × scale / 16, and the unscaled score must be nonzero.
   - Complement cases stay unscaled: same-coloured bishops, one defender pawn, three pawns, an added
     knight.
   - The existing Breakdown test is extended to cover the new class.
   - Each new test is falsified by reverting the classifier.
2. **nps:** `Run-Bench.ps1`, candidate vs `origin/main`. The class is reached only after the queen,
   pawn and N/R tests, so the expected result is no measurable difference. If there is one, it needs
   a stated benefit.
3. **Decision replay** (the real evidence): at d18, with the candidate build, on the 5 #618 decision
   FENs. At least 3 of the 4 in-scope games must now pick a move other than the entering move. In
   addition, every won 1–2-pawn pure OCB entry at ≥250 cp in `entries*.jsonl` whose move changes is
   checked with the oracle, and its new move must still score ≥150. Scripts:
   `StratChessSupport/EvalDatasets/2026-09-28_drawish-move-choice-618/` (`depth618.py`, retargeted).
4. **CI strength lab:** candidate vs `merge-base`, default game count. The gate is no measurable
   regression, i.e. the interval lower bound is not below −3.5 Elo. Starting it is the owner's call.

## Cost

- **Size:** under 50 lines of code plus ~60 lines of tests; 4 files (`Eval.cpp`, `Eval.h`, the
  `AIPerplex.cpp` comment, `EvalEndgameTests.cpp`) plus `Docs/TestDesign.md`.
- **Blast radius:** the engine tier, with an eval-reviewer dispatch. There is no search logic change.
- **Review:** one code review, plus the cross-agent round.
- **Lab run:** ~3 h of CI; one run, repository-wide.

## Follow-up

If #599's lab run shows a gain, extending the scale to OCB with more pawns would be the next thing to
measure.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the defender must be pawnless (D1) | helper comment in `Eval.cpp`; the `Eval.h` constant note |
| Why OCB is scaled here but not with more pawns | `Eval.cpp:920-925` comment, qualitative (no figures) |
| Scale values are unmeasured strength parameters | `Eval.h` constant note |
| Replay result and lab Elo | `Docs/Changelog.md`, PR body, `Measurements/` |
| The declined OCB entry in Outcomes | `Docs/MoveQuality.md` → Outcomes: rewrite it as narrowed and taken |
