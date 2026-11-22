# UCI_Elo calibration against Stockfish 19

What each `UCI_LimitStrength` level scores against Stockfish 19 at its own `UCI_LimitStrength`. The
instrument is `Scripts/Run-UciEloCalibration.ps1` (the gauntlet) and
`Scripts/fit_uci_elo_calibration.py` (the fit); the table in `StratEngine/StrengthLimit.cpp` takes
its anchors from the run it names. Like the move-quality ledgers, this file carries a table per run
and no `Verdict`: a calibration is a scale, not a decision about a change.

A label reads "scores 50% against SF19 set to this `UCI_Elo`, both at the run's TC on the run's
machine": parity with weakened SF19, not a CCRL rating. SF19's `UCI_Elo` was fitted in 2023 at
120+1 and is not refitted per release; at a shorter TC it likely plays below its label. Intervals
are match sampling only and do not cover that drift.

## 2026-11-22 -- candidate 3af9bbe2 (#985) (2100 games)

| Level | `UCI_Elo` set | Played | Pick depth | SF `UCI_Elo` (stage 1/2) | W-D-L | Score | **Elo** | 95% CI | Next - this, 95% CI | Depth median (p10-p90) | Reached pick depth |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 1320 | 0.0000 | 1 | 1320/1320 | 25-1-274 | 8.5% | **907** | [826, 971] | [+115, +286] | 1 (1-1) | 100% |
| 2 | 1455 | 2.0039 | 3 | 1455/1320 | 60-1-239 | 20.2% | **1103** | [1049, 1149] | [+153, +274] | 3 (3-3) | 100% |
| 4 | 1590 | 4.0078 | 5 | 1590/1320 | 126-5-169 | 42.8% | **1315** | [1271, 1357] | [+167, +276] | 5 (5-5) | 100% |
| 7 | 1792 | 7.0063 | 8 | 1792/1430 | 164-5-131 | 55.5% | **1535** | [1498, 1574] | [+172, +290] | 8 (8-8) | 100% |
| 10 | 1994 | 10.0047 | 11 | 1994/1820 | 111-7-182 | 38.2% | **1767** | [1726, 1808] | [+307, +415] | 11 (11-11) | 98% |
| 14 | 2264 | 14.0125 | 15 | 2264/2150 | 121-20-159 | 43.7% | **2127** | [2090, 2165] | [+175, +281] | 12 (11-15) | 15%, clock-bound |
| 19 | 2600 | 19.0000 | 20 | 2600/2410 | 96-34-170 | 37.7% | **2355** | [2317, 2391] | - | 13 (11-15) | 2%, clock-bound |

### Row detail

Conditions: 60+0.6 on both sides, `Threads=1`, `Hash=64`, StratChess at the default `MultiPV`;
concurrency 8 on the 12-core dev machine (POMELO24); fastchess v1.8.2-alpha with Run-EloMatch's
adjudication; `openings-large.pgn` sequential, colour-swapped pairs. Candidate
`build/windows-clang-cl/StratChessEvolved.exe` at 3af9bbe2 (sha256 551353954549…), engine source
identical to the merged option slice (PR #992); Stockfish `sf_19`. Each level reached through
`UCI_Elo = ceil(provisional inverse(L))` under the old 1320..2600 linear table. Stage 1: 60 games a
level against SF at the level's own label (openings 1-30, 108 min); stage 2: 240 games against SF at
the stage-1 estimate rounded to 10, clamped to SF's 1320 floor (openings 31-150, 397 min). Smoke
attempt before stage 1: 20 games at level 0, every StratChess move at depth 1, so the options
reached the engine. No attempt had a time loss, illegal move, disconnect or stall; none discarded.

Fit: per-level fractional-Bernoulli MLE over both stages, SF's `UCI_Elo` taken as exact; 95%
percentile intervals from 10 000 opening-pair resamples (seed 985), stratified by stage and shared
across levels, so the difference intervals carry the shared openings. No pooling: every adjacent
difference excludes zero, so the table is the rounded estimates with the levels played.

The floor is extrapolated. Level 0 scored 8.5% against SF's 1320 floor in both stages, below the
plan's 10% stop, and levels 0 and 2 miss the ±50 precision gate (their stage-2 opponent was clamped
at 1320, so the score sat far from 50%). The owner accepted both on 2026-11-22 (`--accept-floor`);
extending the scale below level 0 stays a separate decision.

Levels 14 and 19 are clock-bound at 60+0.6 here: median depth 12-13 against pick depths 15 and 20.
Between them the median depth rises only from 12 to 13, so their +175..+281 gap comes mainly from the
pick rather than the search, and a faster or slower clock or machine moves the top of the scale. Level 19 at 2355 is the scale's top, not full
strength: the 2026-11-20 scouting bracketed uncapped play at about 2600-2700 on this scale.
