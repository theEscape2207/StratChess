# Why the lab is the default for a new evaluation term

The instrument table names the CI strength lab as the gate for "is this eval term worth shipping".
This is the evidence behind that, recorded as evidence rather than preference.

## The ledgers

Counting every row ever recorded:

- **`Measurements/local.md`: 15 inconclusive against 11 decisive.** Seven of the last ten are
  inconclusive. #97 alone spent ~5,900 games across four SPRTs and resolved nothing.
- **`Measurements/ci-per-change.md`: 9 of 9 decisive.** Every lab run this project has dispatched
  produced a verdict, including on features the local instrument had already failed on.

## Why, arithmetically

A local SPRT runs ~12 games/min on one box, so a night is ~2,500 games and **±10 Elo is its
floor**. Most eval terms are worth 5-25 Elo, i.e. inside that floor, and `NonRegression`'s `[-5, 0]`
region is narrower still. The lab plays 20,000 games in parallel and pools pentanomially, so it
resolves ±4.

## Cost is not the tie-breaker people assume

The repository is public, so the lab's runner minutes are free; what it spends is wall-clock and 18
of 20 CI slots. A local SPRT spends the **user's own machine, exclusively** — no builds, no tests,
no second match — and any of those started alongside it invalidates the batch. Between a 3 h lab run
and a 3 h local SPRT, the lab is cheaper in the resource that is actually scarce, and it is the one
that answers.

## The resulting rule

Reach for the lab when the deliverable is "is this term worth shipping". Reach for a local SPRT when
the question is "did I break something", or when the change is a search change expected to clear
25 Elo. Running the cheap local SPRT first is fine — as a smoke test that catches a disaster in
40 minutes — but plan the lab run as the actual gate from the start, rather than arriving at it
after two inconclusive sessions.

## Two failures to avoid

- **Reporting a fixed batch's point estimate as a measurement.** "+8 ±26" is not a measurement of
  +8; recording it as one is how false confidence accumulates. Below ~25 Elo, use SPRT or the lab.
- **Assuming 500 games is a ceiling.** It is the default, not a limit — see
  [`sizing-a-batch.md`](sizing-a-batch.md). The local book supports 69,400 distinct games.
