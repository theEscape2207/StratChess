# Repair a lost shard inside `strength.yml` — Design

**Issue:** #956

## Goal

A lab batch that loses one shard to its hosted runner reports nothing until someone dispatches a
repair at the exact commit from a temporary branch and pools two runs locally. It happens often
enough to matter: 3 of the last 100 runs lost a runner (`41631693815`, `40242553786`, `38798612654`),
each a single shard, all in the last 40 runs. One was pooled short as 19 of 20 shards and one was
discarded unused. The batch should instead finish itself, with no agent present, by replaying the
lost openings with the same binaries and reporting one result from one run.

This changes what a valid batch means, so the rule is stated exactly in D2.

## Review focus

- **D2 and D4: what makes a batch valid, and what assembling a replay into a shard hides.** The
  aggregate would pool a run whose `match` result is `failure`. A hole here reports a biased figure
  that looks like a measurement.
- **Assumption A3: a runner loss is independent of the games on that shard.** The annotation itself
  says starving the runner can cause it. If the engine causes the loss, the replay can hide that.
- **D3: moving the shard's script out of the workflow.** It touches the path every future batch runs.

## Scope

**This change will:**

- add `repair-plan` and `repair` jobs to `strength.yml`, and change `aggregate`'s refusal;
- add `.github/scripts/plan_repair.py` (planner and assembler), with a self-test run in `setup`;
- move the shard's play script to `.github/scripts/play_shard.sh`, shared by `match` and `repair`;
- make `Sync-LabPgn.ps1` copy repaired runs, which it would otherwise skip;
- rewrite the repair section of `strength-lab.md` and the job list in `Docs/CI.md`.

**This change will not:**

- repair mid-run. A job with `needs: match` starts when every shard has finished, so a 4 h batch
  reports at about 4 h 20. The mid-run failure watcher is a separate, undecided idea;
- turn the run green. `match (N)` stays red and the run's conclusion stays `failure`; the report is
  the verdict (D7);
- repair multi-arm runs (D6), or retry a lost replay automatically (D5);
- remove `pool_repaired_batch.py` (D8).

## Decisions

### D1: A shard is lost only if three signals agree

`repair-plan` classifies every `match (N)` job that did not succeed. It is lost only when:

1. its conclusion is `failure`;
2. its only failure annotation is `The hosted runner lost communication with the server`;
3. the run has no `strength-<run>-shard-<N>` artifact.

Anything else discards the batch, including a batch where another shard qualifies as lost. All three
past losses match exactly, with the identical annotation text. The fatal shards checked (runs
`31281221815`, `31975941562`) carry `Shard N: harness/engine failures detected` instead.

Rejected: **no artifact alone**, the signal used on #895. A shard that fails before fastchess writes
anything, such as a failed toolkit download, also leaves none. Rejected: **annotation alone**, since
an artifact means the upload step ran, so the runner was alive after the games.

**A fatal line before the loss can't be seen, and the owner accepted that risk.** All three lost jobs'
logs return 404, so no log can veto, and requiring one would stop repair ever firing. A runner-caused
time loss only taints that shard's games, and all of those are replaced. An engine bug common enough
to move Elo also shows in the other shards (about 94% of the games) and discards the batch. Rejected:
**a live watcher emitting the annotation mid-game**, which adds code to every batch's play path with
no evidence the annotation would survive the loss.

The match on the wording is deliberate. If GitHub rewords it, the batch is discarded as it is today,
and the fixture holds the text to compare against. A different loss message, such as a runner
shutdown signal, has not been seen in 100 runs and also discards.

**At most 2 lost shards, and at least one survivor** (`MAX_LOST_SHARDS = 2`). At about 1 loss per 600
shard jobs (3 losses in the 100 runs since 2026-08-07, taken as about 1,800 jobs: an estimate, not a
count), two independent losses in one 18-shard batch happen about once in 2,400 batches, and three
essentially never. Three or more means something is wrong with the run, so it stops for a
person, who can use `gh run rerun --failed`. Rejected: no cap, which would replay a whole batch
unattended during an outage.

### D2: The new discard rule

`aggregate` refuses unless one of these holds:

```
needs.match.result == 'success'
|| (needs.match.result == 'failure'
    && github.run_attempt == 1
    && needs.repair-plan.result == 'success'
    && needs.repair-plan.outputs.status == 'repair'
    && needs.repair.result == 'success')
```

| `match` | `repair-plan` | `repair` | Verdict |
|---|---|---|---|
| success | skipped | skipped | pool, as today |
| failure | `repair` | success | assemble, verify, pool |
| failure | `repair` | failure or cancelled | discard |
| failure | `discard`, or the job failed | skipped | discard, with the planner's reason |
| cancelled or skipped | skipped | skipped | discard |
| failure, attempt 2 or later | skipped | skipped | discard |

Both new jobs need `!cancelled()` in their `if:`: without a status function, GitHub adds an implicit
`success()`, which is false once a shard has failed. `aggregate` keeps `always()`.

**Only attempt 1 can repair.** `repair-plan` also requires `github.run_attempt == 1`, so no state has
to say which attempt produced which artifact. Automation is lost only on re-runs, and D5 shows a
re-run is rarely needed. `gh run rerun --failed` stays valid as before: if `match` then succeeds, the
batch pools normally, with no repair.

The reason this is sound is the existing argument turned around. Survivors alone are a biased subset
because they are the shards that avoided the failure. A replay at the same openings completes the
batch that was designed, with no selection on results.

### D3: One play script, staged in the toolkit

The shard's 130-line step moves unchanged to `.github/scripts/play_shard.sh`, staged in the toolkit
beside `cpu_sampler.sh`, and takes `START` and `ROUNDS` instead of deriving the start from the shard.
`match` computes `START` as it does now. Fastchess flags, adjudication and `fatal_re` therefore cannot
differ between a shard and its replay. `pool_repaired_batch.py`'s `fatal_pattern()` re-points at the
script.

Rejected: **a copy of the step in `repair`**, which would make "same settings by construction" false
at the first edit. Rejected: **a composite action or reusable workflow**, because shards deliberately
do not check out the repository, and the toolkit already exists to carry what they run.

### D4: Assemble each replay into the shard it replaces

Every downstream reader already expects `shards/strength-<run>-shard-<N>/`. So a new `Assemble the
replayed shards` step, before `Verify the complete batch`, builds that directory:

1. **Each slice is verified alone** with the unchanged `verify_batch`, as a one-shard batch at the
   slice's own start and pair count: the method `pool_repaired_batch.py` uses. This checks the
   assigned opening, PGN pair count, colours and engine names. `verify_batch` never reads the log's
   counts, so each log also goes through `read_shard(log, slice.pairs)` before its counts are summed.
2. The slices must cover the lost shard's pairs exactly, in order, per the plan.
3. `match.pgn` is the slices' PGNs with `Round` offset to run 1..R. `match.log` is their logs
   concatenated plus one final `Ptnml(0-2)` line of the summed counts, which `parse_counts` reads as
   the last line. There is no `cpu.tsv`: a replay's samples come from several runners, so the report
   reads `Sampled on 17 of 18 shards` and the raw samples stay in the slices.

Then the existing `Verify the complete batch` and `Pool the result` run **unchanged**, with
`--expect-shards` and `--expect-pairs-per-shard` as today. That is what makes a replay count as the
one shard it replaces, with shard agreement correct.

After a successful pool, `aggregate` uploads the assembled directory as
`strength-<run>-shard-<N>` and the plan as `strength-<run>-repair`, so `gh run download -p
'strength-<run>-shard-*'`, `Sync-LabPgn.ps1` and the analysis scripts see a complete batch. The
slices stay as `strength-<run>-replay-<N>-<i>`, 90 days, as the raw evidence. The name does not match
`^strength-\d+-shard-\d+$`, so no reader counts them as shards.The `repair` marker uploads last, once
the assembled shards exist.

The report gains one row, `| Replayed | shard 6, 18 slices (runner lost) |`, so a repaired batch can't
pass for an ordinary one.

Rejected: **teaching `verify_batch`, `pool_pentanomial`, `logs_for_arm` and `summarize_cpu` about
slices.** That changes four readers, and every future reader would have to know too. Its one gain
is a pool with no synthetic files, and the slices retained as evidence cover that.

### D5: Repair shape

Each lost shard gets `min(S // L, R)` slices, for `S` batch shards, `L` lost shards and `R` pairs per
shard. Slice sizes differ by at most one pair, with the larger ones first. A slice stays within one
lost shard, because a fastchess range has to be contiguous.

So the repair never uses more jobs than the batch did, which honours the slots the dispatch chose,
and no slice is longer than a shard, so the 340-minute timeout holds. One lost shard of a default
batch is 18 slices of 31 pairs, roughly 15 min (estimated from #895's 10 x 74 in 29 min). Uneven
sizes mean batches no longer need sizes that divide evenly, so the `Repair shards x pairs` column in
`strength-lab.md` goes.

**A lost replay is not retried automatically.** At about 1 loss per 2,100 runner-hours (the same estimate, at about 3.5 h a job), a repair of 18
quarter-hour jobs loses one on the order of 1 in 500 repairs. With losses in about 3% of runs, that is
roughly once per 15,000 runs (an estimate). It discards the batch, and the manual repair (D8, about 30
min) still works. `repair` sets `fail-fast: false`, as `match` does, so the other slices' PGNs and logs
remain for diagnosis.

Rejected: **one-leg recovery with `gh run rerun --job`**, which needs per-attempt identity for the
plan, slices, assembled shards and marker to guard a case this rare.

### D6: Multi-arm runs are left out

A non-empty `candidate_arms` makes the planner return `discard`. Screens are cheap to redo and
`pool_repaired_batch.py` has no arm support to lose. Rejected: **routing replays by arm now.** The
assembled layout makes it small later, but it adds a path this first version would not prove live.

### D7: A repaired run stays red, so its readers must not take that as the verdict

The run's conclusion stays `failure` because `match (N)` failed, and no workflow setting changes that
for a lost runner (`continue-on-error` is never evaluated). Two consequences:

- `Sync-LabPgn.ps1` lists `--status success` only, so without a change a repaired batch silently
  misses the corpus and expires after 90 days. It also lists failed runs and copies those holding a
  `strength-<run>-repair` artifact, uploaded only after a successful pool, and whose latest attempt is
  1, so a later discarded re-run can't pass on an old marker. Discarded runs still aren't copied.
- `strength-lab.md` tells a reader to judge a run by its report heading. The PR comment is unaffected,
  since it depends on the pool step.

### D8: `pool_repaired_batch.py` stays for now

It remains the fallback when the planner discards a real single loss (reworded annotation) or a
repair job is lost. This is two repair paths, deliberately temporary: the decision to delete it is
taken after the first real repair, per the issue, and the manual procedure shrinks to a short
fallback note meanwhile.

### D9: The owner's stop goes

The 5-minute wait existed because a repair spent lab time and needed a dispatch someone might not
want. Now it costs about 15 minutes of a run already holding the lab, and the batch's outcome is
otherwise lost. Cancelling still works: `repair` is `cancelled`, `aggregate` discards, and the
artifacts remain for local pooling.

Rejected: **keeping a sleep**, which delays every repair for a choice nobody has yet wanted to make.
The survivors' preview also goes, since nothing now waits on it.

### D10: Lab ledger rows get a template, and a repair adds three words

A repaired row adds only `, shard N replayed` after the run id. The owner found the rows and row
details had filled with boilerplate nobody reads, so `Measurements/README.md` gains a template for lab
rows. Detail is written only if something applies, at most 3 sentences: something specific the run
didn't test or separate, an arm tried or superseded, a non-default setting, a null result where nothing
proves the option applied, or anything unusual with its evidence. It never restates what a pooled
figure already implies: forfeits, PV warnings, shard agreement, score, default settings, the comparison
link, or the verdict's rule. Existing rows stay as they are.

Two short facts stay, outside the 3-sentence limit: `Ptnml [a, b, c, d, e]`, on every lab row since a
fixed form needs no judgement, and `bench +0.58%` when the change adds per-node work. The issue number
and commit stay in the row and the detail title, as the keys to find the run, PR and design again.

Rejected: **a `Replayed` field in every row**, which is the same noise again.

## Assumptions I cannot verify from the code

- **A1: A forced power-off yields the real annotation.** The live proof needs a runner loss on demand.
  `sudo poweroff -f` should skip the graceful stop that reports a shutdown signal instead. Not
  verified; settled by the first proof run, with `iptables -A OUTPUT -j DROP` as the fallback. If
  neither gives the annotation, the proof runs against a stubbed annotation lookup and the real path
  waits for a real loss, which comes about 1 run in 13 recently. A stub proves the wiring only, and
  A1 stays open until a real loss passes.
- **A2: GitHub's behaviour on the paths this relies on.** Each is settled by the live proof, and each
  fails closed by discarding:
  - a job token with `actions: read`, `checks: read` can list jobs, artifacts and annotations.
    **Verified only for a personal token**. The planner reads each job's `check_run_url` and
    paginates every listing, since the jobs API defaults to 30 entries;
  - the jobs listing holds every shard. The planner discards unless it finds exactly one completed
    record for each index 0..S-1;
  - the annotation exists when the planner reads it. If a lost shard is the last job to end, a race
    is conceivable; the result is a discard.
- **A3: A runner loss is independent of the games on that shard.** Evidence: three different commits,
  lost at about 1 h 30, 1 h 35 and 3 h 30 into the job, and no failures in #895's replay. Not proof:
  the annotation names resource starvation as a cause, and the CPU samples die with the runner. A
  failure tied to an opening would recur, since the same openings replay, and discard. A failure that
  builds up over hours in one process would not recur in a short slice, and **this is not detected**.
  Accepted because the 17-odd surviving shards ran the same binaries for the full duration without
  it, and the report row names the repair. Comparing replay with survivors is not a usable check:
  #895's replay alone was +3.99 +/- 12.78 against +12.97 +/- 3.11.
- **A4: Short slices play under slightly different conditions, accepted.** At concurrency 3, at most
  two games finish with spare capacity beside them: 3.2% of a 31-pair slice against 0.18% of a 558-pair
  shard. Engine processes also live for fewer games. Neither is shown to treat both sides equally, and
  neither is measured; #895 used the same shape. The report names the replay. A slice under about 30
  pairs only occurs in small test batches, so no minimum size is set.

## Invariants

- A pooled batch is exactly the designed S x R pairs, each opening once, all played from the one
  toolkit under one `comparison.md`.
- Every unclear state discards, as it does today: an unknown annotation, a failed planner, a missing
  job, a slice that is short or at the wrong opening.
- A shard with a visible fatal failure discards the batch, even when another shard is repairable.
- A repaired verdict exists only in attempt 1.
- A run with no lost shard behaves as before: same fastchess command, artifacts and report.
- A shard and its replay differ only in the fastchess `start` and `-rounds`.
- `Sync-LabPgn.ps1` copies repaired runs and never a discarded one.

## Validation

Harness only, with no engine change, so **no Elo match and no bench**. `Build` tier. The gate is the fixtures plus a
live proof; `Compare-SearchEquivalence` has nothing to compare.

**Fixtures.** `plan_repair.py --self-test`, in `setup` and the PR gate, using the jobs and annotation
JSON captured from run `41631693815`:

| Case | Expected |
|---|---|
| one or two lost shards | `repair`, with exact slices and starts |
| three lost, or all lost | `discard` |
| fatal shard alone, or beside a lost one | `discard` |
| one job with the loss annotation and a harness/engine annotation, no artifact | `discard` |
| attempt 2 with a lost shard | no plan; `discard` |
| failed with no artifact and no annotation (toolkit failure) | `discard` |
| annotation with an artifact present | `discard` |
| timed-out or cancelled shard, or a missing `match` job | `discard` |
| multi-arm | `discard` |
| slice sizes for 558/18, 740/18, 900/20, uneven and R < S | cover R exactly, contiguous |
| assembly with a short slice, wrong opening or wrong engine name | refused |
| slice logs with compensating counts (2 and 4 pairs where PGNs hold 3 and 3), or no `Ptnml` line | refused |
| a duplicate job index beside a missing one | `discard` |
| assembled shard | passes `verify_batch` and the pool at S x R; shard tally counts it once |

`test_strength_workflow.py` runs the real workflow blocks against: a repaired batch, which pools;
a failed repair, which reports DISCARDED with no figure; and the staged `play_shard.sh` with a stub
fastchess. The stub asserts the full expected arguments for an ordinary shard with options and an
offset, a multi-arm shard and a replay, that a shard's and a replay's arguments differ only in `start`
and `-rounds`, and that a fatal line or nonzero fastchess exit still fails the script. It also evaluates
the D2 table against the workflow's actual `if:` text, translated to Python, since Actions expressions
don't run locally and that expression is what the change rests on. `Sync-LabPgn.ps1 -SelfTest` covers repaired runs, discarded runs and an old
marker on a later attempt.

**Live proof.** A runner loss can't be scheduled, so a temporary branch on top of the PR head adds a
power-off to `play_shard.sh`, never merged, firing only in `match`, so replay legs survive. Candidate
and reference are the same commit with
`calibration=true`. Total about 40 min of the lab, so timed by the owner:

1. **Repair.** 6 shards x 36 pairs (`games=432`), shard 1 powered off a few minutes in. Closes when:
   the annotation is the real text (A1), the planner finds it with the job token (A2), 6 replay
   legs run, the pool is exactly 216 pairs, `shard-1` and `repair` artifacts exist, and the report
   carries the `Replayed` row.
2. **The ordinary path.** A plain small batch on the PR head with no hook. Closes when it pools,
   the report has no `Replayed` row, and the `repair-plan` and `repair` jobs are skipped.

The first real loss afterwards is the final check, and triggers D8's decision.

## Cost

- **Size:** over 200 lines, about 10 files: `plan_repair.py` plus self-test about 300, `play_shard.sh`
  moved, `strength.yml` about +80, `test_strength_workflow.py` about +60, `Sync-LabPgn.ps1` about +25,
  `pool_repaired_batch.py` one line, docs.
- **Blast radius:** `Build` tier (`.github/workflows`). Every future lab batch runs the moved script. No engine, no gate on
  `main` other than the new self-tests in the existing Linux Release leg.
- **Review:** the code review, plus cross-agent review of this document if the owner routes it.
- **Lab time:** the live proof, about 40 min, with nothing else able to use the lab meanwhile.
- **Optional parts, each priced alone:**
  - the second lost shard (`MAX_LOST_SHARDS` 2 rather than 1): about 10 lines and two fixtures. The
    issue lists it as a gain; the numbers above say it will rarely fire;
  - upload of the assembled shard (D4): about 10 lines. Dropping it means `Sync-LabPgn.ps1` and
    analysis scripts need to learn about slices, so keep it unless that is wanted.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 loss signal, the cap and its numbers | comment in `plan_repair.py`; rule in `strength-lab.md` |
| D2 discard rule, why a replay is not selection | comment on the refusal step; `strength-lab.md` |
| D3 why the script is staged in the toolkit | comment in `play_shard.sh` and `Docs/CI.md` |
| D4 assembly and artifact names | `plan_repair.py` docstring; `Docs/CI.md` |
| D5 repair shape, manual recovery | `strength-lab.md`, replacing the procedure and the table column |
| D7 red run, report is the verdict | `strength-lab.md`; `Sync-LabPgn.ps1` help |
| D8 fallback pending deletion | `strength-lab.md`; note on #956 when closing |
| A3 undetected build-up failure | `strength-lab.md`, beside the discard rule |
| D10 ledger template | `Measurements/README.md`; `strength-lab.md` step 4 goes |
| Live proof runs | `Measurements/ci-calibration.md`, with a `Docs/Changelog.md` line |
