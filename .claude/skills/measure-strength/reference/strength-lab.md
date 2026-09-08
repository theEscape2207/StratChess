# The CI strength lab (`strength.yml`)

`workflow_dispatch` only — it gates nothing and nothing triggers it automatically. Both sides are
built from source by the same GCC, sharded (default 18), pooled **pentanomially** over
colour-swapped pairs. Full input table and internals: `Docs/CI.md` → Strength lab.

```
gh workflow run strength.yml --ref <branch> -f reference_ref=<merge-base|tag|sha>
```

`--ref` supplies the **workflow file** as well as the candidate source, so dispatching against an
old commit runs that commit's harness too.

- `reference_ref` defaults to `merge-base` — the commit this branch forked from `main`, so the result
  is attributable to **this change alone**. A tag like `elo-reference-v2` measures cumulative
  strength instead; the candidate's own SHA is a null test.
- **One run at a time, repository-wide**, occupying 18 of 20 concurrent job slots for ~3 h, so it can
  delay every other PR. Say that when proposing one — but as the cost it is, not as a reason to fall
  back on an instrument that will not answer.
- **A failed shard discards the whole batch**, not just itself: the survivors are the ones that
  happened to avoid whatever went wrong, so pooling them would be a biased subset wearing a full
  batch's error bar.
- Results go in `Measurements/ci-per-change.md` or `ci-anchor.md`, which must **never** be compared
  against the local clang-cl rows — same trap as the MSVC rule, different axis.
