# Waved SPRT for daytime Full strength-lab runs — Design

**Issue:** #963 (evidence: spike #962, scripts in `StratChessSupport\Spikes\962-lab-sprt\`)

## Goal

A daytime Full lab run plays 26,568 games for about 4 h, whatever the change is worth. The lab is
serialized repository-wide, so those hours are the lab's throughput. Spike #962 replayed a 4-wave
pentanomial SPRT [0, 3] on 62 archived valid single-candidate runs of at least 9,990 pairs, Night-sized
runs included. That historical replay cut wall time by an estimated 29% and reached the same final
decision on all 62. A simulation of 26,568-game runs, played as four equal waves of 3,321 pairs,
gave pass probabilities close to today's `Elo − err > 0` rule at the 11 true Elo values it sampled
from −15 to +15: 0.475 against 0.480 at +3. It did not sample +5. The savings come from clear gains
and losses, which stop after about 1 h. This
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
waves 2 to 4 are skipped. At W = 1 everything a reader or tool consumes keeps today's form (D6, I5):
canonical `strength-<run>-shard-<N>` artifacts, raw `strength-<run>-replay-<N>-<i>` pieces, the
`Replayed` text, the repair-plan payload in the marker, and multi-arm verification and pooling. So
`Sync-LabPgn.ps1`, `pool_repaired_batch.py` and the analysis scripts see what they see today, and
the manual repair still finds the surviving shards it needs.

Rejected: keeping today's `match`/`repair-plan`/`repair` jobs beside the wave path. That means two
repair planners, two discard rules and two sets of fixtures to keep in step.

### D3: The wave plan is fixed in `setup`, before any game

`sprt.py plan` cuts each shard's R pairs at `cut_k = ceil(k·R/W)`, the cut `replay.py` used. Wave k
of shard i is the slice `{shard: i, start: offset + i·R + cut_(k-1) + 1, pairs: cut_k − cut_(k-1)}`.
For Full (R = 738) that gives 185, 184, 185 and 184 pairs, or cumulatively 6,660, 13,284, 19,944
and 26,568 games. `setup` outputs the plan as `wave_plan`, a JSON list of W lists of slices. Each
`wave-k` receives the whole plan and its own wave number, so it can read earlier waves at their
planned sizes. Every slice is its own job, played with
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
3. **Verify wave k at batch level** with `plan_arms.py verify`, which keeps today's checks: arm
   routing, distinct starts and complete rounds. All shards play the same number of pairs within a
   wave, so the call is the existing one with `--opening-offset offset + cut_(k-1)`,
   `--rounds-per-shard R` (start spacing), `--pairs-per-shard cut_k − cut_(k-1)` (D6), `--arms` and
   the book from the `strength-<run>-comparison` artifact. Each earlier wave was verified by its own
   `decide`, and artifacts don't change within a run, so waves 1..k−1 are not verified again here.
4. **Pool** the counts of waves 1..k across all shards. Each slice log is read with its planned pair
   count as `expected_pairs`, so a short or excess log is refused. Then decide with `sprt.py decide`
   (D5). A fixed run outputs `fixed`.

The wave workflow's inputs are `wave`, `waves`, `wave_plan` (the whole JSON plan), `prior_lost`,
`sprt`, `book_file` and `opening_offset`, plus the settings `match` passes to `play_shard.sh` today:
time controls, concurrency, threads, both option strings, `candidate_arms`, `shard_arms`, `cand_name`
and `ref_name`. `BOOK_FILE` is a workflow-level `env` in `strength.yml` today and does not reach a
called workflow, so it becomes an input. Each calling `wave-k` job:

- needs `setup` and `build` directly, besides `wave-(k-1)`, because it reads their outputs;
- grants `actions: read`, `checks: read` and `contents: read`, the most the called `repair-plan` job
  can then hold.

The outputs are the wave workflow's contract with `strength.yml`:

| Output | Values |
|---|---|
| `status` | `complete` or `discarded` |
| `reason` | why the wave was discarded; empty when it is complete |
| `decision` | `continue`, `H1`, `H0`, `cap` or `fixed`; empty when discarded |
| `llr` | the cumulative LLR to two decimals; empty for `fixed` |
| `counts` | the cumulative pooled Ptnml as JSON, e.g. `[812,1650,2861,1680,647]` |
| `lost_total` | shards lost so far in the run, this wave included (input `prior_lost` + this wave) |
| `replayed` | today's `Replayed` text, prefixed `wave k: ` only when W > 1; empty when nothing was replayed |
| `lost_a`, `lost_b`, `slices` | today's repair-plan outputs for this wave, which `aggregate` needs for the marker payload |

### D5: The statistic and the stopping rule

`.github/scripts/sprt.py` holds the statistics, and nothing else carries them:

- **LLR:** `replay.py`'s normal approximation, `LLR = N·(s1 − s0)·(2m − s0 − s1) / (2v)`. Here N is
  the number of pairs, m the mean per-game score of the pairs (Ptnml categories score 0, ¼, ½, ¾ and
  1), v the population variance of those per-pair scores, s0 = 0.5 and s1 = 1/(1 + 10^(−3/400)).
  When v = 0 the LLR is 0, which never crosses a bound.
- **Bounds:** lower `ln(β/(1−α))` and upper `ln((1−β)/α)`, computed rather than written as
  literals. They are ±ln 19 = ±2.944438979…, shown as ±2.94. Both are inclusive: LLR ≥ upper is
  `H1` and LLR ≤ lower is `H0`.
- **Decision at wave k:** a crossing gives `H1` or `H0`. Otherwise the result is `continue` for
  k < W and `cap` for k = W.
- **Run verdict:** `H1` passes and `H0` does not. `cap` passes only if `Elo − err > 0` on the full
  batch, pooled by `pool_pentanomial.pool()`. At Full, an H1 crossing needs about +3.8 Elo and an H0
  crossing about −0.8, against the fallback's +3.0, so the two rules cannot disagree at the cap.
  Checking for a crossing first is safe.
- **Guard in `setup`:** `sprt=true` requires `games=26568` and `shards=18`, the cap the simulation
  validated. `calibration=true` lifts this guard, so a small null run can exercise all four waves
  (slice 3). Even then `sprt` needs R ≥ 4 after rounding, because R ≤ 3 leaves a wave with no
  pairs under `ceil(k·R/4)`. `sprt` with `candidate_arms` is refused. Recent Full ledger rows were dispatched at
  26,640 games (18 × 740). The cap is the documented 26,568 that the simulation used.

### D6: Artifacts, joining and aggregation

| Artifact | Written by | Holds |
|---|---|---|
| `strength-<run>-wave-<k>-shard-<N>` (W > 1) / `strength-<run>-shard-<N>` (W = 1) | `match`, or `decide` for a replayed slice | one wave slice: PGN, log, `cpu.tsv` |
| `strength-<run>-wave-<k>-replay-<N>-<i>` (W > 1) / `strength-<run>-replay-<N>-<i>` (W = 1) | `repair` | one repair piece |
| `strength-<run>-shard-<N>` (W > 1) | `aggregate`, after the pool succeeds | shard N's slices of waves 1..k joined, rounds renumbered 1..cut_k |
| `strength-<run>-repair` | `aggregate`, last | marker plus the repair plan of every repaired wave, as today |

`sprt.py slice_artifact(run, wave, waves, shard)` and `replay_artifact(run, wave, waves, shard,
index)` name these. The workflow's upload expressions must agree with them, and a fixture checks that
they do.

Today's join code (`plan_repair.verified_counts` and the body of `assemble`) moves to a new
`.github/scripts/batch_slices.py`, so repair and wave aggregation share one copy:

- `verify_piece(directory, start, pairs, run, names, book) -> counts` checks one repair piece alone,
  using the existing single-shard call to `verify_batch`. Repair pieces never carry an arm: the
  planner refuses multi-arm runs.
- `join_slices(pieces, target)` concatenates verified contiguous pieces, renumbers `[Round]`, writes
  the summed Ptnml as the log's last line and drops `cpu.tsv`.

`plan_arms.py verify` gains `--pairs-per-shard P`, the number of pairs played, which defaults to
`--rounds-per-shard`. A wave (D4) or a stopped run's joined shards then verify through the
batch-level path, arms included, with start spacing R.

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

**Publication fails closed.** The verdict, the PR comment, the joined uploads and the marker are
published only after all of these succeed: the summary is not discarded, the verify succeeds, the pool
succeeds and the counts match. Any failure fails the job and reports DISCARDED with no figures, even
when every shard job was green. The marker is uploaded last, as today.

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

A calibration run keeps its calibration label in the heading and the ledger, whatever it decides.

A planned stop is skipped waves after a decision. A lost shard is a `Replayed` row naming its wave.
A discard has no figures at all, so a reader never sees the LLR of a batch that was not decided. All
three come from `decide` outputs, never from which artifacts exist: a missing artifact could mean
any of them.

### D8: Repair across waves

- **Per wave, in the same order as today:** wave-k `match` → `repair-plan` → `repair` → `decide`.
  `plan_repair.py plan` takes:
  - `--slices`, this wave's planned slices from `wave_plan`, in place of `--shards`, `--rounds` and
    `--opening-offset`. A lost shard's range is its planned slice, which `plan_slices` splits into at
    most S / lost pieces.
  - `--job-prefix "wave-<k> / "`, so `JOB_RE` matches this wave's jobs only (A2).
  - `--wave k`, for the artifact names.
  - `--prior-lost N`.

  `assemble` writes `slice_artifact(run, k, W, N)` and checks that the pieces cover the planned
  slice. A wave slice is about 185 pairs, so 18 pieces of about 10 pairs should replay in about
  5–10 minutes. That is a forecast, not a measurement.
- **Block; do not speculate.** Wave k+1 starts only after wave k's `decide`, including the ~45 min
  GitHub took to declare a runner lost (observed once). A speculative wave k+1 would take 18 of the
  20 job slots, leaving the repair at most 2, so the repair pieces would queue behind it. It could
  also play openings that a later stop never uses. Speculation could overlap some of the detection
  lag, but not enough to justify a cancel path.

  The interruption is rough and rests on the inherited estimate of 1 loss per 600 shard jobs. If
  losses come per job, a run loses a shard with probability 2.96% over 18 jobs or 11.32% over 72.
  Each loss interrupts the run for about 55 min, for an expected total of roughly 2–7 min a run.
  That is a total interruption, not a cost added over today's fixed run.
- **The limit of 2 lost shards counts per run**, cumulatively across waves (`prior_lost`). The limit
  is an alarm that the run's infrastructure is wrong. If losses come per runner-hour, a waved run
  sees the same number as today. If they come per job, at least three losses in 72 jobs happen with
  probability 0.0253%. Either way a per-run limit costs almost nothing, while a per-wave limit would
  allow 8 losses before the alarm.
- **Only attempt 1 repairs**, as today. A waved run discarded by `decide` is dispatched again, at
  the same offset. The discard report shows no figures, so the re-run is not chosen by a peek.

### D9: Recording a stopped run in the ledger

`Measurements/README.md` already rules on this: an SPRT that crossed a bound gives a decision, not a
size. The lab rows follow that rule:

- **Games** is the number of games played at the stop.
- **Elo** is the pooled figure at the stop, marked `(at stop)`, for example
  `**+9.52 +/- 4.27** (at stop)`. It is the only figure the run has. Two sources describe its bias,
  and they measure different things:
  - The replay's 33 early stops averaged −0.02 against the same run's full batch. That is a
    difference between two estimates, on a gain-heavy mix.
  - The simulation models a true +3 stopped early as reading about +6 (+3.04 bias). That is bias
    against the true value, and it is largest near the bounds.
- **Verdict** records a decision between the two hypotheses, not a size:
  - H1 is `gain (SPRT H1 [0, 3])`, which the vocabulary already covers ("an accepted H1").
  - H0 is a new vocabulary entry, `no gain (SPRT H0 [0, 3])`: an accepted H0 on the lab's SPRT, with
    the sign unresolved.

  Neither is quoted as "more than +3" or "less than +3". At these error rates an H1 can happen at a
  true +3 or below.
- **A crossing at wave 4** played every planned game. It takes the SPRT verdict, without the
  `(at stop)` mark.
- **The cap fallback** gives today's interval verdicts from the full batch. That differs from the
  local SPRT ledger rule, where a cap-stopped run is always `inconclusive @ N`. The lab's cap applies
  the fixed-batch rule because the owner set it as the fallback.

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
- **The bounds.** Exact values, and inclusivity tested at the computed thresholds, not at rounded
  literals. Also v = 0, both directions of the cap fallback, and `continue` before W.
- **The plan.** Cuts for (R, W) = (738, 4), (900, 1) and (7, 4). For several (S, R, offset, W),
  every shard's slices must tile its range contiguously and disjointly. The R ≥ 4 guard refuses
  R = 3 and accepts R = 4.
- **`summarize`** over fixture `needs` JSON for every outcome.
- **`slice_artifact` and `replay_artifact`** at W = 1 and W = 4.

`test_strength_workflow.py` reads both workflow files and adds:

- The wave gate (`!cancelled() && decision == 'continue'`) and the moved partial-wave rule,
  evaluated over outcome tables as `discard_rule()` does today.
- Aggregate fixtures, run through the real Bash blocks:
  - H1 at wave 1
  - H0 at wave 2
  - the cap, both pass and fail
  - discard at wave 3
  - a repaired wave 2 and then H1 at wave 3
  - a counts mismatch, a verify failure and a missing decision, each refused with DISCARDED and no
    figures although every shard job was green
  - a fixed W = 1 run, whose report keeps today's form
  - a complete and an invalid multi-arm run at W = 1
  - a repaired W = 1 run taken through the whole chain: assemble, verify and pool, canonical upload,
    marker. The names and texts must match today's.

## Assumptions I cannot verify from the code

- **A1: a failed called workflow still passes on its outputs.** When a called workflow's `match`
  job fails but its `decide` job succeeds, the caller still gets `decide`'s outputs, and a
  `!cancelled()` guard on the next call still runs. Not verified. Slice 2 settles it first, with a
  throwaway draft-PR probe (`on: pull_request`): a called workflow with one failing job and one job
  that emits an output, and a caller that prints the output and gates on it. If it fails, D1 unrolls
  and this document goes back to review.
- **A2: called jobs appear in the run's job list.** The jobs API (`filter=latest`) lists them as
  `<caller job name> / <job name>`, for example `wave-1 / match (3)`, with the check-run annotations
  `plan_repair.py` reads. The read must be authenticated with the scope the calling job grants
  (`actions: read`, `checks: read`, `contents: read`). Not verified. The same probe settles it: a
  called job runs the job, annotation and artifact reads under that scope. The probe also has a red
  first call whose `decide`-like output drives a second reusable call.
- **A3: a wave slice plays at the pace it would have inside a continuous shard.** This is the
  spike's stated limit, since the transposition table is lost only between games, which happens
  anyway. Not verified. It is read on the first waved Full runs, comparing each wave's slowest-slice
  duration with the spike's per-slice durations (slice 4).
- **A4: wave boundaries cost about a minute.** The spike measured jobs starting 3 s after creation
  on one run. At a boundary all 20 slots are briefly free, so a PR's CI can take some of them and
  delay the next wave by up to one CI job's length. Not verified. Slice 4 reads it from the first
  waved runs' job timings, split into toolkit staging and download, queueing, play, and `decide`,
  which reads a growing prefix of logs. It reports how much of the ~1 min budget each part uses.
  This changes the savings estimate but no decision here.
- **A5: SPRT accuracy carries over to new changes.** The 29% is a historical replay estimate. Its mix
  was made gain-heavy by the eval epic, and it includes Night-sized runs. A period of near-zero
  changes saves less: the simulation gives 3.89 h against 4.07 h at a true 0. The owner accepted this
  in #962. Before slice 4 adopts the ship rule, `simulate.py` runs again with the production wave
  sizes (3,330, 3,312, 3,330 and 3,312 pairs), the production `Elo − err` calculation and +5 among
  the sampled Elo values. Adoption pauses for the owner if the pass probability at any sampled Elo
  moves by more than one point from today's rule.
- **A6: `gh run rerun --failed` on a waved run.** It would re-run the failed wave and everything
  after it. Not verified for W > 1, so the docs keep it as a remedy for one-wave runs only, and D8
  re-dispatches a waved run. The slice 2 probe checks the W = 1 retry shape the docs keep: a shard
  that fails before its upload on attempt 1 and passes on a `--failed` retry. On that retry the
  downstream jobs must rerun, and the batch must pool.

## Invariants

- **I1:** The plan is fixed in `setup`. The slices of each shard tile
  `[offset + i·R + 1, offset + (i+1)·R]` in wave order, and no result can change which pairs a wave
  plays.
- **I2:** No decision pools a partial wave. A wave is complete only under today's rule: every shard
  succeeded, or runner-lost shards were repaired on attempt 1.
- **I3:** A decision at wave k uses every slice of waves 1..k. Each wave was verified at batch level
  by its own `decide` against its planned starts and pairs, and every log's count matches its plan.
- **I4:** The reported figure is the one the decision was taken on. The aggregate's pooled Ptnml
  equals the deciding wave's `counts`.
- **I5:** A one-wave run keeps today's form in every respect a reader or tool consumes:
  - the games played and how they are pooled, multi-arm included;
  - the report and its `Replayed` text;
  - every artifact name, raw replay pieces included;
  - the marker's repair-plan payload.
- **I9:** Nothing is published unless the summary is not discarded and the verify, the pool and the
  counts check all succeed. Nothing published means no verdict, no PR comment, no joined shard and
  no marker. A failure reports DISCARDED with no figures.
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
| Join, verify or pool of a stopped prefix wrong (I3, I4, I9) | aggregate fixtures for every outcome, including the counts mismatch, the verify failure and the missing decision |
| Fixed runs changed (I5) | slice 2 merge gate: W = 1 fixtures (single-arm, multi-arm complete and invalid, the repaired chain), plus the smoke run's artifact names and report against today's |
| A1, A2, A6 at W = 1 | the slice 2 probe, before any orchestration code; local expression tests cannot settle these |
| More than 100 artifacts in a run (D6) | slice 3: paginated `Sync-LabPgn.ps1` artifact reads, with a split-listing self-test |
| End-to-end waves | slice 3: a calibration null smoke run, `sprt=true calibration=true games=144 shards=2`, with identical reference and settings. It is expected to run all four waves to the cap. A null can cross by chance, so check its decision, and if it stops early, repeat it for the plumbing check. Optional: a known-sign control at Full (`reference_tc=5+0.1`), expected to stop H1 at wave 1 |
| A3, A4 | slice 4 reads the first waved Full runs' job timings, split by phase |
| A5 | slice 4 re-runs the simulation at production wave sizes before adoption |

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
- `slice_artifact(run, wave, waves, shard)` and `replay_artifact(run, wave, waves, shard, index)`

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

Every run is fixed-size here. Fixed-size compatibility is the merge gate:

- the probe settles A1, A2 and the W = 1 shape of A6;
- the W = 1 fixtures pass: single-arm, multi-arm complete and invalid, and the repaired chain through
  the marker;
- a lab smoke run (`games=144 shards=2`) goes green with today's artifact names and report.

**Slice 3: SPRT waves.**

- The `sprt` input and D5's guard.
- `wave-2` to `wave-4` with D1's gate and the `prior_lost` chain.
- `decide`'s SPRT step.
- `sprt.py summarize`.
- D6's join, verify, pool and counts check in `aggregate`.
- D7's report.
- D11's remaining fixtures.
- Paginated artifact reads in `Sync-LabPgn.ps1`, for both the marker lookup and the shard count. A
  four-wave run with repairs can hold 111–129 artifacts, and today's reads stop at the first 100.
  Its self-test includes a listing whose needed entries come after the first 100.

Done when the fixtures pass and the calibration null smoke run reaches the cap with four waves in
its table. If the smoke run stops early by chance, repeat it. The optional known-sign control is the owner's call: about 1.1 h on 18 runners.

**Slice 4: adopt it as the ship rule.** First, re-run the simulation at production wave sizes
(A5). Then:

- `measure-strength`: the cost row reads ~1–4 h.
- `strength-lab.md`: the Full row dispatches with `sprt=true`, the ship rule is D5's verdict, and a
  discarded waved run is re-dispatched.
- `Docs/CI.md`: replace "a sequential test does not shard across runners" with the wave rationale
  and the Night-run exception.
- `Measurements/README.md`: `no gain`, the `SPRT H1/H0 [0, 3]` labels, `(at stop)`, Games played,
  a crossing at wave 4, and how the lab's cap differs from the local cap rule.
- `Docs/Changelog.md`.

Then read A3 and A4 from the first waved Full runs and record them under the issue.

## Cost

- **Size:** slice 1 is 200–300 lines in 2 files. Slice 2 is over 200 lines in about 8 files: the
  workflow split, `batch_slices.py`, `plan_repair.py`, `plan_arms.py`, fixtures and docs. Slice 3
  is about 250 lines in 5 files, `Sync-LabPgn.ps1` pagination included. Slice 4 is about 80 lines
  of docs plus an offline simulation re-run.
- **Blast radius:** the strength lab and the PR gate's Python self-tests. No engine code. Slice 2
  touches every lab run's plumbing (D2). Slice 4 changes the ship rule.
- **Review:** one code review per slice. Slice 2 is the riskiest diff.
- **Lab time:** the slice 2 smoke run takes about 10 minutes and the slice 3 null smoke run about 15
  minutes, each on 2 runners.
- **Required, not optional:** the I4 counts check, about 10 lines.
- **Optional:** the known-sign Full control, about 1.1 h of the lab. It proves the H1 stop end to
  end at real size, where the fixtures prove it only on fixtures.

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
