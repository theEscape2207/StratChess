# Log-based LMR reduction base — Design

**Issue:** #809 (child B1 of #636)

## Goal

The LMR base `int(sqrt(d-1) * sqrt(m-1))` overshoots its `d-2` cap at almost every reduction. #730
measured 99.0% of reductions on the cap, so a late quiet is scouted at depth 1 whatever the node's
depth. Reducing less has gained twice: the `d-1` → `d-2` cap (#363, +9.64 ± 3.63) and B2's
history adjustment (#730, +10.23 ± 6.25). #809's triage probes measured the opportunity:

- **Reach.** Depth 5 and up holds 21% of reductions but 55% of the nodes in reduced searches. At
  depth 3 the cap binds for any table. Depth 4 is mostly capped, but arm A changes move indices 3-7
  and arm B changes index 3. P2 measured mean child depth there going from 1.10 to 1.15.
- **Cost.** A log base adds +29% nodes at depth 12.
- **Accuracy (spike S1).** The log base hides 27% fewer moves at depth ≥ 5, but today's rate is
  only 0.19% of reduced fail-lows.

So this is a tree-shape bet that only the strength lab can price. This change makes the base a
tunable log formula, off by default, so one binary carries the lab's arms.

## Review focus

- **D3, default off in the lab PR.** The binary the lab runs must be node-identical to the merge
  base at its defaults. Otherwise the arms do not isolate the base change.
- **D3, the two-run gate.** The screen advances an arm at +3 Elo, which is lenient on purpose, and
  the confirmation decides. Check that the confirmation's fresh-opening range and its three
  outcomes leave no result undecided.
- **D2, scaled-integer options.** Truncating `base + ln d · ln m / divisor` to int is the rounding
  every arm is measured under. A reviewer should check that the arms' child depths are the ones
  #809's P1 table quotes.

## Scope

**This change will:**

- Add a log base to `lmr_reduction()`: `R = int(base/100 + ln(d) · ln(m) · 100 / divisor)`, then
  `max(1, ·)` and the existing `d-2` cap. B2's history shift still follows, unchanged.
- Add two tuning fields with UCI names, `LmrLogBase` and `LmrLogDivisor`. A divisor of 0 keeps
  today's sqrt formula.
- Clamp the resolved root depth to `MAX_PLY - 1` (D5).
- After the confirmation, set the defaults to the confirmed arm in the same PR. Leave them at 0 if
  the screen or the confirmation parks it (D3).

**This change will not:**

- Change the `d-2` cap, `lmr_min_depth`, `lmr_min_move_index` or LMR eligibility. Depth 3 is out
  of reach by construction, and depth 4 is mostly out of reach (Goal).
- Re-tune `LmrHistoryDivisor`. B2's "reduce more" side becomes active below the cap, and the lab
  measures B1 together with B2 at 64. A divisor re-screen is a follow-up only if B1 ships.
- Add depth-band telemetry. The existing `lmrhistory capped` counter already shows whether the
  base moved off the cap, and that guards against a silent no-op.

## Decisions

### D1: A log lookup table, not `std::log` per call

`ln(i)` for `i` in `[0, MAX_PLY)` is precomputed once into a namespace-scope table in
`AIPerplex.h`. Slot 0 is unused. The table covers every move number (`MoveList::MAX_MOVES` = 218),
and D5 bounds every depth that reaches it. The log branch asserts that the index is in range. The
divisor-0 branch never reads the table and gains no domain restriction. That keeps
`SearchLmrTests.cpp:27`'s pure-helper grid, which runs up to depth 256, valid.

The table does not depend on tuning, so it has no lifecycle tied to `setoption` or to the search
start.

- **Rejected: `std::log` at each reduction.** Cached logarithms keep a transcendental library call
  off the reduction path, where today's two `sqrt` calls are single instructions.
- **Rejected: fall back to `std::log` past the table.** It costs nothing measurable, but it is a
  branch for a depth the engine cannot complete. D5 removes the case instead. The project owner
  chose this on 2026-10-11, to revisit if the engine ever needs deeper searches.
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

The rules below are fixed before dispatch. The owner approved both runs on 2026-10-11.

1. **Screen** (about 20,000 games, 6,660 per arm, about ±6.3 Elo per arm). The best arm advances
   if its point estimate is **at least +3 Elo**, about half its error bar. The bar is set low on
   purpose, because a +4 ± 6.3 winner is still worth confirming. Otherwise keep 0 and park #809.
2. **Confirmation** (one arm against the merge base, about 25,000 games, about ±3.2 Elo). It uses
   fresh openings: `opening_offset` is set past the screen's range, so it never replays the
   openings the screen selected on. Then:
   - Elo minus its error bar > 0: set the defaults to that arm and ship.
   - A positive estimate whose interval spans zero: the owner decides, as #735's +2.98 ± 3.23 did.
   - Otherwise: keep 0 and park #809.

The confirmation is the gate, not the screen. A best-of-three screen estimate runs high: #735's
divisor 16 screened at +6.99 ± 6.16 and confirmed at +2.98 ± 3.23. The confirmation carries no
selection bias.

- **Rejected: ship 0.75/2.25 as default and lab it alone.** It forfeits the arms, and a loss could
  not be told apart from a bad constant.
- **Rejected: ship the screen winner when Elo minus error bar > 0 (#730's rule).** It is both too
  strict and too lenient. It rejects a +4 winner that may be real, and it ships a lucky +7
  uncorrected.

### D4: One pure function keeps both bases

`lmr_reduction(depth, move_number, ordering_score, history_divisor, log_base, log_divisor)`
stays pure and testable. Both call sites pass the new fields: the reduction in `pvs()` and the
base in `record_lmr_reach()`. The profile counter therefore measures the active base's cap share.
A struct parameter was considered, but every caller already holds the four tuning ints, so
positional ints match today's signature style.

### D5: Clamp the root search depth to `MAX_PLY - 1`

`resolve_limits()` (`SearchLimits.cpp:50`) clamps `effective_depth` to `MAX_PLY - 1` (255).
Today it passes `go depth N` through unbounded. Every search calls `control_.ApplyLimits()`, and
so `resolve_limits()`, before it reads `EffectiveDepth()` (`AIPerplex.cpp:395,411`). So one clamp
covers every entry point. No extension raises remaining
depth: the singular extension keeps the parent's depth (`AIPerplex.cpp:1032`), and nothing adds
+1. So the root bound holds at every `lmr_reduction()` call. Ply is already capped at
`MAX_PLY - 1` (`AIPerplex.cpp:649`), so no search deeper than 255 could complete anyway, and no
real depth is lost.

- **Rejected: reject `go depth` above 255.** The GUI would then receive no `bestmove` and wait
  forever. Tools that send a large depth to mean "until stopped" keep working under a clamp.

## Assumptions I cannot verify from the code

- **The lab accepts two options per arm.** Verified locally:
  `.github/scripts/plan_arms.py list` on the D3 string prints arms A-C, each with both options.
  The options are whitespace-separated, as in `candidate_uci_options`. Before any shard starts, the
  workflow checks that the engine advertises each option.
- **Release nps is unchanged at defaults.** At default 0 the only addition is one predictable
  branch on the reduction path. This is unmeasured; `Compare-Bench.ps1` settles it (Validation).

## Invariants

- `1 <= R <= max(1, depth - 2)` for every input. For `depth >= 3`, the `lmr_min_depth` default,
  that keeps the call site's `depth - 1 - R >= 1` assert true. A lower configured `lmr_min_depth`
  stays exempt, as the assert already allows.
- `lmr_log_divisor == 0` gives exactly today's R for every `(d, m, score, history_divisor)`, over
  the full existing test grid (depth 1..`MAX_PLY`).
- Every search's root depth is at most `MAX_PLY - 1`, so every `(depth, move_number)` that
  reaches the log branch indexes inside the table (D5). This is asserted in the log branch.

## Validation

Engine tier, search change: full `Validate-PrePR.ps1`.

- **Unit tests, `SearchLmrTests.cpp`.** Divisor 0 equals the restated sqrt base over the existing
  grid. For each arm, R lies in `[1, cap]` over d = 3..255 and m = 1..218. Spot values reproduce #809's P1 table: arm 75/225 gives
  child depth 7/6/6/5 at d = 10, m = 5/10/20/40. A depth-4 case pins the reach that is not
  capped: at d = 4, m = 3 and score 0, arm A gives R = 1, against 2 today.
- **Depth clamp, `SearchLimitsTests.cpp`.** `go depth` 255 resolves to 255, while 256 and
  `INT_MAX` resolve to 255. Depth-less limits keep their 50 / default.
- **Tuning tests, `SearchTuningTests.cpp`.** Defaults, UCI round-trip and range rejection for both
  options, and the option listing lines.
- **Node identity.** `Compare-SearchEquivalence.ps1` at defaults against the merge base.
- **nps at defaults.** `Compare-Bench.ps1` against the merge base, per `measure-strength` →
  regression check. Fix the rounds before launch, build both binaries with their own
  `build.ps1 main` under clang-cl, and announce the quiet window. Record the interval and the
  verdict. Escalate only as the regression check prescribes, if the first series is unresolved.
- **Reach.** On a depth-12 profile build at arm 75/225, the `capped` share falls from 99.0% to
  about 79%. P3's per-band cap rates predict that figure; what remains capped is almost all at
  depth 3-4.
- **Strength.** D3's screen (about 3.5 h), then its confirmation if the screen advances an arm
  (about 4 h; #735's 24,696-game run took 4 h 04 min). The owner approved both on 2026-10-11.
  For the confirmed arm, add the #636 Method inputs: the depth-12 profile screen and the depth-14
  wall clock, recorded beside the lab row in `Measurements/ci-per-change.md`. Neither gates.

## Cost

- **Size:** 50-200 lines, in `AIPerplex.h`, `AIPerplex.cpp`, `SearchTuning.def`,
  `SearchLimits.cpp`, three test files,
  `Docs/EngineGuide.md` (option list) and `Docs/Changelog.md`.
- **Blast radius:** Engine tier, search change. One code review plus `search-reviewer`.
- **Measurement:** two lab runs, both approved: the screen, then the confirmation if the screen
  advances an arm. Plus about 40 min of local profile screen and wall clock for the confirmed arm.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 why a ln table, not `std::log` | source comment on the table |
| D2 hundredths scaling, 0 = sqrt | `SearchTuning.def` comments on both fields |
| D3 arms, lab result, shipped default | `Docs/Changelog.md`, `Measurements/ci-per-change.md`, PR body |
| P1-P3 and S1 probe results | #809 comments (already published); a changelog summary line |
