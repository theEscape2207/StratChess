# Regression check: a change that should cost nothing

For a refactor, a configuration or plumbing change, or anything else meant to leave search alone.
The question is **did anything unforeseen happen**, not "how much faster is it". Two runs, both
against the **merge base**:

1. **Behaviour** — `Compare-SearchEquivalence.ps1 -After <candidate> -BaselineRef origin/main`.
   Node counts, best moves and every `info string` line must match. It builds and caches its own
   baseline. That exe answers equality only; leave it out of the speed run.
2. **Speed** — `Compare-Bench.ps1`, below.

## The paired bench series

1. **Build the baseline the way the candidate was built.** Check out the merge base in a detached
   worktree (`git worktree add --detach <path> <sha>`) and run its own `build.ps1 main`. An exe
   from any other build path is a different binary: the equivalence cache's baseline read 7.7%
   slower than a `build.ps1` build of the same commit.
2. **Quiet the machine.** Finish builds and the code review first, then follow
   [the quiet-window rule](../SKILL.md#before-a-local-measurement-quiet-window) at launch.
   Review subagents running beside a series read one pair −14% (#640).
3. **Run** `Compare-Bench.ps1 -Baseline <exe> -Candidate <exe> -BaselineCommit <sha>
   -CandidateCommit <sha>`: 12 rounds of alternating order, about 5 min. Fix `-Rounds` before it
   starts. Its `-?` covers the schedule, the rejections and the verdict rule.
4. **Report its verdict** with the interval line and the output directory's `metadata.json`.

## Reading it

**No slowdown** is done. Report a positive delta as "no slowdown", never as a speedup: timing noise
and placement have not been ruled out.

**Claiming a speedup** — when faster nps is the change's success criterion — needs the
**Speedup** verdict twice: once from a `-Control` series, and again on the placement-equalised pair
that `New-OrderedBuildPair.ps1 -BaselineTree <dir> -CandidateTree <dir>` relinks from both built
trees. The script only issues Speedup while the whole A/A interval lies within ±0.5%; a wide control
is not a quiet one.
This covers node-identical changes only. A change that reshapes the tree is judged on wall clock and
Elo, and the script rejects it.

**Unresolved or Slowdown is not yet a slowdown.** If the change added no per-node work, code
placement alone accounts for several percent — #556 read −3.90% over 9 pairs and was pure
placement. Escalate in order, each a new series with its round count fixed up front:

1. `-Control -Rounds 60 -Affinity 4`, about 35 min: an identical baseline copy measures the
   machine's own noise. Six short pairs that read inconclusive have resolved this way.
2. `New-OrderedBuildPair.ps1` on both trees, then re-run on the pair it prints. It fails rather
   than return a pair whose `pvs` and `quiescence` addresses differ.

Only a delta that survives the relink is a slowdown, and then find it before shipping.
