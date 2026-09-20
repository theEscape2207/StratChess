# Move-Quality Scan — Method

A strength run's pooled Elo says *whether* a change helped and nothing about *where*. The lab
writes a fully annotated PGN of every game; two tools read it.
`Scripts/analyze_move_quality.py` answers "where" from the engine's own annotations.
`Scripts/analyze_external_quality.py` answers it again with an outside engine as judge — the only
way to see mistakes the engine does not know it made.

**This file is the instrument. The numbers are in `Measurements/`** —
[`move-quality-tier1.md`](../Measurements/move-quality-tier1.md) for Tier 1 and
[`move-quality-tier2.md`](../Measurements/move-quality-tier2.md) for Tier 2, append-only. #448
separated the Elo measurements the same way, though it dissolved the method into the
`measure-strength` skill rather than leaving it in `Docs/`; here the method stays, on the
[`MoveQualityExport.md`](MoveQualityExport.md) precedent (#486).

| Need | Section |
|---|---|
| run it | [Regenerating](#regenerating) |
| what the numbers mean, and cannot see | [Method](#method) · [Limits](#limits) |
| the numbers | [Tier 1 ledger](../Measurements/move-quality-tier1.md) · [Tier 2 ledger](../Measurements/move-quality-tier2.md) |
| what they established | [Findings](#findings) · [Tier 2 findings](#findings-1) |
| the exported blunder evidence | [MoveQualityExport.md](MoveQualityExport.md) |

---

## Regenerating

Artifacts are retained 90 days, ~1 MB per shard; a production run is 18 shards.

```sh
gh run download <run_id> --repo theEscape2207/StratChess -p 'strength-<run_id>-shard-*' -D pgn
python Scripts/analyze_move_quality.py pgn --self-check      # the gate; run it first
python Scripts/analyze_move_quality.py pgn --json stats.json
python Scripts/analyze_external_quality.py pgn --depth 12 --json external.json
python Scripts/analyze_external_quality.py pgn --depth 12 --worst-jsonl evidence.jsonl
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

`--worst-jsonl` writes one record per faulted row — the evidence a later attribution stage replays,
rather than the twenty rows the report prints. The footer is written the moment scoring finishes, so
an interrupted scan leaves a footer-less file that the reader rejects, and a failed report still
leaves a usable one. The destination must not already exist; there is no resume. Schema, population
and retry rules: [MoveQualityExport.md](MoveQualityExport.md).

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
for one side; `--self-check` fails loudly on a corpus that violates it, and on the
[baseline run](../Measurements/move-quality-tier1.md#run-33215162562--baseline)
reports the losing side at **+22.26 cp** mean signed self-swing against the winning side's
**−21.86 cp**.

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

**CI games start from an EPD book position** at fullmove 9, with `[SetUp "1"]` + `[FEN]`, so the
movetext is short of the plies that led there. Game length adds them back — the histogram and the
over-200 counter are **total plies**, so a CI game and a normal-start game are the same measurement.
A CI PGN has zero `{book}` comments — every move present is an engine move. A local
`Run-EloMatch.ps1` run against a `.pgn` book does emit them; they are excluded from the move
statistics but present in the movetext, so the length never double-counts a book.

**Score bands in a class table are exclusive.** `+100–249` excludes `≥ +250`; where the table
carries a cumulative row, that is the one to quote for "the engine thought it was winning".

**A clock bucket is what the mover had when it started thinking**, not what was left afterwards. The
two differ, so the bucket is a property of the decision rather than of its aftermath; how often they
differ is a fact about a particular run and belongs in its ledger section.

**This is an upper bound, not an estimate of disagreement.** The two scores are one ply apart: X
reports the value of the position it hands over, Y reports its own value with its own search and
clock. Under the negamax identity the sum is zero when the builds agree — X plays its own PV move, so
the *move* contributes nothing — but the two searches are not the same search, and that residual is
inside the number.

**Level-material classes name no stronger side.** `RvsR` and `OCB` hold the same material on both
sides, so `material_classes()` returns `None` for colour and the caller resolves one from the **sign
of the entry score** — which works only while that score is non-zero. A quarter to a third of `RvsR`
entries are exactly 0, and a `>= 0` test hands every one to White, which then appears as the stronger
side about 2.5× as often as Black with its colour advantage credited to a side that does not exist.
Those games now sit on their own **level, no side** line reporting the drawn rate, the only statistic
defined without a stronger side. No exact-zero entry in either run was decisive (557 games, all
drawn), so the published rows lost nothing to the old rule. The guard is in `--self-test`: three
level-material games, all won by White, one entered at each sign.

**Level-material rows stay pooled-only.** A per-build split of a level-material class measures the
builds' scales, not their play: scaling compresses small scores toward zero, moving entries down the
magnitude bands, which looks exactly like a change's intended effect and is not. The measurement
behind that rule, and the control run that settles it, are in the level-material
[`Row detail`](../Measurements/move-quality-tier1.md#row-detail-1).

## Limits

- **It grades its own homework, and the blind spot is large.** A position both builds misjudge the
  same way produces no swing at all. [Tier 2](#tier-2-external-adjudication) measures that spot:
  Tier 1 sees between a thirteenth and a fiftieth of the blunders actually made, the fraction falling
  as the phase gets earlier. Read Tier 2 before treating any Tier 1 rate as a defect profile.
- **The endgame is censored by adjudication.** 68% of the games in the
  [baseline run](../Measurements/move-quality-tier1.md#run-33215162562--baseline) ended by
  adjudication under `-draw movenumber=40 movecount=8 score=10` / `-resign movecount=4 score=800`.
  "Endgame" means *the position at the moment of adjudication*, not played-out technique, and the
  calibration table is meaningful only below ±800 — above that a win is very nearly definitional.
- **The reported score comes from the search that chose the move** — TT hits, aspiration windows, LMR
  re-searches — so it is a self-consistent series, not an absolute yardstick.
- **Both builds in a merge-base run are nearly identical**, so the cross-build gap measures the noise
  floor rather than real disagreement. It becomes informative when the two sides differ in eval.

---

## Findings

What the runs in the [Tier 1 ledger](../Measurements/move-quality-tier1.md) establish about the
engine and about the scan. The dividing line against the ledgers is mechanical: a reading that
**pools both builds** of a run is here, and a reading that **compares the two builds** belongs to
that run and stays in its `Row detail`. Several of these rest on a single run's table — a limit on
how much weight they carry, not a reason to move them.

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

## Tier 2: External adjudication

The same rows, judged by Stockfish at depth 12 instead of by the engine that played them: one run's
own PGNs, read with the same parser, phase buckets and ±150 cp contested filter Tier 1 applies to
them — only the judge changes. That correspondence holds *within* a run; a Tier 2 run is not the
same match as any run in the Tier 1 ledger. The runs are in the
[Tier 2 ledger](../Measurements/move-quality-tier2.md).

Loss for one move is `max(0, oracle(before) − oracle(after))` from the mover's point of view, clamped
to ±1000 cp so a mate score cannot saturate a mean. `agree%` is how often the played move was the
oracle's first choice; `noise` is the mean loss over exactly those rows, where the residual can only
be search instability rather than a mistake.

**`noise` is a conditional residual, not the oracle's error bar.** The rows it averages are the ones
the oracle already agreed with — the narrower positions, by [T4](#findings-1) — and narrow positions
are the stable ones. It is a *lower bound* on the oracle's error and silent about the disagreement
rows carrying the entire signal. Read it as a floor. #483 would replace it: mean
`|loss(d) − loss(d+1)|` over a sample drawn regardless of agreement.

### Findings

**T1. The self-reported blunder rate understates the real one by 13× to 48×.** Endgame 2.83% against
0.21%, middlegame 5.70% against 0.28%, opening 6.30% against 0.13% — the candidate build's rows; the
reference build's give 11.9× to 53.6×, the same conclusion over a wider spread. This is the blind
spot named in [Limits](#limits), measured rather than assumed, and it retracts
[Finding 1](#findings).

**T2. The profile is monotone, and it points the wrong way.** Self-ACPL is nearly constant across the
phases (11.3 / 12.9 / 9.9); external ACPL climbs 16.9 → 33.9 → 40.3 from endgame to opening, and the
blunder rate climbs with it — again the candidate rows, the reference within 0.6 cp of each.
**The engine plays worst where it is most confident.** Tier 1 reads the
opening as its *best* phase; the outside judge makes it the worst by both measures, on disjoint
intervals.

One mundane explanation must be excluded before any other: the ±150 cp contested filter selects on
the engine's own score, and in the opening that score is least informative, so the filter admits
nearly every opening move while filtering the endgame hard. #481 tracks it; step 1 there is the
filter-independent re-run that settles it. #484 added a lossless export for later diagnostic replay;
it did not make that replay a search-versus-evaluation classifier. A move improving with additional
search establishes budget sensitivity, while one that does not remains unresolved. Attribution needs
separate evidence for a specific mechanism.

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

T5 was a comparison of the two builds in one run, not a property of the engine, and is the
[Tier 2 ledger](../Measurements/move-quality-tier2.md)'s row detail.

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
  re-scoring a row in isolation will not always reproduce it. Measured once, by halving a game's
  searches: external ACPL moved in 11 of 12 phase cells, mean +0.28 cp, one cell past its published
  95% half-width on both shards tested
  ([#582](https://github.com/theEscape2207/StratChess/issues/582#issuecomment-5750971773)). Repeated
  runs are byte-identical, so that size is a property of the scoring protocol: change how rows are
  scored and the table moves, at unchanged rows, oracle and depth.
- **The contested filter is the engine's own.** It selects on the mover's reported score, so it is
  not independent of the quantity being measured. T2's caveat is the concrete consequence.
