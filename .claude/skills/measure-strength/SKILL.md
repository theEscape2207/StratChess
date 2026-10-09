---
name: measure-strength
description: Measure engine strength (Elo), search speed (nps) or where the time goes — choosing between Run-Bench, a
  local Run-EloMatch batch, a local SPRT and the CI strength lab, plus the rules that silently
  invalidate a result. Use when asked to measure, benchmark, run a match or the strength lab, check
  for a regression, or decide whether a change is worth its cost.
---

`Run-Bench.ps1` measures **nps**; `Run-EloMatch.ps1` and the CI strength lab measure **strength**.
The goal is measured positive Elo — speed serves that, it is not the objective.

## Pick the instrument first

**Showing a change cost nothing** — the common case, and the cheap one:

| Question | Instrument | Cost | Gives you |
|---|---|---|---|
| Did behaviour change at all? | `Compare-SearchEquivalence.ps1` | minutes | exact node/bestmove equality |
| Slower or faster? | `Compare-Bench.ps1` (paired `Run-Bench` nps) | ~5 min | a speed verdict, not Elo |
| Does the speed change differ on the lab's GCC? | `Compare-BenchLinux.ps1` (WSL) | ~15 min | a trend, read beside a lab row |
| Did I break something? | local **SPRT** `NonRegression` | 40 min – 1 h | a verdict, if the effect is big enough |

**Regression check** for a change meant to leave search alone — equivalence plus a paired bench
series against the merge base, and the baseline build that silently skews it:
[`reference/regression-check.md`](reference/regression-check.md).

**Showing a change gained something** — one instrument:

| Question | Instrument | Cost | Gives you |
|---|---|---|---|
| Is this worth shipping? | **CI strength lab** (`strength.yml`) | ~3 h, 18 of 20 CI slots | **±4 Elo** — a number, decisive |

**The lab is the gate for anything meant to gain Elo.** A local SPRT's floor is ±10 Elo and most
eval terms are worth 5-25, so it returns inconclusive more often than not: the ledgers run 15
inconclusive to 11 decisive locally, against 9 of 9 decisive in the lab
([`reference/why-the-lab-is-the-default.md`](reference/why-the-lab-is-the-default.md)). Run the
local SPRT first if you like, as a 40-minute smoke test, but plan the lab run as the gate from the
start rather than arriving at it after two inconclusive sessions.

Once a strength candidate passes correctness and the comparison is valid, prioritise its deciding
lab run within the agreed budget. Optional cost attribution can run alongside or afterward; it
must not become a prerequisite for measuring strength. Record capacity/budget/priority deferrals
as unmeasured, not as strength failures. Before dispatch, record the experiment role and stopping
rule in existing task records; `Measurements/README.md` defines how the result links back to them.

**Explaining a result** (how a search change reshaped the tree, which helps read its wall-clock or
Elo result, or what a #636 child is judged by): `Compare-SearchProfile.ps1 -Before -After` on two
`-DSTRAT_SEARCH_PROFILE=1` builds. It prints ordering, LMR, node types, pruning, quiescence,
iterations and stability, pooled and by endgame group. It measures no time: a speed change with an
unchanged tree is `Run-Bench.ps1`'s question, and where the time goes is
`Measure-CpuProfile.ps1 -Before -After`'s; one UCI command's round trip is
`Measure-UciLatency.ps1 -Command <cmd>`'s. It gives direction, never a
verdict: gate on wall clock and Elo as above. Screen with
`-Seeds 8 -Depth 12 -Positions Tests/profile-screen.fen` (~24 min; catches a 5% late-cut change 97% of the time, a 3% one about 3 times in 5) and
read the Screen block's ±2 SE. One run per side carries tree noise larger than a typical ordering
effect (`Measurements/profile-screen.md`).

A **local fixed batch** (500 games, ~40 min, ±25 Elo) is supported but is not the default: a third
of the lab's wall-clock for a fraction of its precision, so it mostly buys inconclusive runs. Reach
for it when the lab is unavailable. Its point estimate is not a measurement — "+8 ±26" recorded as
+8 is how false confidence accumulates. Below ~5% nps difference, nothing resolves it at any
affordable game count.

## Before a local measurement: quiet window

For `Run-Bench.ps1`, `Compare-Bench.ps1`, `Compare-BenchLinux.ps1`, `Run-EloMatch.ps1`,
`Measure-UciLatency.ps1` and `Measure-CpuProfile.ps1`, do this before each launch:

1. **Estimate before launch** from script help, guidance or comparable past runs, adjusted for
   settings. Include benchmark warm-up and all arms/rounds; for matches use the effective game
   budget, or restored remaining work on resume. For SPRT estimate through the game cap and say
   it may stop earlier. Give an honest rough range for unusual settings; no calibration run is needed.
2. **Wait for the owner's explicit go before a long quiet period**, such as SPRT or an extended
   match, after stating the estimate. Routine seconds-to-few-minutes runs proceed without a new
   permission step. If existing diagnostics show a short run was noisy or unstable and a retry
   needs deliberate quiet, explain that and wait for go before the retry.
3. **Immediately before launching, give one short chat heads-up** with the quiet duration and
   approximate end time in the owner's timezone: `Starting the benchmark: quiet for ~5 min,
   until ~14:30.` Relay it even when console output is hidden. After a long-run go, repeat the
   heads-up with the end time calculated from the actual launch time.

In a multi-step plan, identify the quiet steps: `Build (no quiet needed), collect CPU profile
(quiet), analyse (no quiet needed).` At a useful transition back, say `Collection finished;
analysis doesn't need quiet.` A standalone run needs only its launch heads-up; estimates stay
pre-launch, without live ETA updates or extra banners. This rule communicates the window; it
adds no load detection, script enforcement, verdict changes or cross-session coordination.

A long-run proposal can say: `SPRT needs roughly 40-60 min through the game cap and may stop
earlier. I'll wait for your go.` The launch heads-up still gives the approximate local end time.

## Running one

The lab is `workflow_dispatch` only — it gates nothing and nothing triggers it automatically:

```
gh workflow run strength.yml --ref <branch> -f reference_ref=<merge-base|tag|sha>
```

`reference_ref` defaults to `merge-base`, which attributes the result to **this change alone**; a
tag like `elo-reference-v2` measures cumulative strength instead. A feature that ships default-off is
measured with `-f candidate_uci_options="Name=Value"` rather than a probe branch, so the binary that
plays is the one that will merge; the run fails fast if the engine does not advertise that option,
since it would otherwise ignore it silently and report a null result — as it would for a value that
is not unsigned decimal digits, which is all its UCI parser accepts. Shard count, pooling and the
failed-shard rule: [`reference/strength-lab.md`](reference/strength-lab.md).

Locally, build the candidate first (`.\build.ps1 main`) — `Run-EloMatch.ps1` does not build it, and
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
20-game run can never reach a decision, so it would always read "inconclusive", which looks like a
measurement and is not one.

- **CI measurement budget is the user's call.** Report what deciding would cost and let them
  choose before dispatching the lab. Local-run permission follows the quiet-window rule above.
- **Do not hold the session open across a long match.** Start it, report that it is running, and end
  the turn. Polling keeps a large context alive for hours, the prompt cache expires underneath it,
  and the whole context is re-read at full price on wake.
- A lab run occupies 18 of 20 repository CI slots for ~3 h and can delay every other PR. Say that
  when proposing one — as the cost it is, not as a reason to fall back on an instrument that will
  not answer.

## Where the result came from

The pooled Elo says *whether* a change helped and nothing about *where*. Every lab run also uploads
a fully annotated PGN of every game it played, and `Docs/MoveQuality.md` is the method for reading
them — Tier 1 scores the engine's own annotations, Tier 2 re-judges the same rows with an outside
engine and sees mistakes the engine does not know it made. The scan is diagnosis, not a gate;
running one is skill `analyze-games`.

## The rule that silently invalidates everything

**Never compare binaries from different compilers.** Both run, both look healthy; the compiler gap
alone is worth tens of Elo and gets credited to whatever change is under test. So never measure an
MSVC build against a clang one — `Get-BuildArtifact.ps1` defaults to the shipping clang-cl build for
that reason — and never read a GCC-built CI-lab row against a local clang-cl row. Match Lazy SMP
thread count across candidate and reference too.

## Speed

Compare **nps**, never node counts at fixed depth. Node count is a property of the search, not the
machine code — which is exactly what makes it the right *equivalence* check
(`Compare-SearchEquivalence.ps1`), not a speed check. Anything adding per-node work — evaluation
terms as much as compiler flags — gets a bench pass, and a measured slowdown needs a stated benefit
that outweighs it. For a TT or memory-latency change the bench figure is a floor: its table stays
mostly cold. Effect sizing, repeat runs, why an eval change needs the per-position column, and that
floor: `Docs/Workflow.md` → Speed and nps.

## SPRT

- An SPRT that hits the `-Games` cap without crossing a bound is **inconclusive**, not a measured
  zero. Record it as such — `reference/sizing-a-batch.md` estimates what it would have needed.
- **An inconclusive row is still worth reading.** An interval that excludes zero, plus an LLR that
  *drifts monotonically* rather than plateauing, is real evidence even without a crossed bound. A
  plateaued LLR is the opposite signal: more games will most likely buy another inconclusive row.
- **Two runs of the same comparison do not simply pool.** Both are conditioned on having failed to
  cross a bound, which biases a pooled estimate toward the indifference region. Say "both intervals
  sit in the same place", not the average.
- An SPRT needs a reference that isolates the change, so `Run-EloMatch.ps1` refuses `-Sprt` against
  the fixed anchor (`-AnchorSprt` overrides it for a deliberate cumulative reading).

## Recording

**`Measurements/README.md` is the recording convention** — the verdict vocabulary, the discard rules
and what belongs in a row's detail section. Read it before writing a row; a row is easy to write in
a way that cannot later be un-misread.

The ledgers are `Measurements/{ci-calibration,ci-per-change,ci-anchor,local}.md`, one per
instrument-and-reference kind, and **a row is only ever read against others in its own file**.
Move-quality profiles have two ledgers of their own, `Measurements/move-quality-{tier1,tier2}.md`,
carrying a table per run instead of a verdict row — `Measurements/README.md` states that carve-out.
`Run-EloMatch.ps1` appends to `local.md` automatically; CI-lab rows are written by hand. A batch
reporting a time loss, illegal move or disconnect is discarded, never reported — a time loss most
likely means the box was oversubscribed, which invalidates the batch rather than the one game.

**Two more references, beyond the three linked above:**

- [`reference/reading-a-result.md`](reference/reading-a-result.md) — what each outcome licenses you
  to claim, and whether more games would help.
- [`reference/why-measurement-is-hard.md`](reference/why-measurement-is-hard.md) — resolution at a
  given N, why the anchor cannot measure your change, the 100 ms floor, bundling terms.
