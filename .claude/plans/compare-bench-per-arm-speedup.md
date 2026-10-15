# Compare-Bench per-arm Speedup — Design

**Issue:** #827

## Goal

`Compare-Bench.ps1 -Control` issues **Speedup** only when the whole A/A interval lies inside ±0.5%,
whatever the candidate's margin. A clear gain over both baseline copies (#825: +1.88% [+1.32, +2.44]
and +2.35% [+1.75, +2.95], A/A [−0.94, +0.03]; #725: +8.26% against A/A [−1.03, +0.82]) reads
**No slowdown**, so agents either treat it as unproven or spend a 35–40 min quiet window on a
60-round rerun to get the label. The reverse also happens: #825's 12-round ordered run got Speedup
from a quiet A/A although its lower bound against the baseline arm was −0.01%. The rule tests
machine noise, not whether the gain exceeds it.

## Review focus

- **D1, the triple condition.** Is requiring positive lower bounds against baseline, control *and*
  their mean sufficient where the old A/A veto guarded? The triage probe shows two per-arm
  conditions alone pass a 600-round case whose mean comparison is ~0; the mean condition closes it.
  Look for a further input shape that passes all three yet should not.
- **D3, one ordered run is enough.** Dropping the mandatory plain `-Control` series before the
  ordered pair.

## Scope

**This change will:**

- Replace the Speedup rule in `Get-Verdict` (D1) and turn the A/A band into a printed warning (D2).
- Say in the report which Speedup condition failed when one did.
- Update `-?`, the report's next-step lines, `regression-check.md` → "Claiming a speedup" (D3) and
  `Docs/Changelog.md`.
- Add self-tests (Validation).

**This change will not:**

- Change No slowdown / Slowdown / Unresolved, the −0.5% slowdown tolerance, the estimator, the
  schedule, warm-up, validity refusals or `-TrendOnly` output.
- Calibrate the 0.5% A/A band or derive any error rate; add a joint interval for the conjunction.
- Detect or attribute a drifting arm (#787), or detect whether the exes are an ordered pair.
- Re-label historical results.

## Decisions

### D1: Speedup needs all three lower bounds above zero

With `-Control`, the verdict is **Speedup** iff the 95% lower bound is > 0 for candidate vs
baseline, candidate vs control, and candidate vs mean(baseline, control). Otherwise it falls through
to the existing mean-based No slowdown / Slowdown / Unresolved checks. Without `-Control`, no
Speedup, as now. The reported estimate and `VerdictBasis` stay candidate vs mean(baseline, control).
Exactly 0 fails (strict `>`), as now.

`Get-Verdict` takes the three intervals instead of `-AaInterval`; it returns the verdict plus the
names of failed Speedup conditions, so `Format-Report` can print e.g.
`Speedup not issued: lower bound <= 0 against baseline.` only when the mean lower bound is > 0
(otherwise there was no Speedup claim to explain).

Rejected: **the issue's two per-arm conditions alone** — mean(1/x) > 1/mean(x) lets noisy references
make each per-arm ratio read ~+1% while the mean comparison reads 0 (triage probe, 600 rounds).
**Keeping the A/A band as a veto** — demands the same precision for a 0.2% and an 8% gain.
**An A/A-width multiple of the gain** — an uncalibrated constant, the same problem in a new form.

### D2: A/A outside ±0.5% is a warning, not a veto

When the A/A interval does not lie wholly inside ±0.5%, the report prints one line after the
verdict, e.g. `WARNING: A/A [-0.94%, +0.03%] leaves +-0.5%: the two baseline copies disagree
(offset and/or imprecision). The verdict already accounts for it through the per-arm intervals.`
It names no faulty arm and does not void the run. It prints for every verdict, not only Speedup, and
not under `-TrendOnly` (that output stays unchanged).

The band gets its own constant, `$ControlBandPct = 0.5`, recorded in `metadata.json` as
`ControlBand`; it stops sharing `$SlowdownTolerancePct` because acceptable regression and control
agreement are separate questions. Both stay 0.5.

Rejected: dropping the A/A check entirely — #787's uniformly slow control is the case a reader needs
flagged.

### D3: one Speedup on an ordered pair with `-Control` confirms a speedup

`regression-check.md` currently needs Speedup twice: a plain `-Control` series, then the
`New-OrderedBuildPair.ps1` pair. With D1 the ordered run carries both controls itself, so a
**predeclared** `-Control` series on the ordered pair that returns Speedup is sufficient. A plain
series stays optional, for screening or when the shipping layout is itself the question. Every
series tried is reported, failures included; no rerunning until the label appears.

The script cannot tell an ordered pair from an as-built one, so the Speedup next-step line becomes
conditional: `Confirmed only if both exes came from New-OrderedBuildPair.ps1; otherwise relink and
rerun with -Control. Not Elo.`

Rejected: a `-Ordered` switch or exe-provenance check — new surface for a fact the operator already
knows.

## Assumptions I cannot verify from the code

None. The historical numbers are quoted from #825/#827 logs; tests use synthetic fixtures declared
as such, not replayed raw data.

## Invariants

- No Speedup without `-Control`, or when any of the three lower bounds is ≤ 0.
- No slowdown / Slowdown / Unresolved are decided exactly as today from the mean comparison.
- `-TrendOnly` output is byte-identical for the same input.
- Validity refusals, schedule and aggregation are untouched.

## Validation

Tier **Tooling**. `Compare-Bench.ps1 -SelfTest`, `Compare-BenchLinux.ps1 -SelfTest` (its
`-TrendOnly` consumer) and `Validate-PrePR.ps1`. No engine build, benchmark or Elo match: the change
is verdict logic over recorded numbers, fully exercised by synthetic inputs.

New or changed self-tests (boundary table over `Get-Verdict` plus end-to-end `New-Series` fixtures):

| Case | Expect |
|---|---|
| #825 spike shape: all three lower bounds > 0, A/A [−0.94, +0.03] | Speedup + warning |
| #825 12-round ordered shape: baseline lower −0.01, control +0.13, quiet A/A | not Speedup (stricter than today) |
| #825 24-round ordered shape: both arms > 0, A/A low −0.51 | Speedup + warning |
| synthetic #725 shape: +8% over both arms, A/A ±1% | Speedup + warning |
| existing FALSIFY opposed noise, candidate 1.01, 12 rounds | not Speedup |
| same noise, 600 rounds, candidate 1.00 (per-arm both > 0, mean ~0) | not Speedup |
| same noise, candidate 1.30 | Speedup |
| swapped baseline/control noise | same verdict as unswapped |
| a lower bound exactly 0 | not Speedup |
| no control | never Speedup |
| narrow A/A, ordinary gain | Speedup, no warning |
| fallbacks and `-TrendOnly` | unchanged existing cases pass |

## Cost

- **Size:** 50–200 lines across `Compare-Bench.ps1`, `regression-check.md`, `Docs/Changelog.md`.
- **Blast radius:** Tooling tier; the measure-strength skill's speedup procedure.
- **Review:** one standard code review.
- **Optional parts:** the failed-condition explanation line (~5 lines) — drop it and the verdict
  alone still states the outcome, at the cost of the reader recomputing why.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 triple condition and the mean(1/x) counterexample | `-?` verdict section; comment on `Get-Verdict` |
| D2 warning-only band, separate constant | `-?`; comment on `$ControlBandPct` |
| D3 one ordered run confirms | `regression-check.md` → Claiming a speedup |
| the change and its #825/#725 motivation | `Docs/Changelog.md` |
