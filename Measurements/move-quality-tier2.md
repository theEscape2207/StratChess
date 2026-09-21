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

The engine-level readings from this run — that the self-reported rate understates the real one
by 13× to 48×, that the profile is monotone and points the wrong way, that the blunder rates are
the figures to quote, and that agreement measures narrowness — are properties of the instrument
and the engine rather than of this run, and live under
[Tier 2 findings](../Docs/MoveQuality.md#findings-1).
