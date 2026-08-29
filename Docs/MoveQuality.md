# Move-Quality Scan — Method and Baseline

Every strength-lab run writes a fully annotated PGN of every game it plays. The run summary consumes
one number from it — the pooled Elo — which decides *whether* a change helped and is silent on
*where*. `Scripts/analyze_move_quality.py` reads the same PGNs and answers the second question.

This file is the method and the ledger; the tool is the instrument. Like
[`EloLog.md`](EloLog.md) it is a measurement record and is appended to, not rewritten — and like it,
it is in scope of #227 if the ledgers move out of `Docs/`.

| Need | Section |
|---|---|
| run it against a strength run | [Regenerating](#regenerating) |
| know what the numbers mean and what they cannot see | [Method](#method) · [Limits](#limits) |
| the current numbers | [Baseline: run 33215162562](#baseline-run-33215162562) |
| what the baseline established | [Findings](#findings) |

---

## Regenerating

Artifacts are retained 90 days, ~1 MB per shard. A production run is 18 shards.

```sh
gh run download <run_id> --repo theEscape2207/StratChess -p 'strength-<run_id>-shard-*' -D pgn
python StratChessEvolved/Scripts/analyze_move_quality.py pgn --self-check
python StratChessEvolved/Scripts/analyze_move_quality.py pgn --json stats.json
```

`--self-check` is the gate: it asserts the parse covered every game and every annotation, and that
the score perspective is the one every formula assumes. Run it before reading any number. The full
scan is about 6 seconds over 18 shards on 18 workers.

## Method

**Scores are mover-relative.** Each `{+1.22/11 0.415s}` is from the perspective of the side that just
moved, the standard UCI convention. Under a White-relative reading every self-swing would be inverted
for one side; `--self-check` fails loudly if a corpus ever violates it, and on the baseline it reports
the losing side at **+22.26 cp** mean signed self-swing against the winning side's **−21.86 cp**.

**Two measurements, from one walk.** In a candidate-vs-reference match the two builds never score the
same position, so both are available:

- **Self-swing** for build X at ply *t*: `s_X(t) − s_X(t+2)` — the engine's own admission that its
  position got worse.
- **Cross-build gap** at ply *t*: `s_X(t) + s_Y(t+1)`; the addition is the perspective flip. Zero
  means the two builds agree about the position X just handed over.

**Contested positions are the ones that matter.** A build that is already lost reports large swings
while flailing, and those cost nothing. Every blunder statistic is therefore reported twice: over all
positions, and restricted to positions where the mover reported no more than ±150 cp before moving.
The unrestricted numbers are dominated by lost positions and should not be read as a defect profile —
see [Findings](#findings), where that distinction changed the conclusion.

**Phase mirrors `Eval.h`**: `1·N + 1·B + 2·R + 4·Q` over both sides, capped at 24. Buckets are
opening ≥ 20, middlegame 7–19, endgame ≤ 6, where 6 is `MOPUP_MAX_LOSER_PHASE` — approximately where
`eval_mopup` starts to apply. `Eval.h` is the source of truth; a drifted copy shifts bucket
boundaries, it does not invalidate the numbers inside a bucket.

**Mate scores are counted, never averaged.** A mate score is not on the centipawn scale and saturates
any mean it enters. An unrecognised comment aborts the run rather than being skipped: a format the
parser does not understand means a changed fastchess version, and quietly dropping those moves would
bias every statistic toward whatever fastchess still spells the old way.

**The remaining clock is derived, not read.** fastchess writes the time *spent*. Remaining clock is
reconstructed per side from the `TimeControl` tag minus that side's running total, plus one increment
per move.

**CI games start from an EPD book position** at fullmove 9, with `[SetUp "1"]` + `[FEN]`, so ply
counts are counted from the setup position, not the true game start. There are zero `{book}` comments
in a CI PGN — every move present is an engine move. A local `Run-EloMatch.ps1` run against a `.pgn`
book does emit them, and they are excluded.

## Limits

- **It grades its own homework.** A position both builds misjudge the same way produces no swing at
  all. That blind spot is the entire reason #77 also proposes external adjudication.
- **The endgame is censored by adjudication.** 68% of the baseline's games ended by adjudication under
  `-draw movenumber=40 movecount=8 score=10` / `-resign movecount=4 score=800`. "Endgame" numbers mean
  *the position at the moment of adjudication*, not played-out technique, and the calibration table is
  only meaningful below ±800 — above that, a win is very nearly definitional.
- **The reported score comes from the search that chose the move** — TT hits, aspiration windows, LMR
  re-searches — so it is a self-consistent series, not an absolute yardstick.
- **Both builds in a merge-base run are nearly identical**, so the cross-build gap measures the noise
  floor rather than a real disagreement. It becomes informative when the two sides differ in eval.

---

## Baseline: run 33215162562

| | |
|---|---|
| Run | [33215162562](https://github.com/theEscape2207/StratChess/actions/runs/33215162562), 2026-08-28 |
| Builds | `candidate-ff5d2f5` (`worktree-lmr-depth-clamp`) vs `reference-be5ca11` (merge-base) |
| Result | pooled **+9.64 ± 3.63 Elo**, 9990 pairs, score 51.39% |
| Corpus | 19,980 games · 2,230,649 annotated moves · 15,528 mate scores · 0 games skipped |
| Toolchain / TC | GCC on ubuntu-24.04, Release, Threads=1, 10+0.1, `UHO_4060_v3.epd` |

### Game outcomes

| | Games | Share |
|---|---|---|
| Decisive | 12,550 | 62.8% (White 41.1%, Black 21.7%) |
| Drawn | 7,430 | 37.2% |
| Ended by adjudication | 13,589 | 68.0% |
| Over 200 plies | 1,204 | 6.0% |

Draws, by the reason fastchess recorded, and what the better-placed side still reported on its **last**
move of the game:

| Draw reason | Games | Of all games | Last score ≥ 150 cp |
|---|---|---|---|
| 3-fold repetition | 4,393 | 22.0% | 0.1% |
| Insufficient mating material | 1,404 | 7.0% | **47.4%** |
| Adjudication (both sides ≈ 0.00) | 1,094 | 5.5% | 0% |
| Fifty-move rule | 507 | 2.5% | 0% |
| Stalemate | 32 | 0.2% | 0% |

### Blunder profile, contested positions only

Self-swing ≥ 150 cp, restricted to positions the mover scored within ±150 cp. Pooled over both builds;
the two builds differ by under 0.05 percentage points in every cell.

| Phase | Moves | mean \|swing\| | ≥150 cp | ≥300 cp |
|---|---|---|---|---|
| opening | 522,989 | 11.1 | 0.15% | 0.03% |
| middlegame | 522,161 | 12.8 | 0.29% | 0.09% |
| endgame | 393,243 | 9.9 | 0.22% | 0.07% |

By moved piece the rate is flat at 0.2–0.3% for every piece type. By remaining clock, within a phase:

| Phase | > 8 s | 5–8 s | 2–5 s | < 2 s |
|---|---|---|---|---|
| opening | 0.03% | 0.14% | 0.40% | 0.30% |
| middlegame | 0.03% | 0.10% | 0.29% | **0.46%** |
| endgame | — | 0.13% † | 0.11% | 0.25% |

† one blunder in 765 moves — the engine is almost never in an endgame with that much clock left.

### Eval calibration

Observed score for the side to move, by the score it reported, split on whether both sides still hold
at least three pawns. This split is the control that separates "the endgame eval is optimistic" from
"the eval is blind to drawish material".

**Pawn-rich** (both sides ≥ 3 pawns) — the three phases agree within noise:

| Reported | opening | middlegame | endgame |
|---|---|---|---|
| +100–150 | 0.735 | 0.750 | 0.744 |
| +200–250 | 0.889 | 0.895 | 0.908 |
| +300–350 | 0.959 | 0.965 | 0.983 |
| +500–550 | 1.000 | 0.991 | 0.997 |

**Pawn-poor** (either side ≤ 2 pawns) — the endgame column falls away, and is not even monotonic:

| Reported | middlegame | endgame |
|---|---|---|
| +100–150 | 0.692 | 0.631 |
| +200–250 | 0.831 | 0.749 |
| +250–300 | 0.888 | 0.789 |
| +300–350 | 0.882 | **0.764** |
| +500–550 | 0.997 | **0.897** |

### Drawish material classes

Counted once per game, at the first position that reaches the class, against what the stronger side
reported there. Games, not plies — plies over-weight exactly the long drawn games.

| Class | Reported | Games | Of all | Observed | W/D/L |
|---|---|---|---|---|---|
| K + minor vs K | ≥ +250 | 656 | 3.3% | **0.500** | 0/656/0 |
| KR+minor vs KR (pawnless) | ≥ +250 | 262 | 1.3% | **0.645** | 76/186/0 |
| KR vs K+minor (pawnless) | ≥ +100 | 58 | 0.3% | **0.509** | 1/57/0 |
| KR vs K+minor (pawnless) | ≥ +250 | 151 | 0.8% | 0.801 | 91/60/0 |
| Opposite-coloured bishops | ≥ +250 | 143 | 0.7% | 0.881 | 109/34/0 |
| Opposite-coloured bishops | ≥ +100 | 279 | 1.4% | 0.642 | 82/194/3 |

### Cross-build noise floor

Mean `|s_X(t) + s_Y(t+1)|` between two builds differing only by the LMR depth clamp: **9.1 cp**
opening, **14.8 cp** middlegame, **17.8 cp** endgame. A future run whose builds differ in evaluation
should exceed this; a run that does not has not changed what the engine believes.

---

## Findings

**1. There is no general blunder weakness to find.** In contested positions the rate is 0.15–0.29%
and is flat across phase and across moved piece. The unrestricted numbers say the opposite —
endgame 1.7%, king moves 34% of all blunders — and both of those are artifacts of already-lost
positions, where the losing side flails and the swings cost nothing. Any future reading of this
report should use the contested rows.

**2. The evaluation is well calibrated except when the pawns are gone.** With three pawns a side the
three phases agree to within noise, and the endgame is marginally *better* calibrated than the
middlegame at large scores. With two pawns or fewer the endgame curve collapses and stops being
monotonic — +300 converts at 0.764 while +250 converts at 0.789 and +575 at 0.998. So this is not a
tapering or phase-calibration defect; it is the absence of drawish-material knowledge (#128).

**3. The dips have names, and they are not the ones #128 predicted.** The +300–350 dip is dominated by
**rook + minor vs rook**, a fortress the evaluator scores a full piece up: 262 games reach it with the
stronger side reporting ≥ +250, and it scores 0.645. Rook vs minor at ≥ +100 is a dead draw in
practice (0.509 over 58 games). And 656 games — **3.3% of the entire run** — end in K + minor vs K
with the stronger side still reporting ≥ +250; every single one is a draw. In 26.8% of the
insufficient-material draws the claiming side still had a pawn two plies earlier and 44.1% six plies
earlier, so it is trading its last pawn into a dead draw while reporting a piece up.

**4. Opposite-coloured bishops are not worth a scale factor.** The one measured negative result: pure
OCB endings at ≥ +250 convert at 0.881, close enough to the pawn-rich curve that scaling them toward
zero would more likely cost Elo than gain it.

**5. Repetition draws are not squandered wins.** 22.0% of games end in threefold repetition, but in
99.9% of them both sides' last reported score was under 50 cp. The engine repeats from positions it
believes are equal, so this is a contempt/playing-on decision (#76 direction 3), not a defect.

**6. Time pressure is real but second-order.** 23% of contested middlegame decisions are taken
with under 2 s remaining, and the blunder rate there is 0.46% against 0.10% at 5–8 s. Causality runs
both ways — long complex games both consume clock and contain more mistakes — so this bounds the
prize rather than measuring it (#103).

**7. Two invariants confirmed in production data.** All 507 fifty-move draws fire at a halfmove clock
of exactly 100, so #345's fix holds at scale. And no build ever announced a forced mate and failed to
win the game: 1,462 and 1,344 such games, all won.
