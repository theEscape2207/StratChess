# Linux CI — instrument calibration

Produced by `.github/workflows/strength.yml`. **Nothing here is a strength measurement of
anything** — these rows measure the instrument, which is why they sit apart from the two ledgers
that carry results.

**Calibrated 2026-08-05.** Both required runs passed, so results from this instrument may now be
recorded and read. Why its rows are never comparable with the local ledger: [`README.md`](README.md).

| Date | Candidate | Reference | Games | TC | Elo +/- err | Verdict |
|---|---|---|---|---|---|---|
| 2026-08-05 | c2a9f78 | c2a9f78 (same commit) | 200 | 10+0.1 vs **5+0.1** | 75.88 +/- 42.56 | calibration |
| 2026-08-05 | c2a9f78 | c2a9f78 (same commit) | 1000 | 10+0.1 | -3.47 +/- 18.21 | calibration |
| 2026-08-06 | 0e0fc94 | 0e0fc94 (same commit) | 20000 | 10+0.1 | **-2.17 +/- 4.18** | calibration |
| 2026-08-06 | c52d1a6 | c52d1a6 (same commit) | 19980 | 10+0.1 | **-1.51 +/- 4.15** | calibration |

## Row detail

Same order as the table above. A row with nothing to add beyond its verdict has no section here.

### 2026-08-05 — c2a9f78 (200 games)

**Known-sign control — PASS.** 100W/57L/43D (60.75%), LOS 99.99%, DrawRatio 31.00%, PairsRatio 2.29, Ptnml(0-2) [7, 14, 31, 25, 23]. Same binary both sides; the reference gets half the base time and the same increment. The interval [+33, +118] excludes zero, so the harness demonstrably detects a real strength difference — which is all this run claims. It does **not** measure what a 2x time handicap is worth, since there is no independent expectation to compare against. The base is halved and the increment left alone deliberately: at 5+0.05 the engine forfeits on `compute_budget()`'s 100 ms floor (#204), which would test the time manager rather than the harness. **Zero time losses**

### 2026-08-05 — c2a9f78 (1000 games)

**Null test — PASS.** 388W/398L/214D (49.50%), nElo -4.11 +/- 21.53, Ptnml(0-2) [71, 83, 197, 83, 66], 162 min at concurrency 3. Identical commit and identical time control on both sides, so the true difference is zero by construction; the interval [-21.7, +14.7] contains it comfortably. Zero measurable bias in the CI instrument, matching what the local instrument showed at the same game count (-1.4 Elo pooled, 2026-07-03). **Zero time losses**, which also establishes that a shared 4-vCPU runner holds 10+0.1 at concurrency 3 — the open question #204 raised

### 2026-08-06 — 0e0fc94 (20000 games)

**Sharded null test — PASS. Calibrates the 20-way instrument; carries no strength information.** 20 shards x 500 pairs, pooled Ptnml(0-2) [1495, 1665, 3754, 1642, 1444], score 49.69%, 2 h 47 min wall-clock. Identical commit and time control on both sides, so the true difference is zero by construction and [-6.35, +2.01] contains it. **Agrees with the single-job row above** (-3.47 +/- 18.21), which is the check that matters: sharding did not introduce bias. Resolution **±4.18** against ±18 single-job and ±26 locally — the first interval this project has had that can resolve a single-digit eval term. Shard slices verified disjoint (20 opening positions, 20 unique). Error bar pooled **pentanomially over pairs**, not per-game; see `.github/scripts/pool_pentanomial.py`. **Zero time losses** across all 20 shards, at concurrency 3 per shard

### 2026-08-06 — c52d1a6 (19980 games)

**Shard-count experiment (#217 Experiment A) — PASS. Measures the instrument, not the engine.** 18 shards x 555 pairs, pooled Ptnml(0-2) [1461, 1649, 3828, 1620, 1432], score 49.78%, 3 h 04 min wall-clock. Run to decide whether dropping from 20 shards to 18 costs anything, since 20 consumes the entire 20-job concurrency allowance and blocks every other PR's required check for the duration. **The interval is the result: ±4.15 against the 20-shard row's ±4.18.** At ~10,000 pairs a spread estimate is itself known to about ±0.03, so a 0.03 difference is one standard error — the two are indistinguishable, and the split costs no resolution. Point estimate contains the guaranteed zero, [-5.66, +2.64]. **Cost is 17 minutes** (3 h 04 against 2 h 47), matching the estimate in #217. **Zero time losses** across all 18 shards at concurrency 3. Shard slices verified disjoint (18 opening positions, 18 unique). Comparable with the row above: shard count changes how many runners are used, not the CPU each engine gets, so the effective time control is unchanged — unlike a change to per-shard concurrency, which would not be comparable. Measured while an unrelated Build-tier PR ran on the two freed slots: its first job started 5 s after dispatch and the run finished in 10 min against a 4.5-5 min uncontended baseline, i.e. a delay rather than a block
