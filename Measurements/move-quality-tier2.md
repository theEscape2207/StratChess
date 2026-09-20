# Move-quality scan — Tier 2

Runs of `Scripts/analyze_external_quality.py`: the same rows as [Tier 1](move-quality-tier1.md),
re-judged by an outside engine instead of by the engine that played them. The instrument — the
loss formula, what `agree%` and `noise` do and do not mean, and the limits specific to an oracle
— is [`../Docs/MoveQuality.md`](../Docs/MoveQuality.md). This file holds only what each run
measured.

**These are profile tables, not verdict rows**, and they are not comparable against Tier 1's:
the judge differs, which is the whole point of the tier. See the carve-out in
[`README.md`](README.md).

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

The engine-level readings from this run — that the self-reported rate understates the real one
by 13× to 48×, that the profile is monotone and points the wrong way, that the blunder rates are
the figures to quote, and that agreement measures narrowness — are properties of the instrument
and the engine rather than of this run, and live under
[Tier 2 findings](../Docs/MoveQuality.md#findings-1).
