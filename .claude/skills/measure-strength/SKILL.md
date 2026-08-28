---
name: measure-strength
description: Measure engine strength (Elo) or search speed (nps) — choosing between Run-Bench,
  a local Run-EloMatch batch, a local SPRT and the CI strength lab, plus the compiler and
  opening-book rules that silently invalidate a result. Use when the user asks to measure,
  benchmark, run a match or the strength lab, check for a regression, or decide whether a change
  is worth its cost.
---

Two different questions, two different tools. `Run-Bench.ps1` measures **nps**;
`Run-EloMatch.ps1` measures **strength**. The goal is measured positive Elo — speed serves that,
it is not the objective.

## Pick the instrument first

| Question | Instrument | Cost | Gives you |
|---|---|---|---|
| Is it faster? | `Run-Bench.ps1` (nps) | seconds | a speed delta, not Elo |
| Did behaviour change at all? | `Compare-SearchEquivalence.ps1` | minutes | exact node/bestmove equality |
| Is it better — yes/no? | local **SPRT** (`-Sprt`) | 40 min – 2.5 h | a verdict, not a number |
| How much better? (> ~25 Elo) | local fixed batch, 500 games | ~40 min | ±25 Elo |
| How much better? (< ~25 Elo) | CI strength lab (`strength.yml`) | ~3 h, 18 of 20 CI slots | ±5 Elo at ~20k games |
| < ~5% nps difference | none — an Elo match cannot resolve it at any affordable game count |||

Two failures to avoid:

- **Reporting a fixed batch's point estimate as a measurement.** "+8 ±26" is not a measurement of +8;
  recording it as one is how false confidence accumulates. Below ~25 Elo, use SPRT.
- **Assuming 500 games is a ceiling.** It is the default, not a limit — see
  [Sizing the batch](#sizing-the-batch). The local book supports 69,400 distinct games.

```
pwsh -ExecutionPolicy Bypass -File <abs>\StratChessEvolved\Scripts\Run-EloMatch.ps1 [-Smoke] [-Sprt NonRegression|Gain]
pwsh -ExecutionPolicy Bypass -File <abs>\StratChessEvolved\Scripts\Run-Bench.ps1 -Exe <path>
```

## The rule that silently invalidates everything

**Never measure an MSVC-built binary against a clang-built one.** Both run, both look healthy; the
compiler gap alone is worth tens of Elo and gets credited to whatever change is under test.
`Get-BuildArtifact.ps1` defaults to the shipping (clang-cl) build for that reason. Match Lazy SMP
thread count across candidate and reference too.

## Speed

Compare **nps**, never node counts at fixed depth. Node count is a property of the search, not the
machine code — which is exactly what makes it the right *equivalence* check
(`Compare-SearchEquivalence.ps1`), not a speed check. Anything adding per-node work — evaluation
terms as much as compiler flags — gets a bench pass, and a measured slowdown needs a stated benefit
that outweighs it. Effect sizing, repeat runs, and why an eval change needs the per-position column:
`Docs/Workflow.md` → Speed and nps.

## SPRT

- Use `-Sprt NonRegression` / `-Sprt Gain` for anything expected to be worth **less than ~25 Elo**.
- An SPRT that hits the `-Games` cap without crossing a bound is **inconclusive**, not a measured
  zero. Record it as such — and see the LLR formula under Sizing for what it would have needed.
- An SPRT needs a reference that isolates the change, so `Run-EloMatch.ps1` refuses `-Sprt` against
  the fixed anchor (`-AnchorSprt` overrides it for a deliberate cumulative reading).

## Sizing the batch

**`-Games 500` means two different things, and conflating them is the trap:**

- **Fixed batch — 500 *is* the measurement.** An expensive dial (precision scales 1/√N). Raise it
  only when a point estimate is the deliverable — a new reference baseline, or fitting data for
  #117 — not to make a verdict "more certain", which is what SPRT is for.
- **SPRT — 500 is only the give-up point**, with no bearing on the answer's quality. Raising it is
  **statistically free** and costs wall-clock only in runs that would otherwise return inconclusive.
  The 500 default was chosen as a fixed-batch resolution target and merely inherited as the SPRT cap.

**The book is not the constraint.** A large book is already present and auto-resolved by
`Run-EloMatch.ps1` — `EngineTesting\openings-large.pgn`, 34,700 openings = **69,400 distinct
games**. (`-Book` overrides; the committed `openings-250.pgn` is the fallback and yields only 500
distinct games.) The script prints the book and its opening count on every run — read that line, and
note the **discovery glob matches the name, not the content**: that is how #338's first SPRT
exhausted the small book while the full one sat unused in the same directory.

**Reach for wider bounds before more games.** Expected sample size scales roughly with the inverse
square of the indifference region's width, so `-Sprt Custom -Elo0 -10 -Elo1 0` costs about **4×
fewer games** than `NonRegression`'s `[-5, 0]`. Ask the loosest question that still settles the
decision. Buying information per game beats buying more games.

**Estimating what an inconclusive run needed:** `games_needed ≈ N × 2.94 / LLR_at_N`. The #126 row
reached LLR 0.76 at 500 games → ~1,900 games, ~2.5 h. Order-of-magnitude only — LLR is a random walk,
and if the true effect sits *inside* the indifference region it may not converge at any practical N.

## Cost and the operational ceiling

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
- **Measurement budget is the user's call.** Report what deciding would cost and let them choose;
  never start a multi-hour match unilaterally.

## The CI strength lab (`strength.yml`)

`workflow_dispatch` only — it gates nothing and nothing triggers it automatically. Both sides are
built from source by the same GCC, sharded (default 18), pooled **pentanomially** over
colour-swapped pairs. Full input table and internals: `Docs/CI.md` → Strength lab.

- `reference_ref` defaults to `merge-base` — the commit this branch forked from `main`, so the result
  is attributable to **this change alone**. A tag like `elo-reference-v2` measures cumulative
  strength instead; the candidate's own SHA is a null test.
- **One run at a time, repository-wide**, occupying 18 of 20 concurrent job slots for ~3 h. It can
  delay every other PR, so dispatching one is the user's decision — surface the cost, don't just
  start it.
- **A failed shard discards the whole batch**, not just itself: the survivors are the ones that
  happened to avoid whatever went wrong, so pooling them would be a biased subset wearing a full
  batch's error bar.
- Results go in `Docs/EloLog.md`'s **Linux ledger**, which must **never** be compared against the
  local clang-cl rows — same trap as the MSVC rule above, different axis.

## Recording

Method: `Docs/EloMeasurement.md`. Results: `Docs/EloLog.md` (local clang-cl rows and the Linux ledger
are separate and non-comparable). A batch reporting a time loss, illegal move or disconnect is
discarded, never reported — on a shared runner a time loss most likely means the box was
oversubscribed, which invalidates the batch rather than the one game.
