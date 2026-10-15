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

A second defect sits under the first: every contrast is a mean of per-round percentage ratios,
which reads above zero for identically distributed arms (mean(x/y) > 1 by Jensen's inequality). The
old veto masked it; removing the veto exposes it (design review B1: three equal arms at ±30% noise
read +9.9% against each other, all lower bounds positive).

## Review focus

- **D1, the contrast.** Mean log ratios against the geometric mean of the two baseline arms. Is
  there an input with identically distributed arms that still reads a positive lower bound, or a
  real-data case where log and percentage contrasts diverge enough to change a verdict?
- **D2, the gate.** Both per-arm lower bounds > 0, plus the A/A-offset guard. Look for an input that
  passes and should not.
- **D4, one ordered run is enough.** Dropping the mandatory plain `-Control` series before the
  ordered pair.

## Scope

**This change will:**

- Compute every contrast as a mean of per-round log nps ratios, reported as a percentage (D1).
- Replace the Speedup rule in `Get-Verdict` (D2) and turn the A/A band into a warning (D3).
- Say in the report which Speedup condition failed when one did.
- Update `-?`, the report's next-step lines, `regression-check.md` → "Claiming a speedup" (D4) and
  `Docs/Changelog.md`.
- Add self-tests (Validation).

**This change will not:**

- Change the No slowdown / Slowdown / Unresolved thresholds, the −0.5% slowdown tolerance, the
  schedule, warm-up, validity refusals or the `-TrendOnly` output format.
- Calibrate the 0.5% A/A band or derive any error rate; add a joint interval for the conjunction.
- Detect or attribute a drifting arm (#787), or detect whether the exes are an ordered pair.
- Re-label historical results.

## Decisions

### D1: contrasts are mean log ratios against the geometric mean of the references

Each round's contrast is `ln(nps_x / nps_y)`, with `nps` the round's aggregate (total nodes / total
ms, unchanged). With `-Control` the reference for the verdict and the per-position table is the
geometric mean of baseline and control, so that contrast is the average of the two per-arm log
contrasts. `Get-Interval` runs on the log values unchanged; Mean, Median, Min, Max, Low and High are
reported as `100 · (e^v − 1)` %, and the sd column as `100 · sd` of the log ratios. The per-round
delta line prints each round's `100 · (e^d − 1)`.

For identically distributed arms the expected log contrast is exactly zero, and on a schedule that
permutes the same values across arms (B1's fixture) the computed mean is zero. Percentage ratios
cannot have that property. On real data the two agree within 0.01 percentage points: replaying
#825's three runs from their raw CSVs moved no bound by more than 0.01, because per-round spread is
0.5–1%, and the bias grows with its square.

Applied **everywhere**, not only to the Speedup gate: one estimator, so the report never shows two
numbers that disagree, and the fallback verdicts lose the same bias. This supersedes the issue's
"keep the current estimator" scope line.

Rejected: **log ratios for the gate only** — fallback verdicts byte-identical, but two estimators
side by side. **Log ratio against the arithmetic mean of references** — biased negative under noise,
so it would issue a phantom Slowdown on equal arms. **A bias correction on percentage ratios** —
needs a noise model; the log contrast needs none.

### D2: Speedup needs both per-arm lower bounds above zero, and must beat a steady A/A offset

With `-Control`, the verdict is **Speedup** iff:

1. the 95% lower bound is > 0 for candidate vs baseline **and** candidate vs control (strict `>`,
   as now); and
2. when the A/A interval excludes zero, the smaller of those two lower bounds exceeds |A/A mean|.

Otherwise it falls through to the existing No slowdown / Slowdown / Unresolved checks on the
geometric-mean contrast. Without `-Control`, no Speedup, as now.

The triage's third condition (lower bound against the reference mean > 0) is dropped: against the
geometric mean, that contrast is the per-round average of the two per-arm contrasts, so its mean is
their mean and its half-width is at most their average half-width. Its lower bound is therefore
≥ the average of the two per-arm lower bounds, and condition 1 implies it. It was added to close the
triage's 600-round case, where the references' nps alternates 1.1/0.9 against a constant
candidate. Under D1 those references have a geometric mean of 0.995, so their contrast is a real
+0.50% with zero width rather than a percentage-ratio bias. The per-arm intervals (each
[−0.31, +1.32]) still block Speedup there.

Condition 2 covers a run whose identical binaries differ steadily, e.g. A/A −2.0% [−2.3, −1.7]: the
run itself shows that arm identity moves nps by 2%, so a candidate +0.8% over the faster copy has not
beaten it. It uses only the run's own measured offset, with a multiplier of 1, and is inert while
the A/A interval spans zero (all four historical runs below).

`Get-Verdict` takes the per-arm intervals and the A/A interval instead of `-AaInterval`. It returns
the verdict and the failed Speedup conditions, so `Format-Report` prints e.g.
`Speedup not issued: lower bound <= 0 against baseline.` when the reference contrast's lower bound
is > 0 but a condition failed. With no claim to explain, it prints nothing.

Rejected: **keeping the A/A band as a veto** — demands the same precision for a 0.2% and an 8% gain.
**A multiple of the A/A width or offset** — an uncalibrated constant.

### D3: A/A outside ±0.5% is a warning, not a veto

When the A/A interval is not wholly inside ±0.5%, one line follows the verdict:
`WARNING: A/A [-0.94%, +0.03%] leaves +-0.5%: the two baseline copies disagree by more than the
band (offset, imprecision or both). Per-arm intervals carry the imprecision; a steady offset is
gated by D2's offset condition. No arm is named faulty.` It prints for every verdict except under
`-TrendOnly`, whose output format stays unchanged.

The band gets its own constant, `$ControlBandPct = 0.5`, recorded in `metadata.json` as
`ControlBand`. It stops sharing `$SlowdownTolerancePct`, because acceptable regression and control
agreement are separate questions. Both stay 0.5.

Rejected: dropping the check entirely, since #787's uniformly slow control is the case a reader
needs flagged.

### D4: one Speedup on an ordered pair with `-Control` confirms a speedup

`regression-check.md` currently needs Speedup twice: a plain `-Control` series, then the
`New-OrderedBuildPair.ps1` pair. With D2 the ordered run carries both controls itself, so a
**predeclared** `-Control` series on the ordered pair that returns Speedup is sufficient. A plain
series stays optional, for screening or when the shipping layout is itself the question. Every
series tried is reported, failures included; no rerunning until the label appears.

The script cannot tell an ordered pair from an as-built one, so the Speedup next-step line becomes
conditional: `Confirmed only if both exes came from New-OrderedBuildPair.ps1; otherwise relink and
rerun with -Control. Not Elo.`

Rejected: a `-Ordered` switch or exe-provenance check, which adds surface for a fact the operator
already knows.

## Historical effect

Replayed from raw CSVs under D1–D3 (scratch replay, not a new measurement). #725's 12-round
raw data no longer exists.

| Run | Today | Revised |
|---|---|---|
| #825 spike, 12 rounds | No slowdown | Speedup, warning |
| #825 ordered, 12 rounds | Speedup | No slowdown (vs baseline [−0.01, +1.44]) |
| #825 ordered, 24 rounds | No slowdown | Speedup, warning |
| #725, 12 rounds | No slowdown | not reconstructible; per-arm intervals were never recorded |

## Assumptions I cannot verify from the code

None. The historical figures come from the raw CSVs above. Tests use declared synthetic fixtures.

## Invariants

- No Speedup without `-Control`, or when either per-arm lower bound is ≤ 0, or when a steady A/A
  offset is not exceeded.
- Identically distributed arms (the same values permuted across arms) give a zero mean contrast.
- No slowdown / Slowdown / Unresolved keep their thresholds, applied to the D1 contrast.
- `-TrendOnly` output keeps its format; only its numbers change, through D1.
- Validity refusals, the schedule and the aggregation are untouched.

## Validation

Tier **Tooling**. `Compare-Bench.ps1 -SelfTest`, `Compare-BenchLinux.ps1 -SelfTest` (its
`-TrendOnly` consumer, which only prepends a note to the report) and `Validate-PrePR.ps1`. No
engine build, benchmark or Elo match: the change is arithmetic over recorded numbers, fully
exercised by synthetic inputs.

Self-tests: a boundary table over `Get-Verdict` plus end-to-end `New-Series` fixtures.

| Case | Expect |
|---|---|
| B1 equal arms: (0.7, 1.0, 1.3) permuted over all arms × all orders, 576 rounds | mean contrast 0, not Speedup |
| B1 same noise, candidate 0.99 | negative mean, not Speedup |
| #825 spike shape: both arms > 0, A/A [−0.94, +0.03] | Speedup + warning |
| #825 12-round ordered shape: baseline lower −0.01 | not Speedup (stricter than today) |
| #825 24-round ordered shape: A/A low −0.51 | Speedup + warning |
| synthetic #725 shape: +8% over both arms, A/A ±1% | Speedup + warning |
| steady A/A offset −2% [−2.3, −1.7], candidate +0.8% over faster arm | not Speedup, offset condition named |
| same offset, candidate +3% over faster arm | Speedup + warning |
| existing FALSIFY opposed noise, candidate 1.01, 12 rounds | not Speedup |
| triage opposed noise, 600 rounds, candidate 1.00 | not Speedup (per-arm lower −0.31) |
| same noise, candidate 1.30 | Speedup |
| swapped baseline/control noise | same verdict as unswapped |
| a lower bound exactly 0 | not Speedup |
| no control | never Speedup |
| narrow A/A, ordinary gain | Speedup, no warning |
| a known log contrast (hand-computed) | percent conversion of mean, bounds and sd matches |
| fallbacks and `-TrendOnly` | existing cases pass |

## Cost

- **Size:** 50–200 lines across `Compare-Bench.ps1`, `regression-check.md`, `Docs/Changelog.md`.
- **Blast radius:** Tooling tier; the measure-strength skill's speedup procedure.
- **Review:** one standard code review.
- **Optional parts:** the failed-condition explanation line (~5 lines). Without it the verdict still
  states the outcome, and the reader has to work out why.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 log contrasts and the Jensen bias they remove | `-?` statistics section; comment in `Get-Comparison` |
| D2 per-arm gate, offset condition, why no third condition | `-?` verdict section; comment on `Get-Verdict` |
| D3 warning-only band, separate constant | `-?`; comment on `$ControlBandPct` |
| D4 one ordered run confirms | `regression-check.md` → Claiming a speedup |
| the change, B1, and the #825 replay table | `Docs/Changelog.md` |
