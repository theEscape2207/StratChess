# Move-quality scan — Tier 2

Runs of `Scripts/analyze_external_quality.py`: the same rows as [Tier 1](move-quality-tier1.md),
re-judged by an outside engine instead of by the engine that played them. The instrument — the
loss formula, what `agree%` and `noise` do and do not mean, and the limits specific to an oracle
— is [`../Docs/MoveQuality.md`](../Docs/MoveQuality.md). This file holds only what each run
measured.

**These are profile tables, not verdict rows.** A Tier 1 profile of a *different* run is not
comparable against these — different corpus, different judge. The licensed comparison is the `self`
columns in the table below, which reproduce Tier 1 on exactly the rows the oracle judged. See the
carve-out in [`README.md`](README.md).

---

## Run 33989392373 — minor-piece outposts vs merge base

Run [33989392373](https://github.com/theEscape2207/StratChess/actions/runs/33989392373), 2026-09-05,
`candidate-86877f7` (`worktree-minor-piece-outposts`) vs `reference-9708c65` (merge-base), pooled
**+8.05 ± 3.63 Elo** over 9,990 pairs. Oracle Stockfish 19 at depth 12. 19,958 games, **1,492,860
contested rows**, 2,000 game-clustered resamples. Same corpus, parser, phase buckets and ±150 cp
contested filter as Tier 1 — only the judge changes.

| build | phase | n | self ACPL | self blu% | ext ACPL (95%) | ext blu% (95%) | agree% | noise |
|---|---|---|---|---|---|---|---|---|
| candidate | opening | 258,372 | 11.3 | 0.13 | **40.3** [39.9, 40.6] | **6.30** [6.18, 6.41] | 41.7 | 4.5 |
| candidate | middlegame | 272,400 | 12.9 | 0.28 | 33.9 [33.6, 34.3] | 5.70 [5.60, 5.81] | 47.9 | 4.4 |
| candidate | endgame | 214,381 | 9.9 | 0.21 | 16.9 [16.5, 17.3] | 2.83 [2.73, 2.93] | 42.3 | 2.8 |
| reference | opening | 258,835 | 11.1 | 0.12 | **40.9** [40.6, 41.2] | **6.43** [6.32, 6.55] | 41.2 | 4.6 |
| reference | middlegame | 273,660 | 12.7 | 0.27 | 34.2 [33.9, 34.6] | 5.70 [5.59, 5.81] | 47.7 | 4.6 |
| reference | endgame | 215,212 | 9.9 | 0.24 | 16.9 [16.5, 17.3] | 2.86 [2.75, 2.96] | 42.4 | 2.8 |

### Row detail

**T5. Candidate beats reference in the opening only.** Opening ACPL 40.3 against 40.9 — disjoint,
~0.6 cp. Middlegame overlaps, endgame is identical to the decimal, and blunder-rate intervals overlap
in every phase. Both builds' rows come from the same games (each plays each opening once, colours
reversed), which is what makes a difference this small readable. Consistent with the run's +8.05 Elo
without being evidence for it.

**The opening bucket split by plies since book exit.** A re-score of shards 0, 1, 10 and 11 of this
run at the same depth and settings — 4,437 games, 330,237 contested rows, `--by-book-exit`:

| build | band | n | share of bucket | ext ACPL (95%) | ext blu% (95%) | agree% |
|---|---|---|---|---|---|---|
| candidate | 0-3 | 8,738 | 15.3% | 27.2 [26.3, 28.1] | 2.08 [1.80, 2.41] | 43.2 |
| candidate | 4-9 | 12,468 | 21.8% | 33.2 [32.2, 34.2] | 3.95 [3.61, 4.32] | 42.1 |
| candidate | 10+ | 36,037 | 63.0% | **45.7** [44.8, 46.7] | 8.07 [7.74, 8.42] | 40.9 |
| reference | 0-3 | 8,747 | 15.2% | 27.8 [26.8, 28.8] | 2.42 [2.09, 2.77] | 43.8 |
| reference | 4-9 | 12,496 | 21.8% | 34.1 [33.1, 35.1] | 4.07 [3.71, 4.44] | 41.7 |
| reference | 10+ | 36,153 | 63.0% | **46.2** [45.3, 47.2] | 8.31 [7.98, 8.66] | 40.4 |

**A four-shard sub-sample of this run, not a second run** — its phase table reproduces the one above
(opening 40.1, middlegame 34.9, endgame 16.7 for the candidate), which is what licenses reading the
two together. Every game's boundary is `setup_assumed`: the lab starts from an EPD position recorded
as the setup FEN, so the band is plies since that position. The reading this supports is under
[T2](../Docs/MoveQuality.md#findings-1).

**The same rows re-judged at depth 20.** A phase-stratified sample of the same four shards — 2,000
contested rows per build and phase, 12,000 in all, 3,766 games, seed 20260921 — scored at depth 12
and again at depth 20 under a **row-isolated** protocol: every row gets its own `ucinewgame` at each
depth, so neither depth inherits a hash the other warmed. The d12 column here is that isolated
control, **not** the published cells above; reading d20 against those would confound depth with
protocol.

| build | phase | n | ACPL d12 | ACPL d20 | shift | blu% d12 | blu% d20 | agree% d12 | agree% d20 | mean \|Δ\| |
|---|---|---|---|---|---|---|---|---|---|---|
| candidate | opening | 2,000 | 40.6 | 48.0 | +7.4 | 6.10 | 8.20 | 43.0 | 41.6 | 22.6 |
| candidate | middlegame | 2,000 | 33.4 | 42.2 | +8.8 | 5.30 | 8.15 | 46.2 | 46.9 | 24.3 |
| candidate | endgame | 2,000 | 14.6 | 22.3 | +7.7 | 2.15 | 4.45 | 39.8 | 41.9 | 16.4 |
| reference | opening | 2,000 | 41.2 | 48.6 | +7.4 | 6.25 | 8.05 | 41.1 | 41.2 | 23.4 |
| reference | middlegame | 2,000 | 35.3 | 45.4 | +10.1 | 5.65 | 9.20 | 48.9 | 48.1 | 27.0 |
| reference | endgame | 2,000 | 16.1 | 23.0 | +6.8 | 2.35 | 4.35 | 41.9 | 43.1 | 16.5 |

Phase gaps, 95% intervals, games resampled:

| build | pair | d12 | d20 |
|---|---|---|---|
| candidate | opening − endgame | 26.1 [22.5, 29.6] | 25.8 [20.5, 30.6] |
| reference | opening − endgame | 25.1 [21.7, 28.4] | 25.6 [20.9, 30.7] |
| candidate | opening − middlegame | 7.2 [3.0, 11.3] | 5.8 [0.3, 11.3] |
| reference | opening − middlegame | 5.9 [1.8, 9.7] | 3.3 [−2.3, 8.8] |

Every phase gains +6.8 to +10.1 cp at the deeper judge, so the depth effect is a level shift and not
the phase-differential one that could have produced the ordering. The opening-to-endgame gap is
unmoved; the opening-to-middlegame margin thins ~31% pooled and crosses zero for the reference. The
reading this supports is under [T2](../Docs/MoveQuality.md#findings-1) and
[T3](../Docs/MoveQuality.md#findings-1).

`mean |loss(d12) − loss(d20)|`, the unconditional oracle floor, by phase and by the band the
shallower judge put the row in:

| phase | all | 0–150 | 150–300 | 300–600 | 600+ |
|---|---|---|---|---|---|
| opening | 23.0 | 19.7 | 71.2 | 76.5 | 163.5 |
| middlegame | 25.7 | 21.3 | 96.0 | 127.8 | — |
| endgame | 16.4 | 13.6 | 132.1 | 156.8 | — |

Cost, for anyone pricing a repeat: 2,599 s of wall time at `--jobs 12`, of which depth 12 is
46 ms per search and depth 20 is 1.24 s per search. Row isolation makes every search cold, which is
19× the 2.44 ms a warm depth-12 search costs in the production protocol.

**The same rows under three selection rules.** Shards 0–1 of this run, production protocol, oracle
depth 12, with the eligibility filter lifted to structural only — a numeric score on ply *i* and on
the same mover's ply *i+2*, nothing else. **249,552 rows over 2,220 games**, every row recording the
engine's score and both oracle endpoints, so all three views are rebuilt from one scored population
rather than from three runs differing by sampling as well as by rule. The parse reproduces the
contested counts of the four-shard sub-sample exactly (82,562 rows in shard 0), and the
engine-contested view reproduces the published cells with every interval overlapping.

Mean oracle loss in cp, candidate / reference, game-clustered intervals over 2,220 games:

| selection rule | rows | opening | middlegame | endgame | opening − endgame |
|---|---|---|---|---|---|
| engine-contested, `\|engine_before\| ≤ 150` (published) | 166,461 | 39.9 / 40.4 | 34.7 / 34.5 | 15.8 / 16.3 | 24.0 / 24.2 |
| oracle-contested, `\|oracle_before\| ≤ 150` | 133,244 | 32.4 / 33.4 | 25.9 / 25.4 | 11.4 / 11.8 | 21.0 / 21.6 |
| unfiltered, structural only | 249,552 | 40.6 / 41.0 | 36.1 / 35.8 | 21.8 / 22.2 | 18.7 / 18.8 |

Unfiltered `opening − endgame` is 18.7 [17.5, 20.1] and 18.8 [17.4, 20.2]; `opening − middlegame` is
4.5 [3.3, 5.8] and 5.3 [4.0, 6.5].

What the filter keeps, and what the rows it drops are worth — mean oracle loss by the band the
engine's own score put the row in, both builds pooled:

| phase | rows kept by the filter | 0–50 | 50–150 | 150–400 | 400–1000 | 1000+ |
|---|---|---|---|---|---|---|
| opening | 94.8% | 37.2 | 43.8 | 54.2 | 38.4 | 11.0 |
| middlegame | 67.3% | 29.1 | 39.8 | 41.2 | 33.1 | 15.9 |
| endgame | 48.6% | 11.2 | 19.6 | 29.8 | 24.6 | 25.6 |

The filter barely touches the opening and discards more than half the endgame, and the endgame rows
it discards carry more loss than the ones it keeps — so it understates the endgame and widens the
gap rather than creating it. The oracle-contested row is a cross-check only: conditioning on one
endpoint of the difference being measured truncates the distribution and biases loss downward, which
is why its means are uniformly lower. The unfiltered row is the one selected by nothing.

The engine and the oracle also disagree about which positions are level, asymmetrically by phase:

| phase | both call it contested | engine only | oracle only | mean loss, engine-only rows |
|---|---|---|---|---|
| opening | 41,547 | 16,441 | 276 | 58.8 |
| middlegame | 40,067 | 20,792 | 2,151 | 52.7 |
| endgame | 40,287 | 7,327 | 8,916 | 48.0 |

That last table compares searched scores at unequal depth and admits lost positions as well as won
ones, so it is a prior rather than a finding;
[#593](https://github.com/theEscape2207/StratChess/issues/593) is the instrument for it. Cost: 705 s
at `--jobs 12`, 708 searches/s aggregate. The reading this supports is under
[T2](../Docs/MoveQuality.md#findings-1).

**Search or evaluation, on the rows the judge faulted.** The 4,145 rows exported by #484
(`legacy_loss_cp >= 150`), less the 15 where the oracle's preferred move *is* the played move —
**4,129 comparisons**, zero errors. Each is asked of the build that played it, rebuilt from its own
commit, at the depth that build reached in the game: the position after the played move and the
position after the oracle's move are both searched to `engine_depth`, cold, and the two values
compared from the mover's point of view. A dead band of ±10 cp is reported rather than assigned.
`v_oracle > v_played` means the engine rated the move it did not play higher — the knowledge was
present and the root search did not deliver it. `v_oracle < v_played` means it preferred its own
move on the evidence it had.

Re-searching the *played* move reproduces the score the engine wrote during the game to **a median
of 0.0 cp** (mean 6.1 / 5.5, p90 14 / 13, exact on 58.3% / 57.8% of rows), against a predeclared
tolerance of 25 cp. The lab plays GCC on Linux and this instrument is clang-cl on Windows, so that
also measures the two toolchains against each other at fixed depth and finds no difference.

| | rows | search failure % | tie % | evaluation failure % |
|---|---|---|---|---|
| all | 4,129 | 20.1 [18.9, 21.3] | 30.0 | 49.9 [48.2, 51.6] |
| opening | 1,802 | 18.1 [16.3, 20.0] | 29.9 | 52.0 [49.4, 54.6] |
| middlegame | 1,728 | 20.3 [18.4, 22.2] | 29.1 | 50.6 [48.2, 53.1] |
| endgame | 599 | 25.4 [21.8, 29.3] | 32.9 | 41.7 [37.0, 46.5] |
| loss 150–250 cp | 2,759 | 17.7 [16.2, 19.1] | 31.5 | 50.9 [48.8, 52.8] |
| loss 250–400 cp | 1,051 | 23.2 [20.7, 25.8] | 28.4 | 48.3 [45.0, 51.6] |
| loss ≥ 400 cp | 319 | 31.0 [25.8, 36.3] | 22.3 | 46.7 [40.6, 52.6] |
| candidate | 2,057 | 20.9 [19.1, 22.7] | 29.8 | 49.3 [47.0, 51.6] |
| reference | 2,072 | 19.4 [17.6, 21.0] | 30.2 | 50.5 [48.2, 52.7] |

The ratio is not an artifact of the band: at ±0 cp it is 31.3 / 3.2 / 65.5, at ±5 cp 24.8 / 17.9 /
57.3, at ±25 cp 11.8 / 55.9 / 32.2. The two builds agree throughout. Cost: 67 s and 68 s at
`--jobs 12`, ~61 searches/s aggregate.

**The same rows six plies deeper.** At the depth the games were played the engine's verdict on the
two moves is flat — a median gap of −11 cp where the depth-12 oracle sees 211. That is consistent
with an evaluation that cannot separate them, and equally with a refutation beyond the horizon, so a
**1,200-row sample** (600 per build, drawn from the same global shuffle) was re-scored at
`engine_depth + 6`, which puts the engine at a median depth of 15 — deeper than the judge that
faulted the rows.

| same 1,200 rows | median search depth | search failure % | tie % | evaluation failure % |
|---|---|---|---|---|
| as played | 9 | 20.4 [18.3, 22.7] | 29.2 | 50.3 [47.4, 53.1] |
| plus 6 plies | 15 | 53.5 [50.5, 56.5] | 20.7 | 25.8 [23.2, 28.4] |

**Half the rows change class (49.9%), and 35.3% move into search failure.** The effect is strongest
where the mistakes are largest, and it is not confined to the games where the engine searched
shallowest:

| | rows | search % as played | search % at +6 |
|---|---|---|---|
| loss 150–250 cp | 796 | 18.1 [15.6, 20.8] | 47.1 [43.6, 50.6] |
| loss 250–400 cp | 312 | 23.7 [19.2, 28.6] | 62.5 [57.0, 67.9] |
| loss ≥ 400 cp | 92 | 29.3 [19.6, 39.1] | 78.3 [70.4, 86.7] |
| opening | 507 | 18.9 [15.6, 22.4] | 49.7 [45.4, 54.1] |
| middlegame | 502 | 20.5 [16.8, 24.2] | 55.8 [51.3, 60.6] |
| endgame | 191 | 24.1 [17.6, 31.1] | 57.6 [50.8, 64.6] |
| game depth ≤ 9 | 348 | 14.4 [10.7, 18.1] | 56.6 [51.5, 62.0] |
| game depth 10–11 | 564 | 23.6 [20.0, 27.2] | 52.7 [48.4, 57.0] |
| game depth ≥ 12 | 288 | 21.5 [16.8, 26.4] | 51.4 [45.5, 56.7] |

**310 rows — 25.8% — still rate the wrong move higher after searching deeper than their judge.**
Median judged loss 196 cp, median engine gap −39 cp in the wrong direction; 150 opening, 120
middlegame, 40 endgame, both builds. That is the population with a genuine evaluation defect, held
apart from the horizon cases rather than inferred. Cost: 363 s and 383 s at `--jobs 12`, ~3.2
searches/s aggregate — six extra plies cost 19× per row.

What this does **not** support: the engine's gap stays far below the oracle's even at +6 (median 14
cp against 211), which looks like a scale under-reporting by an order of magnitude. Centipawns are
not comparable across engines, so that ratio conflates under-reporting with different units and is
not quotable. The class split is a within-engine comparison and does not have that problem. The
reading this supports is under [T5](../Docs/MoveQuality.md#findings-1).

The engine-level readings from this run — that the self-reported rate understates the real one
by 13× to 48×, that the profile is monotone and points the wrong way, that the blunder rates are
the figures to quote, and that agreement measures narrowness — are properties of the instrument
and the engine rather than of this run, and live under
[Tier 2 findings](../Docs/MoveQuality.md#findings-1).
