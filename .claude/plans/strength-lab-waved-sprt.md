# Waved SPRT for daytime Full strength-lab runs — Design

**Issue:** #963 (evidence: spike #962, scripts in `StratChessSupport\Spikes\962-lab-sprt\`)

## Goal

A daytime Full lab run plays 26,568 games for about 4 h, whatever the change is worth. The lab is
serialized repository-wide, so those hours are the lab's throughput. Spike #962 replayed 62 archived
runs and simulated the historical pentanomial distribution. Playing the batch in 4 waves, with a
pentanomial SPRT [0, 3] checked between waves, cut wall time by about 29%. It reached the same final
decision on all 62 runs, and its pass probability matches today's `Elo − err > 0` rule at every true
Elo from −15 to +15. The savings come from clear gains and losses, which stop after about 1 h. This
document designs that run inside GitHub Actions. Actions cannot report mid-job, so each wave is a
matrix of jobs followed by a job that decides.

Settled by the owner, not reopened here: SPRT logistic-Elo bounds [0, 3], α = β = 0.05, the
`replay.py` LLR, W = 4 equal waves, cap = Full (26,568 games), fallback `Elo − err > 0` at the cap.
Night runs stay fixed-size, and multi-arm screens are out of scope.

## Review focus

- **D2: fixed-size runs become the one-wave case of the same path.** This puts Night, Normal and
  screen runs through new orchestration they don't need, in exchange for a single repair and verify
  path. Their games, pooling, report and artifact names stay as they are today (I5). Is the blast
  radius worth avoiding two paths?
- **A1: a called workflow whose match job failed still hands its decide outputs to the caller.**
  Repaired waves depend on it. If it is false, D1's reusable workflow must become unrolled jobs and
  the document goes back to review.
- **D8: repair blocks the next wave and does not speculate, and the lost-shard limit counts per
  run.** Check the slot argument and the loss-rate arithmetic.

## Scope

**This change will:**

- Add a `sprt` dispatch input that plays a Full run as 4 waves. After each wave, a pentanomial SPRT
  stops the run with H1 or H0, or continues it. A run that reaches the cap falls back to
  `Elo − err > 0`.
- Move a wave's match matrix, repair and verification into a reusable workflow that `strength.yml`
  calls once per wave. Fixed-size runs call it once.
- Adapt `plan_repair.py` from whole shards to wave slices, with the lost-shard limit counted per run.
- Report every wave's LLR, the stop reason and the planned games, and keep a planned stop apart from
  a discarded or repaired wave.
- Make the SPRT decision plus the cap fallback the ship rule for Full runs, with ledger rules for a
  stopped run, in `measure-strength`, `Docs/CI.md` and `Measurements/README.md`.

**This change will not:**

- Change Night runs (36,000 games), Normal runs or multi-arm screens. They stay fixed-size, with no
  SPRT and no verdict line.
- Correct the Elo of an early-stopped run for stopping bias (D9).
- Start the next wave while a wave is being repaired (D8).
- Repair across attempts, or make `gh run rerun --failed` a supported remedy for a waved run (A6).
- Change fastchess settings, the book, pooling or the error-bar formula.

## Decisions

### D1: One reusable workflow per wave, called up to four times

`strength.yml` keeps `setup`, `build` and `aggregate`. Its shard work moves to a new
`.github/workflows/strength-wave.yml` (`on: workflow_call`), which holds a single wave's `match`
matrix, `repair-plan`, `repair` and a new `decide` job. `strength.yml` calls it as jobs `wave-1` to
`wave-4`. Each `wave-k` for k ≥ 2 needs `wave-(k-1)` and runs only on
`!cancelled() && needs.wave-(k-1).outputs.decision == 'continue'`. The guard needs `!cancelled()`
because a repaired wave's match job failed, and that fails the called workflow (A1).

Rejected:

- **Unrolled jobs**: 16 jobs in `strength.yml` that differ only in their wave number, and that can
  drift apart.
- **One dispatched run per wave**: it splits a batch across run ids. GitHub also keeps only one
  pending run in the `strength-lab` concurrency group, so a chained dispatch can cancel a queued
  run.
- **A long job that polls the shards**: Actions has no way for a running matrix to report.

`.gitignore` already allows `/.github/workflows/*.yml`.

### D2: A fixed-size run is the one-wave case

`setup` computes `waves`: 4 when `sprt` is true, otherwise 1. A one-wave run calls only `wave-1`,
whose single slice per shard is the whole shard. Its `decide` emits `fixed` and runs no SPRT, so
waves 2 to 4 are skipped. The wave artifact keeps today's name when W = 1 (D6), so
`Sync-LabPgn.ps1`, `pool_repaired_batch.py` and the analysis scripts see what they see today.

Rejected: keeping today's `match`/`repair-plan`/`repair` jobs beside the wave path. That means two
repair planners, two discard rules and two sets of fixtures to keep in step.

### D3: The wave plan is fixed in `setup`, before any game

`sprt.py plan` cuts each shard's R pairs at `cut_k = ceil(k·R/W)`, the cut `replay.py` used. Wave k
of shard i is the slice `{shard: i, start: offset + i·R + cut_(k-1) + 1, pairs: cut_k − cut_(k-1)}`.
For Full (R = 738) that gives 185, 184, 185 and 184 pairs, or cumulatively 6,660, 13,284, 19,944
and 26,568 games. `setup` outputs the plan as `wave_plan`, a JSON list of W lists of slices, and
each `wave-k` receives its own list as the `slices` input. Every slice is its own job, played with
`order=sequential start=… -rounds pairs`. So "the first k/4 of every shard in planned order" holds by
construction: game speed cannot choose which pairs a decision sees. The book check and the report's
`Openings` row reserve the cap's whole range, even when the run stops early. A later run's
`opening_offset` skips that whole range.

### D4: `decide` closes each wave

`decide` runs `if: always()` inside the wave workflow, in this order:

1. **Refuse a partial wave.** Today's aggregate rule, moved here unchanged: the wave is complete only
   if every `match` job succeeded, or if `match` failed on attempt 1 and `repair-plan` said `repair`
   and `repair` succeeded. Otherwise it outputs `status=discarded` with the reason and stops. The
   reason is the existing one: the surviving shards are a biased subset, and a sequential test would
   let that bias choose an early stop.
2. **Assemble replayed slices** into the wave's canonical slice artifacts (D8), and upload them.
3. **Verify** every slice of waves 1..k alone, against its planned `start` and `pairs` (D6).
4. **Pool** the slices' counts of waves 1..k across all shards, and decide with `sprt.py decide`
   (D5). A fixed run outputs `fixed`.

The wave workflow's inputs are `wave`, `waves`, `slices` (JSON), `prior_lost` and `sprt`, plus the
settings `match` passes to `play_shard.sh` today: time controls, concurrency, threads, both option
strings, `shard_arms`, `cand_name` and `ref_name`. Its outputs are its contract with `strength.yml`:

| Output | Values |
|---|---|
| `status` | `complete` or `discarded` |
| `reason` | why the wave was discarded; empty when it is complete |
| `decision` | `continue`, `H1`, `H0`, `cap` or `fixed`; empty when discarded |
| `llr` | the cumulative LLR to two decimals; empty for `fixed` |
| `counts` | the cumulative pooled Ptnml as JSON, e.g. `[812,1650,2861,1680,647]` |
| `lost_total` | shards lost so far in the run, this wave included (input `prior_lost` + this wave) |
| `replayed` | today's `Replayed` text, prefixed `wave k: `; empty when nothing was replayed |

### D5: The statistic and the stopping rule

`.github/scripts/sprt.py` holds the statistics, and nothing else carries them:

- **LLR:** `replay.py`'s normal approximation, `LLR = N·(s1 − s0)·(2m − s0 − s1) / (2v)`. Here N is
  the number of pairs, m the mean per-game score of the pairs (Ptnml categories score 0, ¼, ½, ¾ and
  1), v the population variance of those per-pair scores, s0 = 0.5 and s1 = 1/(1 + 10^(−3/400)).
  When v = 0 the LLR is 0, which never crosses a bound.
- **Bounds:** lower `ln(β/(1−α))` = −2.944, upper `ln((1−β)/α)` = +2.944. Both are inclusive: LLR
  ≥ +2.944 is `H1` and LLR ≤ −2.944 is `H0`.
- **Decision at wave k:** a crossing gives `H1` or `H0`. Otherwise the result is `continue` for
  k < W and `cap` for k = W.
- **Run verdict:** `H1` passes and `H0` does not. `cap` passes only if `Elo − err > 0` on the full
  batch, pooled by `pool_pentanomial.pool()`. At Full, an H1 crossing needs about +3.8 Elo and an H0
  crossing about −0.8, against the fallback's +3.0, so the two rules cannot disagree at the cap.
  Checking for a crossing first is safe.
- **Guard in `setup`:** `sprt=true` requires `games=26568` and `shards=18`, the cap the simulation
  validated. `calibration=true` lifts this guard, so a small null run can exercise all four waves
  (slice 3). `sprt` with `candidate_arms` is refused. Recent Full ledger rows were dispatched at
  26,640 games (18 × 740). The cap is the documented 26,568 that the simulation used.

### D6: Artifacts, joining and aggregation

| Artifact | Written by | Holds |
|---|---|---|
| `strength-<run>-wave-<k>-shard-<N>` (W > 1) / `strength-<run>-shard-<N>` (W = 1) | `match`, or `decide` for a replayed slice | one wave slice: PGN, log, `cpu.tsv` |
| `strength-<run>-wave-<k>-replay-<N>-<i>` | `repair` | one repair piece |
| `strength-<run>-shard-<N>` (W > 1) | `aggregate`, after the pool succeeds | shard N's slices of waves 1..k joined, rounds renumbered 1..cut_k |
| `strength-<run>-repair` | `aggregate`, last | marker plus the repair plan of every repaired wave, as today |

`sprt.py slice_artifact(run, wave, waves, shard)` names a slice. The workflow's upload expression
must agree with it, and a fixture checks that it does.

Today's join code (`plan_repair.verified_counts` and the body of `assemble`) moves to a new
`.github/scripts/batch_slices.py`, so repair and wave aggregation share one copy:

- `verify_slice(directory, start, pairs, run, names, book) -> counts` checks one slice alone, using
  the existing single-shard call to `verify_batch`.
- `join_slices(pieces, target)` concatenates verified contiguous pieces, renumbers `[Round]`, writes
  the summed Ptnml as the log's last line and drops `cpu.tsv`.

`plan_arms.py verify` gains `--pairs-per-shard P`, the number of pairs played, which defaults to
`--rounds-per-shard`. A stopped run's joined shards then verify with start spacing R and P = cut_k.

`aggregate` reads every wave's result and outputs at once, `NEEDS: ${{ toJSON(needs) }}`, through
`sprt.py summarize`. The summary is one of these:

- **Discarded:** a wave reported `discarded`, or a called wave ended without outputs, for example
  cancelled.
- **Fixed.**
- **A verdict:** decision, deciding wave k, counts and replayed rows.

For W > 1 it then joins waves 1..k, verifies them, pools them with `--expect-pairs-per-shard cut_k`
and uploads the joined shards. It **refuses unless its pooled Ptnml equals the deciding wave's
`counts`**, so the figure reported is the one the decision was taken on. The CPU summary reads the
wave-slice directories, since each slice ran on its own runner.

### D7: The report

The heading names the outcome. The run's colour still doesn't (I6).

| Outcome | Heading |
|---|---|
| H1 at wave k | `## Strength lab — passes (SPRT H1, wave k of 4)` |
| H0 at wave k | `## Strength lab — does not pass (SPRT H0, wave k of 4)` |
| cap | `## Strength lab — passes at the cap (Elo − err > 0)` or `— does not pass at the cap` |
| discarded | `## Strength lab — DISCARDED` and `Wave k: <reason>.` |
| fixed | `## Strength lab`, unchanged |

A waved report adds three things:

- The row `| Games | 13,284 of 26,568 planned = 18 shards × 2 of 4 waves, stopped by SPRT H1 |`.
- An SPRT block with the line
  `SPRT [0, 3], α = β = 0.05: H1 at LLR ≥ +2.94, H0 at ≤ −2.94, cap 26,568 games`.
- A table `| Wave | Games | Elo ± err | LLR | Decision |` with one row per wave played, each row
  pooled cumulatively.

A planned stop is skipped waves after a decision. A lost shard is a `Replayed` row naming its wave.
A discard has no figures at all, so a reader never sees the LLR of a batch that was not decided. All
three come from `decide` outputs, never from which artifacts exist: a missing artifact could mean
any of them.

### D8: Repair across waves

- **Per wave, in the same order as today:** wave-k `match` → `repair-plan` → `repair` → `decide`.
  `plan_repair.py plan` takes:
  - `--slices`, the wave's planned slices, in place of `--shards`, `--rounds` and
    `--opening-offset`. A lost shard's range is its planned slice, which `plan_slices` splits into at
    most S / lost pieces.
  - `--job-prefix "wave-<k> / "`, so `JOB_RE` matches this wave's jobs only (A2).
  - `--wave k`, for the artifact names.
  - `--prior-lost N`.

  `assemble` writes `slice_artifact(run, k, W, N)` and checks that the pieces cover the planned
  slice. A wave slice is about 185 pairs, so 18 pieces of about 10 pairs replay in about 5–10
  minutes.
- **Block; do not speculate.** Wave k+1 starts only after wave k's `decide`, including the ~45 min
  GitHub takes to declare a runner lost. Speculation can't save that time. A wave holds 18 of the 20
  job slots, so a speculative wave k+1 would leave the repair 2 slots. The repair pieces would queue
  behind it, and the lost runner still has to be declared first. The cost of blocking is small: at
  ~1 loss per 600 shard jobs, 3% (per runner-hour) to 12% (per job) of waved runs lose a shard, for
  about 55 min each, an expected 2–7 min a run against the ~70 min a run saves.
- **The limit of 2 lost shards counts per run**, cumulatively across waves (`prior_lost`). The limit
  is an alarm that the run's infrastructure is wrong. If losses come per runner-hour, a waved run
  sees the same number as today. If they come per job, three losses in 72 jobs happen about
  C(72,3)/600³ ≈ 0.03% of the time. Either way a per-run limit costs nothing, while a per-wave
  limit would allow 8 losses before the alarm.
- **Only attempt 1 repairs**, as today. A waved run discarded by `decide` is dispatched again, at
  the same offset. The discard report shows no figures, so the re-run is not chosen by a peek.

### D9: Recording a stopped run in the ledger

`Measurements/README.md` already rules on this: an SPRT that crossed a bound gives a decision, not a
size. The lab rows follow that rule:

- **Games** is the number of games played at the stop.
- **Elo** is the pooled figure at the stop, marked `(at stop)`, for example
  `**+9.52 +/- 4.27** (at stop)`. It is the only figure the run has. On the 33 real early stops,
  stopping bias averaged −0.02, but near the bounds it reads high: a true +3 stopped early reads
  about +6.
- **Verdict:**
  - H1 is `gain`, which the vocabulary already covers ("an accepted H1"), quoted as "worth more
    than +3".
  - H0 is a new vocabulary entry, `no gain`: an accepted H0 on the lab's SPRT [0, 3], worth less than
    +3, with the sign unresolved.
  - The cap gives today's verdicts from the full-batch interval.

A magnitude that matters, such as an anchor step or a choice between two candidates, needs a
fixed-size run.

Rejected: a stopping-bias-corrected estimator, as fishtest uses. It is a new statistic to
implement and validate, for rows that are never summed.

### D10: Who sets `sprt`

`sprt` is a boolean input with default `false`. A Full dispatch that omits it runs today's
fixed-size batch, which is harmless. Slice 4 makes the procedure explicit: `measure-strength`'s Full
row dispatches with `-f games=26568 -f sprt=true`, and its ship rule becomes D5's run verdict. The
input ships before the docs adopt it, so the end-to-end checks in slice 3 can run first.

### D11: Self-tests

`setup` and the PR gate's Linux Release self-test list run `sprt.py --self-test`, next to
`pool_pentanomial.py`'s. It covers:

- **LLR golden values.** Three cumulative wave counts from archived runs, one H1 at wave 1, one H0
  and one cap, with the LLR that `replay.py`'s `llr()` gives each. Run ids sit in the comments, as in
  `SELF_TEST_CASES`.
- **The bounds.** Exact values, inclusivity at ±2.944, v = 0, both directions of the cap fallback,
  and `continue` before W.
- **The plan.** Cuts for (R, W) = (738, 4), (900, 1) and (7, 4). For several (S, R, offset, W),
  every shard's slices must tile its range contiguously and disjointly.
- **`summarize`** over fixture `needs` JSON for every outcome.
- **`slice_artifact`** at W = 1 and W = 4.

`test_strength_workflow.py` reads both workflow files and adds:

- The wave gate (`!cancelled() && decision == 'continue'`) and the moved partial-wave rule,
  evaluated over outcome tables as `discard_rule()` does today.
- Aggregate fixtures, run through the real Bash blocks:
  - H1 at wave 1
  - H0 at wave 2
  - the cap, both pass and fail
  - discard at wave 3
  - a repaired wave 2 and then H1 at wave 3
  - a counts mismatch, which is refused
  - a fixed W = 1 run, whose report keeps today's form

## Assumptions I cannot verify from the code

- **A1: a failed called workflow still passes on its outputs.** When a called workflow's `match`
  job fails but its `decide` job succeeds, the caller still gets `decide`'s outputs, and a
  `!cancelled()` guard on the next call still runs. Not verified. Slice 2 settles it first, with a
  throwaway draft-PR probe (`on: pull_request`): a called workflow with one failing job and one job
  that emits an output, and a caller that prints the output and gates on it. If it fails, D1 unrolls
  and this document goes back to review.
- **A2: called jobs appear in the run's job list.** The jobs API (`filter=latest`) lists them as
  `<caller job name> / <job name>`, for example `wave-1 / match (3)`, with the check-run annotations
  `plan_repair.py` reads. Not verified. The same probe settles it.
- **A3: a wave slice plays at the pace it would have inside a continuous shard.** This is the
  spike's stated limit, since the transposition table is lost only between games, which happens
  anyway. Not verified. It is read on the first waved Full runs, comparing each wave's slowest-slice
  duration with the spike's per-slice durations (slice 4).
- **A4: wave boundaries cost about a minute.** The spike measured jobs starting 3 s after creation
  on one run. At a boundary all 20 slots are briefly free, so a PR's CI can take some of them and
  delay the next wave by up to one CI job's length. Not verified. It is read from job start times on
  the first waved runs (slice 4). It changes the savings estimate but no decision here.
- **A5: SPRT accuracy carries over to new changes.** The 29% and the pass probability come from the
  historical mix, which the eval epic made gain-heavy. A period of near-zero changes saves less:
  3.89 h against 4.07 h at a true 0. The owner accepted this in #962, and it is not verified
  further.
- **A6: `gh run rerun --failed` on a waved run.** It would re-run the failed wave and everything
  after it. Not verified, so the docs keep it as a remedy for one-wave runs only, and D8 re-dispatches
  a waved run.

## Invariants

- **I1:** The plan is fixed in `setup`. The slices of each shard tile
  `[offset + i·R + 1, offset + (i+1)·R]` in wave order, and no result can change which pairs a wave
  plays.
- **I2:** No decision pools a partial wave. A wave is complete only under today's rule: every shard
  succeeded, or runner-lost shards were repaired on attempt 1.
- **I3:** A decision at wave k uses every slice of waves 1..k, each one verified against its planned
  start and pairs.
- **I4:** The reported figure is the one the decision was taken on. The aggregate's pooled Ptnml
  equals the deciding wave's `counts`.
- **I5:** A one-wave run plays the same games, pools them the same way, writes the same report and
  uploads the same artifact names as today.
- **I6:** A planned stop, a discard and a repair are told apart by `decide` outputs, never by which
  artifacts exist. A planned stop with no repair is a green run. The heading carries the verdict.
- **I7:** At most 2 lost shards are repaired per run, counting every wave.
- **I8:** The statistic is `replay.py`'s, and the golden values pin it.

## Validation

Tier: lab harness only. No engine code changes, so no Elo match and no equivalence gate is needed.
The evidence:

| Risk | Closed by |
|---|---|
| Wrong LLR, bounds, cuts or verdict (I8, D5) | `sprt.py --self-test` golden values and tables, in `setup` and the PR gate |
| Gate or discard expression wrong (I2, I6) | expression tables in `test_strength_workflow.py` |
| Join, verify or pool of a stopped prefix wrong (I3, I4) | aggregate fixtures for every outcome, including the counts mismatch |
| Fixed runs changed (I5) | W = 1 fixtures, plus the slice 2 smoke run's artifact names and report against today's |
| A1, A2 | the slice 2 probe, before any orchestration code |
| End-to-end waves | slice 3: a calibration null smoke run at `sprt=true games=144 shards=2`, which runs all four waves to the cap. Optional: a known-sign control at Full (`reference_tc=5+0.1`), expected to stop H1 at wave 1 |
| A3, A4 | slice 4 reads the first waved Full runs' job timings |

## Slices

Each slice starts from this document. What a slice produces and a later one relies on is named
exactly here.

**Slice 1: statistics module.** Add `.github/scripts/sprt.py`, holding the constants
(`ELO0 = 0`, `ELO1 = 3`, `ALPHA = BETA = 0.05`, `WAVES = 4`, `FULL_GAMES = 26568`,
`FULL_SHARDS = 18`) and these functions:

- `wave_cuts(rounds, waves)`
- `wave_plan(shards, rounds, offset, waves)`, which returns D3's slice lists
- `llr(counts)`
- `decide(counts, wave, waves)`, which returns a D4 `decision`
- `verdict(decision, counts)`, which returns `{"passes": bool, "rule": "sprt" | "cap"}`
- `slice_artifact(run, wave, waves, shard)`

It also has CLI subcommands `plan` and `decide` that write `GITHUB_OUTPUT` (`wave_plan`;
`decision`, `llr`, `counts`), plus `--self-test` as D11 describes, except `summarize`. Wire the
self-test into `setup` and the PR gate list. The workflow behaves no differently. Done when the
self-test passes in CI.

**Slice 2: wave plumbing at W = 1.** First, the A1/A2 probe. Then:

- Add `strength-wave.yml`, with D4's inputs and outputs.
- Add `batch_slices.py`.
- Give `plan_repair.py` `--slices`, `--job-prefix`, `--wave` and `--prior-lost`.
- Give `plan_arms.py verify` `--pairs-per-shard`.
- `strength.yml` gets `setup` planning through `sprt.py plan --waves 1` and calls `wave-1` only.
- The W = 1 boundary fixtures.
- `Docs/CI.md` → Strength lab: the job list and artifact names.
- `strength-lab.md` → Repairing a lost shard: per wave.

Every run is fixed-size here. Done when the fixtures pass and a lab smoke run (`games=144 shards=2`)
goes green, with today's artifact names and report.

**Slice 3: SPRT waves.**

- The `sprt` input and D5's guard.
- `wave-2` to `wave-4` with D1's gate and the `prior_lost` chain.
- `decide`'s SPRT step.
- `sprt.py summarize`.
- D6's join, verify, pool and counts check in `aggregate`.
- D7's report.
- D11's remaining fixtures.

Done when the fixtures pass and the calibration null smoke run reaches the cap with four waves in
its table. The optional known-sign control is the owner's call: about 1.1 h on 18 runners.

**Slice 4: adopt it as the ship rule.**

- `measure-strength`: the cost row reads ~1–4 h.
- `strength-lab.md`: the Full row dispatches with `sprt=true`, the ship rule is D5's verdict, and a
  discarded waved run is re-dispatched.
- `Docs/CI.md`: replace "a sequential test does not shard across runners" with the wave rationale
  and the Night-run exception.
- `Measurements/README.md`: `no gain`, `(at stop)` and Games played.
- `Docs/Changelog.md`.

Then read A3 and A4 from the first waved Full runs and record them under the issue.

## Cost

- **Size:** slice 1 is 200–300 lines in 2 files. Slice 2 is over 200 lines in about 8 files: the
  workflow split, `batch_slices.py`, `plan_repair.py`, `plan_arms.py`, fixtures and docs. Slice 3
  is about 200 lines in 4 files. Slice 4 is about 80 lines of docs.
- **Blast radius:** the strength lab and the PR gate's Python self-tests. No engine code. Slice 2
  touches every lab run's plumbing (D2). Slice 4 changes the ship rule.
- **Review:** one code review per slice. Slice 2 is the riskiest diff.
- **Lab time:** the slice 2 smoke run takes about 10 minutes and the slice 3 null smoke run about 15
  minutes, each on 2 runners.
- **Optional parts:**
  - The known-sign Full control, about 1.1 h of the lab. It proves the H1 stop end to end at real
    size, where the fixtures prove it only on fixtures.
  - The I4 counts check, about 10 lines. It covers an aggregate that reports a figure the decision
    never saw.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Wave orchestration, artifact names, decide outputs (D1, D4, D6) | `Docs/CI.md` → Strength lab; header comment of `strength-wave.yml` |
| Why fixed runs are one wave (D2) | `Docs/CI.md` → Strength lab |
| LLR formula, bounds, cap fallback and why they cannot disagree (D5) | `sprt.py` docstring |
| Repair blocks; per-run limit arithmetic (D8) | `plan_repair.py` docstring; `strength-lab.md` → Repairing a lost shard |
| Ship rule and dispatch (D5, D10) | `measure-strength` → `reference/strength-lab.md` |
| Stopped-run ledger rules (D9) | `Measurements/README.md` |
| Replacing "a sequential test does not shard" | `Docs/CI.md` → Two deliberate differences |
| A3/A4 readings | a comment on the issue; `Docs/CI.md` if they change the savings figure |
| Spike evidence (29%, 62/62, pass-probability table) | already in #962; `Docs/Changelog.md` cites it |
