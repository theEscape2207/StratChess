# Move-Quality Scan — Method and Baseline

Every strength-lab run writes a fully annotated PGN of every game it plays. The run summary consumes
one number from it — the pooled Elo — which decides *whether* a change helped and is silent on
*where*. `Scripts/analyze_move_quality.py` reads the same PGNs and answers the second question.

This file is the method and the ledger; the tool is the instrument. Like
[`../Measurements/`](../Measurements/) it is a measurement record and is appended to, not rewritten — and like it,
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
python Scripts/analyze_move_quality.py pgn --self-check
python Scripts/analyze_move_quality.py pgn --json stats.json
python Scripts/analyze_move_quality.py --self-test   # no corpus needed
```

`--self-check` is the gate: it asserts the parse covered every game and every annotation, and that
the score perspective is the one every formula assumes. Run it before reading any number.
`--self-test` is the other half — fixture games covering every annotation shape, and a deliberately
corrupt game that must be counted once, not as both parsed and skipped. The full scan is about 6
seconds over 18 shards on 18 workers.

**Every rate carries a game-clustered interval.** Plies inside one game share its opening, its two
builds and its result, so a per-ply confidence interval is several times too tight to believe. The
report bootstraps by resampling whole games; where it prints `[lo, hi]`, that is the 95% interval.

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

| Phase | Moves | mean \|swing\| | ≥150 cp (95% clustered) | ≥300 cp |
|---|---|---|---|---|
| opening | 522,989 | 11.1 | 0.154% [0.144, 0.165] | 0.03% |
| middlegame | 522,161 | 12.8 | 0.293% [0.277, 0.308] | 0.09% |
| endgame | 393,243 | 9.9 | 0.215% [0.200, 0.231] | 0.07% |

The three intervals are disjoint: the middlegame rate is genuinely 1.9× the opening rate. All three
sit inside the same 0.15–0.30% band, which is the point — but the band is not one flat rate.

By moved piece, same treatment:

| Piece | Moves | ≥150 cp (95% clustered) |
|---|---|---|
| bishop | 227,637 | 0.160% [0.144, 0.178] |
| knight | 195,017 | 0.200% [0.180, 0.220] |
| pawn | 289,165 | 0.204% [0.188, 0.220] |
| king | 241,601 | 0.216% [0.198, 0.236] |
| queen | 175,410 | 0.262% [0.238, 0.287] |
| rook | 309,563 | 0.277% [0.258, 0.295] |

Bishop and rook do not overlap either, so this is not flat; it is a 1.7× spread inside a narrow band,
with the heavy pieces at the top. Nothing here resembles the 34%-of-blunders king-move story the
unrestricted scan tells.

By the clock the mover **had when it started thinking** — not what was left afterwards; the two
differ for 5.0% of moves, and the band is a property of the decision, not of its aftermath:

| Phase | > 8 s | 5–8 s | 2–5 s | < 2 s |
|---|---|---|---|---|
| opening | 0.03% | 0.17% | 0.41% | 0.30% |
| middlegame | 0.04% | 0.11% | 0.30% | **0.47%** |
| endgame | — | 0.00% † | 0.12% | 0.25% |

† zero blunders in 1,293 moves — the engine is almost never in an endgame with that much clock left.

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

**The score bands are exclusive.** `+100–249` does not include `≥ +250`; the cumulative row is listed
separately, and it is the one to quote for "the engine thought it was winning".

| Class | Reported | Games | Of all | Observed (95% clustered) | W/D/L |
|---|---|---|---|---|---|
| K + minor vs K | ≥ +250 | 656 | 3.3% | **0.500** [0.500, 0.500] | 0/656/0 |
| KR+minor vs KR (pawnless) | ≥ +250 | 262 | 1.3% | **0.645** [0.618, 0.672] | 76/186/0 |
| KR vs K+minor (pawnless) | +100–249 | 58 | 0.3% | **0.509** [0.500, 0.526] | 1/57/0 |
| KR vs K+minor (pawnless) | ≥ +250 | 151 | 0.8% | 0.801 [0.762, 0.838] | 91/60/0 |
| KR vs K+minor (pawnless) | ≥ +100 cumulative | 209 | 1.0% | 0.720 [0.684, 0.751] | 92/117/0 |
| Opposite-coloured bishops | ≥ +250 | 143 | 0.7% | 0.881 [0.846, 0.916] | 109/34/0 |
| Opposite-coloured bishops | +100–249 | 279 | 1.4% | 0.642 [0.615, 0.668] | 82/194/3 |
| Opposite-coloured bishops | ≥ +100 cumulative | 422 | 2.1% | 0.723 [0.697, 0.748] | 191/228/3 |

### Cross-build noise floor

Mean `|s_X(t) + s_Y(t+1)|` between two builds differing only by the LMR depth clamp: **9.1 cp**
opening, **14.8 cp** middlegame, **17.8 cp** endgame. A future run whose builds differ in evaluation
should exceed this; a run that does not has not changed what the engine believes.

**This is an upper bound, not an estimate of disagreement.** The two scores are one ply apart: X
reports the value of the position it is handing over, Y reports its own value for that position with
its own search and its own clock. Under the negamax identity the sum is zero when the builds agree —
X plays its own PV move, so the *move* contributes nothing — but the two searches are not the same
search, and that residual is inside the number. Read it as a noise floor to exceed.

The clean version is available for a subset: positions with an identical FEN that **both** builds
actually scored, no ply offset. Within a shard there are 46,389 of them and the gap is **5.4 cp** —
but 93% are opening positions and only 469 are endgames, because the two builds rarely reach the same
middlegame. It cross-checks the opening row of the table above (9.1 cp against 5.4 cp, the difference
being the one-ply offset); it cannot replace it.

---

## Findings

**1. There is no general blunder weakness to find.** In contested positions every rate sits between
0.15% and 0.30%, by phase and by moved piece alike. The unrestricted numbers say something else
entirely — endgame 1.7%, king moves 34% of all blunders — and both are artifacts of already-lost
positions, where the losing side flails and the swings cost nothing. Any future reading of this
report should use the contested rows.

Inside that band the rates are *not* equal, and the game-clustered intervals are tight enough to say
so: middlegame 0.293% against opening 0.154%, rook 0.277% against bishop 0.160%. That 1.9× ratio is
worth knowing, but it is not the shape of a defect either — middlegame worst and heavy pieces worst
is what a search that is depth-limited in the most complex positions should look like.

**2. The evaluation is well calibrated except when the pawns are gone.** With three pawns a side the
three phases agree to within noise, and the endgame is marginally *better* calibrated than the
middlegame at large scores. With two pawns or fewer the endgame curve collapses and stops being
monotonic — +300 converts at 0.764 while +250 converts at 0.789 and +575 at 0.998. So this is not a
tapering or phase-calibration defect; it is the absence of drawish-material knowledge (#128).

**3. The dips have names, and they are not the ones #128 predicted.** The +300–350 dip is dominated by
**rook + minor vs rook**, a fortress the evaluator scores a full piece up: 262 games reach it with the
stronger side reporting ≥ +250, and it scores 0.645. Rook vs minor at +100–249 is a dead draw in
practice (0.509 over 58 games); across the whole ≥ +100 range it is 0.720 over 209 games, because the
≥ +250 half is often a real win. And 656 games — **3.3% of the entire run** — end in K + minor vs K
with the stronger side still reporting ≥ +250; every single one is a draw. In 26.8% of the
insufficient-material draws the claiming side still had a pawn two plies earlier and 44.1% six plies
earlier, so it is trading its last pawn into a dead draw while reporting a piece up.

**4. Opposite-coloured bishops are not worth a scale factor.** The one measured negative result: pure
OCB endings at ≥ +250 convert at 0.881, close enough to the pawn-rich curve that scaling them toward
zero would more likely cost Elo than gain it.

**5. Repetition draws are not squandered wins.** 22.0% of games end in threefold repetition, but in
99.9% of them both sides' last reported score was under 50 cp. The engine repeats from positions it
believes are equal, so this is a contempt/playing-on decision (#76 direction 3), not a defect.

**6. Time pressure is real but second-order.** 21% of contested middlegame decisions are *started*
with under 2 s on the clock, and the blunder rate there is 0.47% against 0.11% at 5–8 s. Causality runs
both ways — long complex games both consume clock and contain more mistakes — so this bounds the
prize rather than measuring it (#103).

**7. Two invariants confirmed in production data.** All 507 fifty-move draws fire at a halfmove clock
of exactly 100, so #345's fix holds at scale. And no build ever announced a forced mate and failed to
win the game: 1,462 and 1,344 such games, all won.

---

## Addendum: run 33429454765 — level-material rook endings

The `RvsR` class (pawnless K+R vs K+R) was added to `material_classes()` after the baseline above, so
these rows come from a later run, post-#128, and are not comparable ply-for-ply with the table in
[Baseline](#baseline-run-33215162562). 19,980 games.

| Class | Reported | Games | Of all | Observed (95% clustered) | W/D/L |
|---|---|---|---|---|---|
| KR vs KR (pawnless) | < +100 | 592 | 3.0% | **0.500** [0.500, 0.500] | 0/592/0 |
| KR vs KR (pawnless) | ≥ +250 | 309 | 1.5% | **1.000** [1.000, 1.000] | 309/0/0 |

The class separates completely, and the band that matters is the low one — the reverse of every other
class here. Below +100 the reported edge is phantom: 592 games, not one of them decisive. At ≥ +250 the
score is real, because it comes from a leaf where the rook has already fallen and the material is no
longer this class. Nothing lands in between.

This is the evidence behind #436: the phantom is the PST, rook-file and mobility terms surviving on a
level-material board, a median of 22 cp and a maximum of 64 over 400 positions sampled from this run.

It **replicates** on run `33568346899`, a fresh 19,980 games in which one side carries the resulting
scale and the other does not:

| Class | Reported | Games | Of all | Observed (95% clustered) | W/D/L |
|---|---|---|---|---|---|
| KR vs KR (pawnless) | < +100 | 650 | 3.3% | **0.500** [0.500, 0.500] | 0/650/0 |
| KR vs KR (pawnless) | ≥ +250 | 307 | 1.5% | **1.000** [1.000, 1.000] | 307/0/0 |

Same total separation on a corpus that shares no games with the one above. That run also shows the
scale itself in the annotations: |score| reported from inside the class has p90 **13** for the
unscaled build and **3** for the scaled one, which is 4/16 to the digit. **No steering effect is
visible** — the two builds enter the class 777 and 784 times and spend 6.6 and 6.8 plies in it — and
the low band offers none to find, since it was already saturated at 0.500.

### Symmetric classes cannot be attributed to a build

`RvsR` and `OCB` hold level material, so `material_classes()` returns a placeholder colour and the
**sign of the entry score** names the stronger side. Two consequences, both of which have produced a
false signal:

- **It is White-biased.** An entry scored exactly 0 falls to White under the `>= 0` rule, and roughly
  a quarter to a third of `RvsR` entries are exactly 0 — White is named the stronger side about 2.5
  times as often as Black in both runs above.
- **It is invalid across a build that scales the class.** Scaling truncates small scores toward zero,
  so entries move out of the low magnitude bands into the exact zero the rule hands to White. Split
  by build, run `33568346899`'s `< +100` band reads 295/355 — 2.3 standard errors, and it looks
  exactly like the intended effect. It is not: the whole skew sits in the `|cp| 1–24` bucket the
  scale compresses, above `|cp| >= 25` it is 150/143, and the control run, where neither build scaled
  the class, carries a skew of the same size pointing the other way.

Use the pooled rows for a symmetric class. A per-build split of one is only readable when both builds
value the class identically, which is the case the split is least often wanted for.
