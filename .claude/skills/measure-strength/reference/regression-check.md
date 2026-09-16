# Regression check: a change that should cost nothing

For a refactor, a configuration or plumbing change, or anything else meant to leave search alone.
The question is **did anything unforeseen happen**, not "how much faster is it". Two runs, both
against the **merge base**:

1. **Behaviour** — `Compare-SearchEquivalence.ps1 -After <candidate> -BaselineRef origin/main`.
   Node counts, best moves and every `info string` line must match. It builds and caches its own
   baseline. That exe answers equality only; leave it out of the speed run.
2. **Speed** — a paired `Run-Bench.ps1` series, below.

## The paired bench series

1. **Build the baseline the way the candidate was built.** Check out the merge base in a detached
   worktree (`git worktree add --detach <path> <sha>`) and run its own `build.ps1 main`. An exe
   from any other build path is a different binary: the equivalence cache's baseline read 7.7%
   slower than a `build.ps1` build of the same commit.
2. **Run pairs back to back**, baseline then candidate, 6 pairs, each with `-Csv`. Leave the
   machine otherwise idle.
3. **Discard the first pair** as warm-up; it read ~3 points off the rest.
4. **Check node counts match per position in every pair.** A mismatch means behaviour changed, and
   the pair's nps is meaningless.
5. **Report the per-pair aggregate nps delta**: mean, standard deviation and range over the kept
   pairs.

## Reading it

**Done** is a spread that sits at or above zero. A delta whose whole range is negative is a
slowdown: find it before shipping. A small positive delta is layout noise and order bias (the
baseline always runs first), so report it as "no slowdown", never as a speedup. Claiming a speedup
is a different measurement: alternate the order, and see `Docs/Workflow.md` → Speed and nps.
