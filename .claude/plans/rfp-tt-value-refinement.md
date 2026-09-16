# Reverse futility on a TT-refined evaluation — Design

**Issue:** #545 · spike #548 · triage https://github.com/theEscape2207/StratChess/issues/545#issuecomment-5704056807

## Goal

Reverse futility decides on one static evaluation even when the node's TT probe just returned a
searched MAIN value that failed to cut off. When that value is a lower bound above the static
evaluation, it is better evidence that the node stands above beta. Spike #548 measured −2.86% wall
clock and −4.6% main nodes at fixed depth from using it, on a baseline that predates depth-2 LMP.

## Scope

**This change will:**

- let reverse futility compare a refined value: `max(static_eval, tt_value)` when the failed-cutoff
  MAIN entry is `LOWER` or `EXACT` and non-mate;
- add one runtime catalogue flag for it, default on.

**This change will not:**

- change `static_eval`, frontier futility or its fail-low floor (#529), quiescence stand-pat,
  null-move pruning, singular eligibility, TT layout or TT storage;
- use `UPPER` values (downward refinement belongs with frontier futility, after #529);
- change reverse futility's margin, depth band, fail-hard return or its no-store rule;
- add a rule-50 guard (D4).

## Decisions

### D1: Refine reverse futility only, upward only

Chosen: a local `rfp_eval` that starts from `node_eval()` and is raised to the TT value. `node_eval()`
stays the true static evaluation, so frontier futility and its floor read exactly what they do today.
Rejected: refining `static_eval` itself — the frontier floor bounds a *reported* score, and raising it
could exceed the alpha that licensed a skip (#545 triage). Stockfish keeps the same split.

### D2: Trust any stored depth

Chosen: no depth condition on the entry. #548 showed every flip comes from an entry shallower than the
node: a non-mate `LOWER`/`EXACT` entry with `depth >= node depth` and a value `>= beta` already takes
the TT cutoff above. A depth-qualified rule is therefore a no-op. The trust gap is bounded by the band:
MAIN entries have depth ≥ 1 and the node depth ≤ `reverse_futility_max_depth` (3 by default), so the
entry is at most two plies shallower by default. Raising `ReverseFutilityMaxDepth` over UCI widens
that gap too; that is accepted as a tuning experiment's own responsibility.
Rejected: a `depth - k` margin field. With the default band it would only drop depth-1 entries at
depth-3 nodes; no evidence says those are the bad ones, and a new tunable needs its own measurement.

### D3: Runtime catalogue flag, default on

Chosen: `TUNING_FIELD(bool, reverse_futility_tt_refine_enabled, true, false, true, true,
"ReverseFutilityTtRefine", true)` in `SearchTuning.def`, read when capturing the TT value. This
follows the catalogue convention (LMP, frontier futility) and lets a lab or `Run-EloMatch
-CandidateOptions` compare on/off on one binary.
Rejected: a compile gate — end state for a search feature is a runtime bool or deletion; the spike's
`if constexpr` form was disposable.
Cost: one predictable load-and-branch per MAIN non-cutoff hit and one compare at eligible RFP nodes.
Measured by the gate-off bench in Validation, not assumed free.

### D4: No rule-50 context guard; defer to #347

Chosen: no halfmove-clock condition. The refinement consumes the same MAIN entries the TT cutoff
already trusts unguarded; the widening is shallower entries, at depths 2–3, for a fail-hard cut that
stores nothing. #549 counted zero cutoffs near the boundary on the bench and on high-clock self-play
replays (max clock 53). If #347 later lands a guard, it sits in the probe and covers this read too.
Rejected: a local `halfmove_clock() + depth` guard — hot-path work against measured-zero exposure, and a
second, divergent policy beside #347's.
**Owner decision (2026-09-17)**: accepted. #545 ships without a guard; any rule-50 guard belongs to a
future #347 implementation, which would cover this read along with the TT cutoff.

## Assumptions I cannot verify from the code

- **Headroom survives LMP.** #552 prunes late quiet moves at depth 2, where 67% of #548's flips were,
  so −2.86% is an upper bound. Not verified. Settled by the re-screen in Validation; the #548 park rule
  applies (≥1% median wall clock, ≥7/9 rounds faster), and a miss closes #545 rather than proceeding.
- **Fewer nodes at fixed depth converts to Elo.** Not verified and not inferable: the extra cuts rest
  on the least-trusted entries. Settled only by an owner-approved lab run.
- **#549's zero exposure is representative.** Its corpus never reached clock 80. Not verified further
  here; residual risk is stated in D4.

## Invariants

- Flag off: search is node-identical to `origin/main` at `Threads=1` (same nodes, PVs, best moves).
- `node_eval()` / `static_eval` never takes a TT value; frontier futility's skip test and floor are
  unchanged with the flag on.
- Only a MAIN entry from this node's own probe refines; exclusion frames (no probe), PV nodes and
  in-check nodes (ineligible) never do.
- A mate-range value, an `UPPER` bound, or a value not above static eval never refines.
- Reverse futility still returns exactly `beta` and stores nothing.

## Validation

Engine tier, search behaviour change.

1. **Tests** (`SearchFutilityTests.cpp`, falsified by breaking each guard): refinement cuts on `LOWER`
   and `EXACT` above static eval; no cut from `UPPER`, wrong-direction, mate-range, `QUIESCENCE`-phase
   entries, or with the flag off; frontier futility unaffected; exclusion frame unaffected. Catalogue
   default/UCI parse cases in `SearchTuningTests.cpp`. Re-check `SearchTTContractTests.cpp` "a TT bound
   inside the window does not change the value" still states the window contract, not RFP.
2. **Equivalence, flag off:** land the field with default `false` first;
   `Compare-SearchEquivalence.ps1` against the merge base → `IDENTICAL`. Paired bench of that build
   vs merge base, 9 interleaved rounds: nps within noise (the runtime check's cost).
3. **Headroom re-screen:** flip the default to `true`; 9 interleaved `Run-Bench.ps1` depth-12 rounds
   vs step 2's build, clang-cl, `Threads=1`. Report median/range wall clock and main/QS nodes. Park
   rule as above.
4. Full fast suite, `[tactical]` and `[tactical_full]` with the flag on; `search-reviewer`.
5. **Elo:** a lab run vs merge base is required, but only after step 3 passes and the owner approves
   the spend. No Elo claim from steps 2–3.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 split (`rfp_eval` vs `static_eval`), D2 any-depth reason | source comment at the reverse futility guard |
| D3 flag semantics | `SearchTuning.def` comment beside the field |
| D4 no rule-50 guard, residual risk | `Docs/EngineContracts.md` → Search internals, and a note on #347 |
| Re-screen numbers, lab result | `Measurements/ci-per-change.md`, `Docs/Changelog.md`, PR body |
