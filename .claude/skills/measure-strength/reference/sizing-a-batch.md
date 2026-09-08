# Sizing a batch, and what it costs to run

## `-Games 500` means two different things

Conflating them is the trap:

- **Fixed batch — 500 *is* the measurement.** An expensive dial (precision scales 1/√N). Raise it
  only when a point estimate is the deliverable, not to make a verdict "more certain".
- **SPRT — 500 is only the give-up point**, with no bearing on the answer's quality. Raising it is
  **statistically free** and costs wall-clock only in runs that would otherwise return inconclusive.

## The book is not the constraint

A large book is already present and auto-resolved by `Run-EloMatch.ps1` —
`EngineTesting\openings-large.pgn`, 34,700 openings = **69,400 distinct games**. (`-Book` overrides;
the committed `openings-250.pgn` is the fallback and yields only 500 distinct games.) The script
prints the book and its opening count on every run — read that line, and note the **discovery glob
matches the name, not the content**: that is how #338's first SPRT exhausted the small book while
the full one sat unused in the same directory.

## Reach for wider bounds before more games

Expected sample size scales roughly with the inverse square of the indifference region's width, so
`-Sprt Custom -Elo0 -10 -Elo1 0` costs about **4× fewer games** than `NonRegression`'s `[-5, 0]`.
Ask the loosest question that still settles the decision. Buying information per game beats buying
more games.

## Estimating what an inconclusive run needed

`games_needed ≈ N × 2.94 / LLR_at_N`. The #126 row reached LLR 0.76 at 500 games → ~1,900 games,
~2.5 h. Order-of-magnitude only — LLR is a random walk, and if the true effect sits *inside* the
indifference region it may not converge at any practical N.

## Throughput and the operational ceiling

≈**12–13 games/min** at the default `-Concurrency 6`: 500 games ≈ 40 min, 800 ≈ 1 h, 1,900 ≈ 2.5 h.

**The binding ceiling is operational, not statistical.** A background-launched match is capped near
**700–750 games** by a background-task duration limit (the 2026-07-26 mop-up row was killed at
~60 min). Past that, run it in the foreground or expect to resume.

- **Do not raise `-Concurrency`** to buy throughput. It is pinned to physical cores deliberately;
  oversubscribing injects timing noise — or genuine time losses — into a fixed real-time control, and
  a batch with a time loss is thrown away.
- **Lowering `-Games`** is only useful for `-Smoke`. Under SPRT it is counterproductive: an early
  decision costs nothing, so a low cap buys nothing and risks an avoidable inconclusive.
- A completed capped run **cannot be extended** — resume restores the original `-Games`. Decide the
  cap up front.
