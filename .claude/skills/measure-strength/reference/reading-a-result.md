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
