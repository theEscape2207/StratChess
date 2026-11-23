# Reading a result

What each outcome licenses you to claim. The verdict vocabulary and the row format are
`Measurements/README.md`; this is how to decide which verdict applies.

Bounds and reported Elo are both **logistic** Elo. `Run-EloMatch.ps1` pins `model=logistic`
deliberately — fastchess's own default is `normalized` (nElo), a different scale on which
`elo1 = 10` would silently mean something else.

- **Within ±error of 0** — no measurable change. This is the *expected* result for a pure refactor;
  byte-identical node-count validation (`Compare-SearchEquivalence.ps1`) is a stronger check for
  those than any match.
- **Clearly negative after pooling** (e.g. −30 ± 18 over 1000 games) — a regression. Investigate
  before merging.
- **Near the bound** — unresolved. Re-run and pool; single batches wander, see
  [why-measurement-is-hard.md](why-measurement-is-hard.md).
- **`H1 accepted` / `H0 accepted`** — a decision, not an estimate. An `H1 accepted` at 300 games is a
  *stronger* claim than a fixed 500-game point estimate, not a weaker one. The point estimate that
  comes with it carries optional-stopping inflation and must not be quoted as the change's value.
- **`inconclusive @ N games`** — a real outcome meaning "smaller than `elo1`, or the budget ran
  out". Not a failure, and **not a measurement of zero**. The script flags it in yellow rather than
  letting it pass as a decision. **An inconclusive `NonRegression` run whose interval excludes zero
  still answers the question it was asked** — "is this worse?" — so it is normally enough to act on.
  It does **not** license quoting the point estimate as the change's value.
- **`FAILURES, discard`** — a time loss, illegal move, disconnect or stall. The script marks the row
  and exits 1. Discard the batch; do not read its Elo. An illegal *PV* warning is not this case
  (#310) — fastchess warns and plays on from `bestmove`.
- **`same binary and configuration — carries no strength information`** — both sides were the same
  bytes with the same options, so the true difference is zero by construction and the Elo column is
  pure noise. Pipeline checks, time-forfeit checks and reference re-pin verification all land here.
  The script detects this rather than offering a flag to suppress the row: a suppression switch can
  be forgotten, and could be reached for after seeing an unwelcome number. **Same binary with
  *different* options is not this case** — that is a configuration comparison and a real
  measurement, which is how the Lazy SMP `+128.55 Elo` row was produced.

**Whether more games would help is a separate question from whether the run decided.** Read the LLR
trajectory, not just its final value: one that drifts steadily toward a bound will likely get there,
one that wanders will not. An SPRT whose true effect lies strictly inside `[elo0, elo1]` is
*expected* to run to the cap, because neither hypothesis ever accumulates evidence against the
other — more games buy another inconclusive row. Deciding a change that small needs a fixed-batch
estimate (size), not an SPRT (decision).

## Before quoting a number

- **State the range before the mean.** Three positions are not a measurement; a node-count delta
  over three read −22% and over five ranged −44% to +229%. Say which sample a number came from.
- **Fixed-depth node counts carry aspiration noise.** Two builds that score differently seed
  different windows, so set `aspiration_enabled = false` before a node delta carries a claim.
- **Stratify a rate by the gate that produces it.** A depth-gated feature averaged over all depths
  is diluted by depth 1, and the average can invert the conclusion.
- **A gate must be able to fail.** Before naming a run as a gate, simulate its power: state the
  expected effect and the instrument's error at its budget. If the interval cannot exclude zero at
  that effect, the run is indicative.
- **"Nothing changed" bounds the whole interval** inside a band declared before measuring. An
  interval spanning zero is unresolved, not equivalent.
- **Interleave any hand-run A/B timing**: one round of every build, then repeat, and quote the
  median of the per-round ratios. This box drifts several percent over minutes, so running one
  build's repeats before the next charges the drift to whichever owned the slow minutes. Absolute
  figures from such a session are not quotable; the ratios are. `Compare-Bench.ps1` does this for nps.
- **A corrected number is fixed once it is on `origin/main`.** `gh pr edit` changes only the PR
  body; check with `git show origin/main:<file>` before reporting the record corrected (#160 merged
  with the retracted claim still in two documents).
