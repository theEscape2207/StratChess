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

The engine-level readings from this run — that the self-reported rate understates the real one
by 13× to 48×, that the profile is monotone and points the wrong way, that the blunder rates are
the figures to quote, and that agreement measures narrowness — are properties of the instrument
and the engine rather than of this run, and live under
[Tier 2 findings](../Docs/MoveQuality.md#findings-1).
