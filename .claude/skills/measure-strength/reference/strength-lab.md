# The CI strength lab (`strength.yml`)

The dispatch line is in the skill body; this is what it does and what the inputs mean. Both sides are
built from source by the same GCC, sharded (default 18), pooled **pentanomially** over
colour-swapped pairs. Full input table and internals: `Docs/CI.md` → Strength lab.

- `--ref` supplies the **workflow file** as well as the candidate source, so dispatching against an
  old commit runs that commit's harness too.
- `reference_ref` beyond the `merge-base` default: a tag like `elo-reference-v2` measures cumulative
  strength; the candidate's own SHA is a null test only when build configuration, effective options
  and playing conditions also match.
- **One run at a time, repository-wide** (concurrency group `strength-lab`), occupying 18 of 20
  concurrent job slots for ~3 h. A second dispatch queues behind the running one, so two lab runs
  never overlap.
- **A failed shard discards the whole batch**, not just itself: the survivors are the ones that
  happened to avoid whatever went wrong, so pooling them would be a biased subset wearing a full
  batch's error bar. An engine or harness failure ends there. **A shard lost to its runner is
  replayed inside the run**, because the same openings complete the batch designed rather than
  selecting from it: [Repairing a lost shard](#repairing-a-lost-shard).
- **Judge a run by its report heading, not its colour.** A repaired run stays red, since `match (N)`
  failed, and its report still says `## Strength lab` with a `Replayed` row.
- **Screening several settings in one run:** `candidate_arms` splits the shards among K option sets,
  each pooled alone at about √K times the full run's error bar. Confirm the winner in a
  single-candidate run with its own `opening_offset`. The best of K noisy arms reads high, so the
  screen figure is never the claimed gain.
- Results go in `Measurements/ci-per-change.md` or `ci-anchor.md`.

## Batch sizes

| Use | `games` | Shards x pairs | Time | Error bar |
|---|---|---|---|---|
| Normal (the default) | 20088 | 18 x 558 | ~3 h | ±3.48 |
| Full | 26568 | 18 x 738 | ~4 h | ±3.02 |
| Night only, CI unused | 36000 | 20 x 900 | ~4 h 55 (estimated, worst shard ~292 of the job's 340 min) | ±2.60 |

## Repairing a lost shard

**Automatic, in the run.** When every shard has finished, `repair-plan` looks at the failed ones. It
replays up to 2 shards as small jobs from the batch's own toolkit, then `aggregate` verifies each slice
and pools the batch. This adds roughly 15-30 min. A shard counts as lost only when its job failed, "The
hosted runner lost communication with the server" is its only failure annotation, and it left no
artifact. Anything else discards the batch. Cancelling during the repair discards it too, and the
artifacts stay.

**What this can't see:** a lost job's log is gone, so a fatal line played before the loss is invisible,
and a failure that builds up over hours wouldn't recur in a short slice. The owner accepted this: the
surviving shards ran the same binaries for the full duration.

**Manual, the fallback** for a single lost shard the job didn't repair: a repair job was itself lost,
or GitHub reworded the annotation. Three or more lost shards get `gh run rerun <run> --failed` instead,
as does a multi-arm run. Only attempt 1 repairs automatically.

1. **Wait for the batch to finish**, then pool the survivors with `pool_pentanomial.py`, read as a
   preview rather than a result.
2. Dispatch at the batch's exact commit through a temporary branch. `pool_pentanomial.py` refuses mixed revisions, so `--ref main` fails
   once `main` moves. Repeat every other input the batch set, concurrency included, since
   `comparison.md` omits it. Take `<reference sha>` from the preview's `Revisions:` line:

   Size the repair so its shards divide the lost shard's pairs evenly.

   ```
   git push origin <batch sha>:refs/heads/lab-repair-<original run>
   gh workflow run strength.yml --ref lab-repair-<original run> -f reference_ref=<reference sha> \
     -f opening_offset=<batch offset + lost shard x pairs> -f games=<2 x pairs> -f shards=<repair shards>
   ```

   Done when every repair shard is green. A repair shard lost in turn gets
   `gh run rerun <repair run> --failed`.
3. **Pool both runs**, downloading their `strength-<run>-comparison` and `strength-<run>-shard-*`
   artifacts into one directory:

   ```
   python .github/scripts/pool_repaired_batch.py --root <dir> --original-run <original run> \
     --repair-run <repair run> --shards <batch shards> --pairs-per-shard <batch pairs> \
     --lost-shard <lost shard> --repair-shards <repair shards> --opening-offset <batch offset>
   ```

   Done when it prints the pool with no `REFUSED`. Its pool counts each repair shard separately, so
   take shard agreement from its `For the ledger:` line.
4. **Record** both run IDs in the row, then delete the branch.
