---
name: measure-strength
description: Measure engine strength (Elo) or search speed (nps) — choosing between Run-Bench, a
  local Run-EloMatch batch, a local SPRT and the CI strength lab, plus the rules that silently
  invalidate a result. Use when asked to measure, benchmark, run a match or the strength lab, check
  for a regression, or decide whether a change is worth its cost.
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

**For a new evaluation term the lab is the gate, not a local SPRT.** A local SPRT's floor is
±10 Elo and most eval terms are worth 5-25, so it returns inconclusive more often than not — the
ledgers run 15 inconclusive to 11 decisive locally against 9 of 9 decisive in the lab. A local SPRT
first is fine as a 40-minute smoke test, but plan the lab run as the actual gate from the start.
The evidence, and the cost argument that is usually got backwards:
[`reference/why-the-lab-is-the-default.md`](reference/why-the-lab-is-the-default.md).

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
- [`reference/sizing-a-batch.md`](reference/sizing-a-batch.md) — what `-Games` means under each
  instrument, the opening book, wider bounds before more games, throughput and the 700–750-game
  background ceiling.
- [`reference/strength-lab.md`](reference/strength-lab.md) — dispatching `strength.yml`, what
  `reference_ref` selects, and why a failed shard discards the batch.

## The rule that silently invalidates everything

**Never measure an MSVC-built binary against a clang-built one.** Both run, both look healthy; the
compiler gap alone is worth tens of Elo and gets credited to whatever change is under test.
`Get-BuildArtifact.ps1` defaults to the shipping (clang-cl) build for that reason. Match Lazy SMP
thread count across candidate and reference too. CI-lab rows are built by GCC and never comparable
against local clang-cl rows — same trap, different axis.

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
  zero. Record it as such — and see `reference/sizing-a-batch.md` for what it would have needed.
- **An inconclusive row is still worth reading.** An interval that excludes zero, plus an LLR that
  *drifts monotonically* rather than plateauing, is real evidence even without a crossed bound. A
  plateaued LLR is the opposite signal: more games will most likely buy another inconclusive row.
- **Two runs of the same comparison do not simply pool.** Both are conditioned on having failed to
  cross a bound, which biases a pooled estimate toward the indifference region. Say "both intervals
  sit in the same place", not the average.
- An SPRT needs a reference that isolates the change, so `Run-EloMatch.ps1` refuses `-Sprt` against
  the fixed anchor (`-AnchorSprt` overrides it for a deliberate cumulative reading).

## Running a match

- **Measurement budget is the user's call.** Report what deciding would cost and let them choose;
  never start a multi-hour match unilaterally. That applies to a local SPRT exactly as much as to a
  lab run — it is the one that takes their machine away — so it is not a reason to prefer one.
- **Do not hold the session open across a long match.** Start it, report that it is running, and end
  the turn. Polling it with a monitor or a background wait keeps a large context alive for hours,
  the prompt cache expires underneath it, and the whole context is re-read at full price on wake.
- A lab run occupies 18 of 20 repository CI slots for ~3 h and can delay every other PR. Say that
  when proposing one — but as the cost it is, not as a reason to fall back on an instrument that
  will not answer.

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
