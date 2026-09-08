# The CI strength lab (`strength.yml`)

The dispatch line is in the skill body; this is what it does and what the inputs mean. Both sides are
built from source by the same GCC, sharded (default 18), pooled **pentanomially** over
colour-swapped pairs. Full input table and internals: `Docs/CI.md` → Strength lab.

- `--ref` supplies the **workflow file** as well as the candidate source, so dispatching against an
  old commit runs that commit's harness too.
- `reference_ref` beyond the `merge-base` default: a tag like `elo-reference-v2` measures cumulative
  strength; the candidate's own SHA is a null test.
- **One run at a time, repository-wide**, occupying 18 of 20 concurrent job slots for ~3 h.
- **A failed shard discards the whole batch**, not just itself: the survivors are the ones that
  happened to avoid whatever went wrong, so pooling them would be a biased subset wearing a full
  batch's error bar.
- Results go in `Measurements/ci-per-change.md` or `ci-anchor.md`.
