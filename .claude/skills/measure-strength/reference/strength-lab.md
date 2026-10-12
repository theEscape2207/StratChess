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
  concurrent job slots for ~3 h. A second dispatch, a re-run of one shard included, queues behind
  the running one, so two lab runs never overlap.
- **A failed shard discards the whole batch**, not just itself: the survivors are the ones that
  happened to avoid whatever went wrong, so pooling them would be a biased subset wearing a full
  batch's error bar. A shard that failed for an infrastructure reason ("The hosted runner lost
  communication with the server", in the job's annotations) can be re-run alone with
  `gh run rerun <run> --failed`: it replays the same openings, so the completed batch is the one
  designed, not a selection. A shard that failed in the engine or harness discards the batch.
- **Screening several settings in one run:** `candidate_arms` splits the shards among K option sets,
  each pooled alone at about √K times the full run's error bar. Confirm the winner in a
  single-candidate run with its own `opening_offset`. The best of K noisy arms reads high, so the
  screen figure is never the claimed gain.
- Results go in `Measurements/ci-per-change.md` or `ci-anchor.md`.
