# Why the lab is the default for a gain

The instrument table names the CI strength lab as the gate for "is this worth shipping". This is
the reasoning behind that.

## Resolution

A local SPRT runs ~12 games/min on one box, so a night is ~2,500 games and **±10 Elo is its
floor**. Most changes are worth 3-25 Elo, much of that inside the floor, and `NonRegression`'s
`[-5, 0]` region is narrower still. The lab plays up to 26,568 games in parallel and pools
pentanomially, so it resolves ±3. Locally, #97 alone spent ~5,900 games across four SPRTs and
resolved nothing.

## Cost is not the tie-breaker people assume

What a lab run spends is wall-clock and 18 of 20 CI slots — runner minutes have never been the
constraint. A local SPRT spends the **user's own machine, exclusively** — no builds, no tests, no
second match — and any of those started alongside it invalidates the batch. Between a 3 h lab run
and a 3 h local SPRT, the lab is cheaper in the resource that is actually scarce, and it is the one
that answers.

## The exception

A change expected to clear 25 Elo is above the local floor, so a local SPRT can settle it. That is
the only case where the lab is not the better instrument for a gain.

"Raise `-Games`" is not a way out: 500 is the default, not a limit — the local book supports 69,400
distinct games ([`sizing-a-batch.md`](sizing-a-batch.md)) — but a night's worth of games is still
~2,500, which is where the ±10 Elo floor comes from.
