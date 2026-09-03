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
| **Is a new eval term worth shipping?** | **CI strength lab** (`strength.yml`) | ~3 h, free minutes, 18 of 20 CI slots | **±4 Elo** — a number, decisive |
| Did I break something? (expected neutral) | local **SPRT** `NonRegression` | 40 min – 1 h | a verdict, if the effect is big enough |
| How much better? (> ~25 Elo) | local fixed batch, 500 games | ~40 min | ±25 Elo |
| < ~5% nps difference | none — an Elo match cannot resolve it at any affordable game count |||

### The default for a new evaluation term is the lab, not a local SPRT

This is the correction the ledgers argue for, and it is worth stating as evidence rather than as
preference. Counting every row ever recorded:

- **`Measurements/local.md`: 15 inconclusive against 11 decisive.** Seven of the last ten are
  inconclusive. #97 alone spent ~5,900 games across four SPRTs and resolved nothing.
- **`Measurements/ci-per-change.md`: 9 of 9 decisive.** Every lab run this project has dispatched
  produced a verdict, including on features the local instrument had already failed on.

The reason is arithmetic, not luck. A local SPRT runs ~12 games/min on one box, so a night is
~2,500 games and **±10 Elo is its floor**. Most eval terms are worth 5-25 Elo, i.e. inside that
floor, and `NonRegression`'s `[-5, 0]` region is narrower still. The lab plays 20,000 games in
parallel and pools pentanomially, so it resolves ±4.

**Cost is not the tie-breaker people assume, either.** The repository is public, so the lab's
runner minutes are free; what it spends is wall-clock and 18 of 20 CI slots. A local SPRT spends
the *user's own machine, exclusively* — no builds, no tests, no second match — and any of those
started alongside it invalidates the batch. Between a 3 h lab run and a 3 h local SPRT, the lab is
cheaper in the resource that is actually scarce, and it is the one that answers.

So: **reach for the lab when the deliverable is "is this term worth shipping"**, and reach for a
local SPRT when the question is "did I break something" or when the change is a search change
expected to clear 25 Elo. Run the cheap local SPRT first if you like — as a smoke test that catches
a disaster in 40 minutes — but plan the lab run as the actual gate from the start, rather than
arriving at it after two inconclusive sessions.

Two failures to avoid:

- **Reporting a fixed batch's point estimate as a measurement.** "+8 ±26" is not a measurement of +8;
  recording it as one is how false confidence accumulates. Below ~25 Elo, use SPRT or the lab.
- **Assuming 500 games is a ceiling.** It is the default, not a limit — see
  [Sizing the batch](#sizing-the-batch). The local book supports 69,400 distinct games.

Build the candidate first (`.\build.ps1 main`) — `Run-EloMatch.ps1` does not build it, and
`build.ps1` defaults to the shipping clang-cl build, the only one comparable against the reference.

```
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\Run-Bench.ps1 -Exe <path>

# fixed batch against the default anchor — "where do we stand"
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\Run-EloMatch.ps1

# does the pipeline work at all — 20 games, ~2 min, resolves nothing
... Run-EloMatch.ps1 -Smoke

# against the merge base rather than the anchor — the only way to attribute a delta to one change.
# Every -Sprt form needs that same isolating reference, so build the merge base first.
... Run-EloMatch.ps1 -ReferenceExe <merge-base build> -ReferenceTag <commit>
... Run-EloMatch.ps1 -Sprt NonRegression -ReferenceExe <...> -ReferenceTag <...>   # "not worse"
... Run-EloMatch.ps1 -Sprt Gain         -ReferenceExe <...> -ReferenceTag <...>   # "worth >= ~10"
... Run-EloMatch.ps1 -Sprt Custom -Elo0 0 -Elo1 5 -ReferenceExe <...> -ReferenceTag <...>

# a cumulative verdict, asked for deliberately — "how far ahead of the anchor are we"
... Run-EloMatch.ps1 -Sprt Custom -Elo0 0 -Elo1 20 -AnchorSprt
```

`-Sprt Custom` requires both `-Elo0` and `-Elo1`. `-Sprt` cannot be combined with `-Smoke`: a
20-game run can never reach a decision, so the result would always read "inconclusive", which looks
like a measurement and is not one.

**Deeper reference, read when the situation calls for it:**

- [`reference/reading-a-result.md`](reference/reading-a-result.md) — what each outcome licenses you
  to claim, and whether more games would help.
- [`reference/why-measurement-is-hard.md`](reference/why-measurement-is-hard.md) — resolution at a
  given N, why the anchor cannot measure your change, the 100 ms floor, bundling terms.

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

- Use `-Sprt NonRegression` / `-Sprt Gain` for anything expected to be worth **less than ~25 Elo** —
  but see the instrument table: for a new eval term the lab is the gate and this is the smoke test.
- An SPRT that hits the `-Games` cap without crossing a bound is **inconclusive**, not a measured
  zero. Record it as such — and see the LLR formula under Sizing for what it would have needed.
- **An inconclusive row is still worth reading.** An interval that excludes zero, plus an LLR that
  *drifts monotonically* rather than plateauing, is real evidence even without a crossed bound. A
  plateaued LLR is the opposite signal: more games will most likely buy another inconclusive row.
- **Two runs of the same comparison do not simply pool.** Both are conditioned on having failed to
  cross a bound, which biases a pooled estimate toward the indifference region. Say "both intervals
  sit in the same place", not the average.
- An SPRT needs a reference that isolates the change, so `Run-EloMatch.ps1` refuses `-Sprt` against
  the fixed anchor (`-AnchorSprt` overrides it for a deliberate cumulative reading).

## Sizing the batch

**`-Games 500` means two different things, and conflating them is the trap:**

- **Fixed batch — 500 *is* the measurement.** An expensive dial (precision scales 1/√N). Raise it
  only when a point estimate is the deliverable, not to make a verdict "more certain".
- **SPRT — 500 is only the give-up point**, with no bearing on the answer's quality. Raising it is
  **statistically free** and costs wall-clock only in runs that would otherwise return inconclusive.

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
  never start a multi-hour match unilaterally. That applies to a local SPRT exactly as much as to a
  lab run — it is the one that takes their machine away — so it is not a reason to prefer one.
- **Do not hold the session open across a long match.** Start it, report that it is running, and end
  the turn. Polling it with a monitor or a background wait keeps a large context alive for hours,
  the prompt cache expires underneath it, and the whole context is re-read at full price on wake.

## The CI strength lab (`strength.yml`)

**The instrument to reach for on a new eval term** (see the table above for why). `workflow_dispatch`
only — it gates nothing and nothing triggers it automatically. Both sides are built from source by
the same GCC, sharded (default 18), pooled **pentanomially** over colour-swapped pairs. Full input
table and internals: `Docs/CI.md` → Strength lab.

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
  against the local clang-cl rows — same trap as the MSVC rule above, different axis.

## Recording

**`Measurements/README.md` is the recording convention** — the verdict vocabulary, the discard
rules, and what belongs in a row's detail section. Read it before writing a row; a row is easy to
write in a way that cannot later be un-misread.

The ledgers are `Measurements/{ci-calibration,ci-per-change,ci-anchor,local}.md`, one per
instrument-and-reference kind, and **a row is only ever read against others in its own file**.
`Run-EloMatch.ps1` appends to `local.md` automatically; CI-lab rows are written by hand.

A batch reporting a time loss, illegal move or disconnect is discarded, never reported — on a shared
runner a time loss most likely means the box was oversubscribed, which invalidates the batch rather
than the one game.
