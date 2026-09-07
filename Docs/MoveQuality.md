# Move-Quality Scan — Method and Baseline

A strength run's pooled Elo says *whether* a change helped and nothing about *where*. The lab writes
a fully annotated PGN of every game; two tools read it. `Scripts/analyze_move_quality.py` answers
"where" from the engine's own annotations. `Scripts/analyze_external_quality.py` answers it again
with an outside engine as judge — the only way to see mistakes the engine does not know it made.

This file is the method and the ledger. It is appended to, not rewritten, and is in scope of #227 if
the ledgers move out of `Docs/`.

| Need | Section |
|---|---|
| run it | [Regenerating](#regenerating) |
| what the numbers mean, and cannot see | [Method](#method) · [Limits](#limits) |
| the numbers | [Baseline](#baseline-run-33215162562) · [Tier 2](#tier-2-external-adjudication) |
| what they established | [Findings](#findings) · [Tier 2 findings](#findings-1) |

---

## Regenerating

Artifacts are retained 90 days, ~1 MB per shard; a production run is 18 shards.

```sh
gh run download <run_id> --repo theEscape2207/StratChess -p 'strength-<run_id>-shard-*' -D pgn
python Scripts/analyze_move_quality.py pgn --self-check      # the gate; run it first
python Scripts/analyze_move_quality.py pgn --json stats.json
python Scripts/analyze_external_quality.py pgn --depth 12 --json external.json
```

**`--self-check` before reading any number.** It asserts the parse covered every game and every
annotation, and that the score perspective is the one every formula assumes. `--self-test` is the
other half — fixture games covering every annotation shape, plus a corrupt game that must be counted
once, not as both parsed and skipped. Tier 1's full scan is about 6 seconds over 18 shards.

Tier 2's oracle is a Stockfish binary in `EngineTesting/` beside `fastchess.exe`, outside the
checkout: it is GPL-3, and keeping it out keeps the repo free of that obligation. `--engine` or
`STOCKFISH_PATH` override the search. **Its `--self-test` is not optional** — a point-of-view slip
inverts every loss it reports without failing anything else, so four of its checks exist only to
catch that, and they need the binary to run.

### Cost

The scan reports its own: every progress line is stamped with elapsed time, and each shard prints
how long it took to score. On a 24-core box at `--jobs 12` a shard of ~83,000 contested rows scores
in ~280 s; the full 18-shard corpus took 5,125 s to score plus 584 s to bootstrap, **~95 minutes end
to end**. Depth and hardware move all of that. `--shards N` takes the first N — shards are
independent samples of one match, so a prefix is a smaller run of the same experiment, not a biased
one. Tier 1's six seconds buys a different question, not a worse one; run Tier 1 first.

**It does not scale with `--jobs`.** 6 workers to 12 cut per-shard time from 422 s to 278 s — 1.43×,
not 2×. Cause unmeasured; the obvious suspect is ruled out below. Raise `--jobs` only on a box nobody
is using, and expect less than you paid for.

### Oracle process lifetime

**One process per game, closed inside the task that opened it**, never left to interpreter exit. A
worker still holding a live engine never exits and the parent waits for it forever — which looks
exactly like a finished run whose report never prints.

That restart is also the fastest of three architectures measured, which was not the expected answer.
One shard, `--jobs 12`, settings alternated so run order could not carry the result:

| architecture | oracle starts / shard | per-shard time |
|---|---|---|
| one process per game (default) | 1,109 | 290, 288, 287 s |
| `--batch 24` | 47 | 333, 365, 322 s |
| 12 persistent workers, `ucinewgame` per game | 12 | 310, 330 s |

Every run produced byte-identical reports, so `ucinewgame` reproduces a fresh process exactly and the
choice is purely about speed. Batching loses ~18% and is fourteen times more variable — tail
imbalance across chunky work units. Persistent workers keep game-level scheduling and reach 96%
worker occupancy (~10 s idle per worker), and still lose ~13%, which **rules scheduling out and
leaves engine reuse itself as the cost**. Removing 99% of the NNUE loads made the scan slower, so
process startup is not why it scales sublinearly with `--jobs`. The unmeasured candidate is
`ucinewgame` clearing a 64 MB hash once per game against the memory bandwidth twelve concurrent NNUE
searches already saturate. `--batch` stays as a knob for measuring a different box; the default does
not use it.

**Every rate carries a game-clustered interval.** Plies inside one game share its opening, its two
builds and its result, so a per-ply interval is several times too tight to believe. The report
bootstraps by resampling whole games; where it prints `[lo, hi]`, that is the 95% interval.

## Method

**Scores are mover-relative.** Each `{+1.22/11 0.415s}` is from the perspective of the side that just
moved, the standard UCI convention. Under a White-relative reading every self-swing would be inverted
for one side; `--self-check` fails loudly on a corpus that violates it, and on the baseline reports
the losing side at **+22.26 cp** mean signed self-swing against the winning side's **−21.86 cp**.

**Two measurements from one walk.** In a candidate-vs-reference match the two builds never score the
same position, so both are available:

- **Self-swing** for build X at ply *t*: `s_X(t) − s_X(t+2)` — the engine's own admission that its
  position got worse.
- **Cross-build gap** at ply *t*: `s_X(t) + s_Y(t+1)`; the addition is the perspective flip. Zero
  means the builds agree about the position X just handed over.

**Contested positions are the ones that matter.** A build that is already lost reports large swings
while flailing, and those cost nothing. Every blunder statistic is reported twice: over all
positions, and restricted to positions where the mover reported within ±150 cp before moving. The
unrestricted numbers are dominated by lost positions and are not a defect profile.

**Phase mirrors `Eval.h`**: `1·N + 1·B + 2·R + 4·Q` over both sides, capped at 24. Opening ≥ 20,
middlegame 7–19, endgame ≤ 6, where 6 is `MOPUP_MAX_LOSER_PHASE`. `Eval.h` is the source of truth; a
drifted copy shifts bucket boundaries, it does not invalidate the numbers inside a bucket.

**Mate scores are counted, never averaged** — a mate score is not on the centipawn scale and
saturates any mean it enters. **An unrecognised comment aborts the run** rather than being skipped: a
format the parser does not understand means a changed fastchess version, and quietly dropping those
moves would bias every statistic toward whatever fastchess still spells the old way.

**The remaining clock is derived, not read.** fastchess writes the time *spent*; remaining clock is
reconstructed per side from `TimeControl` minus that side's running total, plus one increment a move.

**CI games start from an EPD book position** at fullmove 9, with `[SetUp "1"]` + `[FEN]`, so ply
counts run from the setup position, not the true game start. A CI PGN has zero `{book}` comments —
every move present is an engine move. A local `Run-EloMatch.ps1` run against a `.pgn` book does emit
them, and they are excluded.

## Limits

- **It grades its own homework, and the blind spot is large.** A position both builds misjudge the
  same way produces no swing at all. [Tier 2](#tier-2-external-adjudication) measures that spot:
  Tier 1 sees between a thirteenth and a fiftieth of the blunders actually made, the fraction falling
  as the phase gets earlier. Read Tier 2 before treating any Tier 1 rate as a defect profile.
- **The endgame is censored by adjudication.** 68% of the baseline's games ended by adjudication under
  `-draw movenumber=40 movecount=8 score=10` / `-resign movecount=4 score=800`. "Endgame" means *the
  position at the moment of adjudication*, not played-out technique, and the calibration table is
  meaningful only below ±800 — above that a win is very nearly definitional.
- **The reported score comes from the search that chose the move** — TT hits, aspiration windows, LMR
  re-searches — so it is a self-consistent series, not an absolute yardstick.
- **Both builds in a merge-base run are nearly identical**, so the cross-build gap measures the noise
  floor rather than real disagreement. It becomes informative when the two sides differ in eval.

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

Decisive 12,550 (62.8%, White 41.1% / Black 21.7%) · drawn 7,430 (37.2%) · ended by adjudication
13,589 (68.0%) · over 200 plies 1,204 (6.0%).

Draws by the reason fastchess recorded, against what the better-placed side reported on its **last**
move:

| Draw reason | Games | Of all | Last score ≥ 150 cp |
|---|---|---|---|
| 3-fold repetition | 4,393 | 22.0% | 0.1% |
| Insufficient mating material | 1,404 | 7.0% | **47.4%** |
| Adjudication (both sides ≈ 0.00) | 1,094 | 5.5% | 0% |
| Fifty-move rule | 507 | 2.5% | 0% |
| Stalemate | 32 | 0.2% | 0% |

### Blunder profile, contested positions only

Self-swing ≥ 150 cp, mover within ±150 cp before moving. Pooled over both builds, which differ by
under 0.05 percentage points in every cell.

| Phase | Moves | mean \|swing\| | ≥150 cp (95% clustered) | ≥300 cp |
|---|---|---|---|---|
| opening | 522,989 | 11.1 | 0.154% [0.144, 0.165] | 0.03% |
| middlegame | 522,161 | 12.8 | 0.293% [0.277, 0.308] | 0.09% |
| endgame | 393,243 | 9.9 | 0.215% [0.200, 0.231] | 0.07% |

| Piece | Moves | ≥150 cp (95% clustered) |
|---|---|---|
| bishop | 227,637 | 0.160% [0.144, 0.178] |
| knight | 195,017 | 0.200% [0.180, 0.220] |
| pawn | 289,165 | 0.204% [0.188, 0.220] |
| king | 241,601 | 0.216% [0.198, 0.236] |
| queen | 175,410 | 0.262% [0.238, 0.287] |
| rook | 309,563 | 0.277% [0.258, 0.295] |

Both sets of intervals are disjoint at the extremes — middlegame is 1.9× opening, rook 1.7× bishop —
so neither is flat, but both sit inside one narrow band, with heavy pieces at the top. Nothing here
resembles the 34%-of-blunders king-move story the unrestricted scan tells.

By the clock the mover **had when it started thinking**, not what was left afterwards; the two differ
for 5.0% of moves, and the band is a property of the decision, not its aftermath:

| Phase | > 8 s | 5–8 s | 2–5 s | < 2 s |
|---|---|---|---|---|
| opening | 0.03% | 0.17% | 0.41% | 0.30% |
| middlegame | 0.04% | 0.11% | 0.30% | **0.47%** |
| endgame | — | 0.00% † | 0.12% | 0.25% |

† zero blunders in 1,293 moves — the engine is almost never in an endgame with that much clock left.

### Eval calibration

Observed score for the side to move, by the score it reported, split on whether both sides still hold
at least three pawns. The split is the control separating "the endgame eval is optimistic" from "the
eval is blind to drawish material".

| Reported | opening (rich) | middlegame (rich) | endgame (rich) | middlegame (poor) | endgame (poor) |
|---|---|---|---|---|---|
| +100–150 | 0.735 | 0.750 | 0.744 | 0.692 | 0.631 |
| +200–250 | 0.889 | 0.895 | 0.908 | 0.831 | 0.749 |
| +250–300 | — | — | — | 0.888 | 0.789 |
| +300–350 | 0.959 | 0.965 | 0.983 | 0.882 | **0.764** |
| +500–550 | 1.000 | 0.991 | 0.997 | 0.997 | **0.897** |

Pawn-rich, the three phases agree within noise. Pawn-poor (either side ≤ 2 pawns) the endgame column
falls away and stops being monotonic.

### Drawish material classes

Counted once per game at the first position reaching the class, against what the stronger side
reported there. Games, not plies — plies over-weight exactly the long drawn games. **Score bands are
exclusive**: `+100–249` excludes `≥ +250`, and the cumulative row is the one to quote for "the engine
thought it was winning".

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
opening, **14.8 cp** middlegame, **17.8 cp** endgame. A run whose builds differ in evaluation should
exceed this; one that does not has not changed what the engine believes.

**This is an upper bound, not an estimate of disagreement.** The two scores are one ply apart: X
reports the value of the position it hands over, Y reports its own value with its own search and
clock. Under the negamax identity the sum is zero when the builds agree — X plays its own PV move, so
the *move* contributes nothing — but the two searches are not the same search, and that residual is
inside the number.

A clean version exists for a subset: identical FENs that **both** builds actually scored, no ply
offset. Within a shard there are 46,389, and the gap is **5.4 cp** — but 93% are opening positions
and only 469 endgames, because the builds rarely reach the same middlegame. It cross-checks the
opening row (9.1 against 5.4, the difference being the one-ply offset); it cannot replace it.

---

## Findings

**1. ~~There is no general blunder weakness to find.~~ Retracted by
[Tier 2](#tier-2-external-adjudication).** The claim rested on the engine grading its own homework.

What survives is a statement about self-knowledge, not about play: *of the mistakes the engine can
see*, every rate sits between 0.15% and 0.30% by phase and by piece, ordered middlegame-worst and
heavy-pieces-worst, which is what a depth-limited search should look like. The unrestricted numbers
say something else entirely — endgame 1.7%, king moves 34% of all blunders — and both are artifacts
of already-lost positions where the swings cost nothing. Use the contested rows.

**2. The evaluation is well calibrated except when the pawns are gone.** With three pawns a side the
three phases agree within noise, and the endgame is marginally *better* calibrated than the
middlegame at large scores. With two pawns or fewer the endgame curve collapses and stops being
monotonic — +300 converts at 0.764 while +250 converts at 0.789 and +575 at 0.998. Not a tapering or
phase-calibration defect; the absence of drawish-material knowledge (#128).

**3. The dips have names, and they are not the ones #128 predicted.** The +300–350 dip is dominated
by **rook + minor vs rook**, a fortress the evaluator scores a full piece up: 262 games reach it with
the stronger side reporting ≥ +250, and it scores 0.645. Rook vs minor at +100–249 is a dead draw in
practice (0.509 over 58 games); across ≥ +100 it is 0.720 over 209, because the ≥ +250 half is often
a real win. And 656 games — **3.3% of the entire run** — end in K + minor vs K with the stronger side
reporting ≥ +250; every one is a draw. In 26.8% of insufficient-material draws the claiming side
still had a pawn two plies earlier, 44.1% six plies earlier: it trades its last pawn into a dead draw
while reporting a piece up.

**4. Opposite-coloured bishops are not worth a scale factor.** The one measured negative result: pure
OCB at ≥ +250 converts at 0.881, close enough to the pawn-rich curve that scaling toward zero would
more likely cost Elo than gain it.

**5. Repetition draws are not squandered wins.** 22.0% of games end in threefold repetition, but in
99.9% of them both sides' last reported score was under 50 cp. The engine repeats from positions it
believes equal — a contempt/playing-on decision (#76 direction 3), not a defect.

**6. Time pressure is real but second-order.** 21% of contested middlegame decisions *start* with
under 2 s on the clock, and the blunder rate there is 0.47% against 0.11% at 5–8 s. Causality runs
both ways — long complex games both consume clock and contain more mistakes — so this bounds the
prize rather than measuring it (#103).

**7. Two invariants confirmed in production data.** All 507 fifty-move draws fire at a halfmove clock
of exactly 100, so #345's fix holds at scale. And no build ever announced a forced mate and failed to
win: 1,462 and 1,344 such games, all won.

---

## Addendum: level-material rook endings

`RvsR` (pawnless K+R vs K+R) was added to `material_classes()` after the baseline, so these rows come
from later, post-#128 runs and are not comparable ply-for-ply with the table above. Two independent
corpora of 19,980 games each; the second carries the #436 scale on one side only.

| Class | Reported | 33429454765 | 33568346899 |
|---|---|---|---|
| KR vs KR (pawnless) | < +100 | 368 games, **0.500** [0.500, 0.500] | 361 games, **0.500** [0.500, 0.500] |
| KR vs KR (pawnless) | ≥ +250 | 309 games, **1.000** [1.000, 1.000] | 307 games, **1.000** [1.000, 1.000] |
| KR vs KR (pawnless) | level, no side | 224 games, drawn 100.0% | 289 games, drawn 100.0% |

The class separates completely, on corpora sharing no games, and the band that matters is the low one
— the reverse of every other class here. **Below +100 the reported edge did not predict a decisive
result once**, and the games entered dead level were drawn too. At ≥ +250 the score does predict one,
because it comes from a leaf where the rook has already fallen and the material is no longer this
class. Nothing lands in between.

Read that as calibration, not absence of information: the magnitude in the low band carries no signal
about the result, but whether its ordering still steers the search usefully is a separate question
these rows cannot answer. That is why #436 discounts the class rather than zeroing it. The low band
is the PST, rook-file and mobility terms surviving on a level-material board — median 22 cp, maximum
64, over 400 positions sampled from the first run.

The second run shows the scale itself: `|score|` reported from inside the class has p90 **13**
unscaled and **3** scaled, 4/16 to the digit. **No steering effect is visible** — the builds enter the
class 777 and 784 times and spend 6.6 and 6.8 plies there — and the low band offers none to find,
being already saturated at 0.500.

**Level-material classes name no stronger side.** `RvsR` and `OCB` hold the same material on both
sides, so `material_classes()` returns `None` for colour and the caller resolves one from the **sign
of the entry score** — which works only while that score is non-zero. A quarter to a third of `RvsR`
entries are exactly 0, and a `>= 0` test hands every one to White, which then appears as the stronger
side about 2.5× as often as Black with its colour advantage credited to a side that does not exist.
Those games now sit on their own **level, no side** line reporting the drawn rate, the only statistic
defined without a stronger side. No exact-zero entry in either run was decisive (557 games, all
drawn), so the published rows lost nothing to the old rule. The guard is in `--self-test`: three
level-material games, all won by White, one entered at each sign.

**The rows stay pooled-only.** A per-build split of a level-material class measures the builds'
scales, not their play: scaling compresses small scores toward zero, moving entries down the
magnitude bands. Run `33568346899`'s `< +100` band splits **145 candidate / 216 reference** — 3.7
standard errors, and it looks exactly like the change's intended effect. It is not. The skew sits
entirely in the `|cp| 1–24` bucket the scale compresses; from `|cp| ≥ 25` up it is 150/142, and the
control run, where neither build scaled the class, splits 202/166 the other way.

The [Baseline](#baseline-run-33215162562) table predates the level line, so its `OCB` `< +100` row
still folds in that run's dead-level entries.

---

## Tier 2: External adjudication

Run [33989392373](https://github.com/theEscape2207/StratChess/actions/runs/33989392373), 2026-09-05,
`candidate-86877f7` (`worktree-minor-piece-outposts`) vs `reference-9708c65` (merge-base), pooled
**+8.05 ± 3.63 Elo** over 9,990 pairs. Oracle Stockfish 19 at depth 12. 19,958 games, **1,492,860
contested rows**, 2,000 game-clustered resamples. Same corpus, parser, phase buckets and ±150 cp
contested filter as Tier 1 — only the judge changes.

Loss for one move is `max(0, oracle(before) − oracle(after))` from the mover's point of view, clamped
to ±1000 cp so a mate score cannot saturate a mean. `agree%` is how often the played move was the
oracle's first choice; `noise` is the mean loss over exactly those rows, where the residual can only
be search instability rather than a mistake.

**`noise` is a conditional residual, not the oracle's error bar.** The rows it averages are the ones
the oracle already agreed with — the narrower positions, by [T4](#findings-1) — and narrow positions
are the stable ones. It is a *lower bound* on the oracle's error and silent about the disagreement
rows carrying the entire signal. Read it as a floor. #483 would replace it: mean
`|loss(d) − loss(d+1)|` over a sample drawn regardless of agreement.

| build | phase | n | self ACPL | self blu% | ext ACPL (95%) | ext blu% (95%) | agree% | noise |
|---|---|---|---|---|---|---|---|---|
| candidate | opening | 258,372 | 11.3 | 0.13 | **40.3** [39.9, 40.6] | **6.30** [6.18, 6.41] | 41.7 | 4.5 |
| candidate | middlegame | 272,400 | 12.9 | 0.28 | 33.9 [33.6, 34.3] | 5.70 [5.60, 5.81] | 47.9 | 4.4 |
| candidate | endgame | 214,381 | 9.9 | 0.21 | 16.9 [16.5, 17.3] | 2.83 [2.73, 2.93] | 42.3 | 2.8 |
| reference | opening | 258,835 | 11.1 | 0.12 | **40.9** [40.6, 41.2] | **6.43** [6.32, 6.55] | 41.2 | 4.6 |
| reference | middlegame | 273,660 | 12.7 | 0.27 | 34.2 [33.9, 34.6] | 5.70 [5.59, 5.81] | 47.7 | 4.6 |
| reference | endgame | 215,212 | 9.9 | 0.24 | 16.9 [16.5, 17.3] | 2.86 [2.75, 2.96] | 42.4 | 2.8 |

### Findings

**T1. The self-reported blunder rate understates the real one by 13× to 48×.** Endgame 2.83% against
0.21%, middlegame 5.70% against 0.28%, opening 6.30% against 0.13%. This is the blind spot named in
[Limits](#limits), measured rather than assumed, and it retracts [Finding 1](#findings).

**T2. The profile is monotone, and it points the wrong way.** Self-ACPL is nearly constant across the
phases (11.3 / 12.9 / 9.9); external ACPL climbs 16.9 → 33.9 → 40.3 from endgame to opening, and the
blunder rate climbs with it. **The engine plays worst where it is most confident.** Tier 1 reads the
opening as its *best* phase; the outside judge makes it the worst by both measures, on disjoint
intervals.

One mundane explanation must be excluded before any other: the ±150 cp contested filter selects on
the engine's own score, and in the opening that score is least informative, so the filter admits
nearly every opening move while filtering the endgame hard. #481 tracks it; step 1 there is the
filter-independent re-run that settles it. **#484** is the work that turns this into an attribution —
search or evaluation — rather than a rate.

**T3. Quote the blunder rates; the ACPL means carry a floor of unmeasured size.** External ACPL is 6×
to 9× the `noise` column, but that column is a lower bound, so the ratio is an *upper* bound on the
signal-to-noise, not the reassurance it looks like. What does not depend on it: 6.3% of opening rows
lose ≥ 150 cp as a difference of two same-depth searches, and no plausible depth-12 instability
manufactures a 150 cp gap at that rate. The floor does behave as a floor should — 2.8 cp where
positions are simple, 4.4–4.6 where they are not — which at least checks the oracle is not
misconfigured.

**T4. Agreement is under half, and does not track quality.** The played move is the oracle's first
choice 41.7% of the time in the opening and 47.9% in the middlegame — the phase with the *worse* ACPL
agrees *more*. Agreement measures how narrow the position is, not how well it was played.

**T5. Candidate beats reference in the opening only.** Opening ACPL 40.3 against 40.9 — disjoint,
~0.6 cp. Middlegame overlaps, endgame is identical to the decimal, and blunder-rate intervals overlap
in every phase. Both builds' rows come from the same games (each plays each opening once, colours
reversed), which is what makes a difference this small readable. Consistent with the run's +8.05 Elo
without being evidence for it.

### Limits specific to Tier 2

- **Depth 12 is a judge, not the truth** — roughly the engine's own search depth. Losses under ~30 cp
  are within its instability; only the aggregate is meaningful.
- **The ±1000 cp clamp compresses the tail.** Most of the report's worst rows sit exactly at −1000,
  meaning "lost or mated", not "lost by ten pawns". Counts of clamped rows are interpretable; their
  mean is not.
- **Scores are path-dependent inside a game, independent across games.** Each game opens with
  `ucinewgame`, which clears the hash, so a game's numbers cannot depend on which games preceded it.
  Within a game nothing is cleared: the `after` search inherits the hash `before` warmed, and later
  rows inherit earlier ones. A row's loss is therefore a difference of two *correlated* searches, and
  re-scoring a row in isolation will not always reproduce it.
- **The contested filter is the engine's own.** It selects on the mover's reported score, so it is
  not independent of the quantity being measured. T2's caveat is the concrete consequence.
