# Log-based LMR reduction base — Design

**Issue:** #809 (child B1 of #636)

## Goal

The LMR base `int(sqrt(d-1) * sqrt(m-1))` overshoots its `d-2` cap at almost every reduction. #730
measured 99.0% of reductions on the cap, so a late quiet is scouted at depth 1 whatever the node's
depth. Reducing less has gained twice: the `d-1` → `d-2` cap (#363, +9.64 ± 3.63) and B2's
history adjustment (#730, +10.23 ± 6.25). #809's triage probes measured the opportunity:

- **Reach.** Depth 5 and up holds 21% of reductions but 55% of the nodes in reduced searches. At
  depth 3-4 the cap binds for any table.
- **Cost.** A log base adds +29% nodes at depth 12.
- **Accuracy (spike S1).** The log base hides 27% fewer moves at depth ≥ 5, but today's rate is
  only 0.19% of reduced fail-lows.

So this is a tree-shape bet that only the strength lab can price. This change makes the base a
tunable log formula, off by default, so one binary carries the lab's arms.

## Review focus

- **D3, default off in the lab PR.** The binary the lab runs must be node-identical to the merge
  base at its defaults. Otherwise the arms do not isolate the base change.
- **D2, scaled-integer options.** Truncating `base + ln d · ln m / divisor` to int is the rounding
  every arm is measured under. A reviewer should check that the arms' child depths are the ones
  #809's P1 table quotes.

## Scope

**This change will:**

- Add a log base to `lmr_reduction()`: `R = int(base/100 + ln(d) · ln(m) · 100 / divisor)`, then
  `max(1, ·)` and the existing `d-2` cap. B2's history shift still follows, unchanged.
- Add two tuning fields with UCI names, `LmrLogBase` and `LmrLogDivisor`. A divisor of 0 keeps
  today's sqrt formula.
- After the lab, set the defaults to the winning arm in the same PR, or leave them at 0 if no arm
  clears.

**This change will not:**

- Change the `d-2` cap, `lmr_min_depth`, `lmr_min_move_index` or LMR eligibility. Depth 3-4 is
  out of reach by construction.
- Re-tune `LmrHistoryDivisor`. B2's "reduce more" side becomes active below the cap, and the lab
  measures B1 together with B2 at 64. A divisor re-screen is a follow-up only if B1 ships.
- Add depth-band telemetry. The existing `lmrhistory capped` counter already shows whether the
  base moved off the cap, and that guards against a silent no-op.

## Decisions

### D1: A log lookup table, not `std::log` per call

`ln(i)` for `i` in `[0, 256)` is precomputed once into a namespace-scope table in `AIPerplex.h`.
It covers `MAX_PLY` (256) and `Move::MAX_MOVES` (218), and slot 0 is unused. It does not depend
on tuning, so it has no lifecycle tied to `setoption` or to the search start.

- **Rejected: `std::log` at each reduction.** About 0.18 reductions per node and a 10-20 ns library
  call add up to an estimated ~1% nps, which this change has no reason to spend. Today's two `sqrt`
  calls are single instructions.
- **Rejected: a full `[d][m]` R table rebuilt when tuning changes.** It saves one multiply and adds
  a rebuild hook on every tuning write path.

### D2: Constants as hundredths, in two integer UCI fields

UCI exposes only integer spins, and the existing fields follow that (`LmrHistoryDivisor`,
`SeePruningMargin`). Fields and domains:

- `lmr_log_base`, `LmrLogBase`: default 0, range `[0, 1000]`. 75 means 0.75.
- `lmr_log_divisor`, `LmrLogDivisor`: default 0, range `[0, 10000]`. 225 means 2.25. 0 selects the
  sqrt formula and makes `lmr_log_base` inert.

Truncation toward zero matches today's `static_cast<int>`. A divisor in `(0, 50)` would reduce
more than the cap almost everywhere and is valid but useless; Validate() does not police taste.

- **Rejected: a `double` field without a UCI name.** The lab's `candidate_arms` passes UCI options,
  so the arms would need three binaries.

### D3: Default off until the lab picks; the lab runs the PR binary

Both fields default to 0, so the PR's binary is node-identical to `origin/main` at defaults.
`Compare-SearchEquivalence.ps1` proves it. The lab runs one candidate binary with three arms
against the merge base, following #730's procedure:

`candidate_arms`: `LmrLogBase=75 LmrLogDivisor=225;LmrLogBase=100 LmrLogDivisor=175;LmrLogBase=100 LmrLogDivisor=150`

These are arms A, B and C. The run uses 18 shards, a multiple of 3.

The rule, fixed before dispatch as in PR #731: ship the best arm if its Elo minus its error bar is
above 0, uncorrected for selection. If none clears, keep 0 and park #809.

- **Rejected: ship 0.75/2.25 as default and lab it alone.** It forfeits the arms, and a loss could
  not be told apart from a bad constant.

### D4: One pure function keeps both bases

`lmr_reduction(depth, move_number, ordering_score, history_divisor, log_base, log_divisor)`
stays pure and testable. Both call sites pass the new fields: the reduction in `pvs()` and the
base in `record_lmr_reach()`. The profile counter therefore measures the active base's cap share.
A struct parameter was considered, but every caller already holds the four tuning ints, so
positional ints match today's signature style.

## Assumptions I cannot verify from the code

- **The lab accepts two options per arm.** Verified locally:
  `.github/scripts/plan_arms.py list` on the D3 string prints arms A-C, each with both options.
  The options are whitespace-separated, as in `candidate_uci_options`. Before any shard starts, the
  workflow checks that the engine advertises each option.
- **Release nps is unchanged at defaults.** At default 0 the only addition is one predictable
  branch on a path taken by about 0.18 reductions per node. This is assumed, and settled by
  Run-Bench (Validation).

## Invariants

- `1 <= R <= max(1, depth - 2)` for every input, so the `depth - 1 - R >= 1` assert still holds.
- `lmr_log_divisor == 0` gives exactly today's R for every `(d, m, score, history_divisor)`.
- The log table covers every `(depth, move_number)` reaching `lmr_reduction()`. It is asserted
  in `lmr_reduction()`.

## Validation

Engine tier, search change: full `Validate-PrePR.ps1`.

- **Unit tests, `SearchLmrTests.cpp`.** Divisor 0 equals the restated sqrt base over the existing
  grid. For each arm, R lies in `[1, cap]` over d = 3..64 and m = 1..218. Spot values reproduce
  #809's P1 table, e.g. arm 75/225 gives child depth 7/6/6/5 at d = 10, m = 5/10/20/40.
- **Tuning tests, `SearchTuningTests.cpp`.** Defaults, UCI round-trip and range rejection for both
  options, and the option listing lines.
- **Node identity.** `Compare-SearchEquivalence.ps1` at defaults against the merge base.
- **nps.** A `Run-Bench` pass at defaults against the merge base, per `measure-strength`.
- **Reach.** A profile build at arm 75/225 shows the `capped` share falling well below 99%. P2
  predicts most depth ≥ 5 reductions leave the cap.
- **Strength.** The lab run in D3, about 3.5 h, approved by the owner on 2026-10-11. For the
  shipped arm, add the #636 Method inputs: the depth-12 profile screen and the depth-14 wall clock,
  recorded beside the lab row in `Measurements/ci-per-change.md`.

## Cost

- **Size:** 50-200 lines, in `AIPerplex.h`, `AIPerplex.cpp`, `SearchTuning.def`, two test files,
  `Docs/EngineGuide.md` (option list) and `Docs/Changelog.md`.
- **Blast radius:** Engine tier, search change. One code review plus `search-reviewer`.
- **Measurement:** one lab run (approved), plus about 40 min of local screen and wall clock for
  the winner.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 why a ln table, not `std::log` | source comment on the table |
| D2 hundredths scaling, 0 = sqrt | `SearchTuning.def` comments on both fields |
| D3 arms, lab result, shipped default | `Docs/Changelog.md`, `Measurements/ci-per-change.md`, PR body |
| P1-P3 and S1 probe results | #809 comments (already published); a changelog summary line |
