# Depth-2 LMP fail-low UPPER stores — Design

**Issue:** #972, evidence gate for #940.
**Original design baseline:** `731baf276dda0bbd18987753fc766381b7994436`.
**Implementation baseline:** `5d9a0e50224bb609e5b11ffebf766eb29029b2f4` (fresh `origin/main`, 2026-11-10).
**Approved source:** `e3ad9d21`; all six cross-agent findings accepted, no open Blocking findings.

**Record status (2026-11-10):** Experiment completed with a valid D4 H1. Retained because the
measurement ledger cites this approved contract and its final evidence. D1-D4 remain unchanged;
this record does not authorize a production default change. Disposable PR #976 remains unmerged.

## Goal

Decide whether retaining a TT entry after a completed depth-2 LMP fail-low improves playing
strength enough to keep the policy. More stores can supply bounds, searched moves and static
evaluation on later visits, but can also displace useful entries and cut off on a false selective
bound. Neither a missing-store count nor depth 3's bundled LMP gain answers the question.

## Review focus

- D1–D2: does the change isolate the store policy while preserving searched cutoffs, effective
  depth, exclusion and abort contracts? Is the selective-bound exposure stated accurately?
- D3: do the tests distinguish the new alpha floor/store from the old no-store path and from a
  searched fail-high, without relying on a TT entry that was never replaced?
- D4 and A1: is the same-commit comparison interpretable after #970's index decision, and is the
  advance/park rule sufficient to end this experiment without another tuning campaign?

## Scope

**This change will:** add one default-off runtime flag and the bounded lab vehicle, validate it,
and define one separately budgeted strength comparison and its evidence/cleanup outcome.

**This change will not:** change production defaults, pruning eligibility or indices, SEE, IIR,
TT layout/replacement/consumers, or introduce instrumentation, extra arms or threshold tuning.
The owner authorized this implementation/validation/draft-PR slice after design approval.
Paid dispatch requires separate owner budget approval.

## Decisions

### D1: Reuse the common store behind one default-off flag

Add `TUNING_FLAG(late_move_pruning_depth2_store_upper, false, false, true, true,
"LateMovePruningDepth2StoreUpper", true)` beside the existing LMP fields in
`StratEngine/SearchTuning.def`. Existing catalogue machinery supplies JSON/UCI plumbing; extend
the option catalogue expectations in `StratChessTests/SearchTuningTests.cpp` and add a fixture
setter in `StratChessTests/SearchTestFixture.h`.

In `StratEngine/AIPerplex.cpp::pvs()`, extend the existing post-loop alpha-floor condition to
include `lmp_skipped && depth == kLateMovePruningDepth && tuning_.late_move_pruning_depth2_store_upper`.
Use `std::max(best_value, original_alpha)` before bound classification. Add
`!tuning_.late_move_pruning_depth2_store_upper` to the existing depth-2 skipped-fail-low no-store
return. Both edits are necessary: bypassing the return alone could store a searched-subset score
below entry alpha. Keep one common store rather than adding a parallel TT write.

A completed skipped fail-low with the flag on returns entry alpha and attempts to store
MAIN/UPPER/ALL_NODE at entry alpha, with the effective post-IIR `depth`, `best_move` from a searched
child, and the existing `storable_static_eval(...)` metadata. Replacement still decides whether
the entry survives. A searched value at or above beta remains unchanged by the floor and stores
LOWER/CUT_NODE with its searched score. The flag has no effect without a depth-2 LMP skip; it
does not enable LMP itself. Depth 3 retains its existing unconditional floor/store policy.

### D2: Deliberately retain ordinary selective-bound consumers

This UPPER is a selective search claim, **not a proved upper bound over every legal move**.
`SearchLateMovePruningTests.cpp`'s `kFiftyMoveEdgeFen` already demonstrates late pawn pushes
beating alpha while the searched piece moves draw. Main search can consume the new bound at a
shallower requested depth without matching LMP; quiescence accepts MAIN entries of depth >= 1
and can also cut off on it. Main search takes a MAIN entry's move even when too shallow for a
cutoff, so later deeper visits can avoid IIR, whose default minimum depth is 4. At depths 1 and 2,
a nonempty hash move also disables staged generation: a later non-cutting visit uses full
generation with the stored move first. These interactions are part of the measured policy cost
and benefit, not a prediction of gain or a mechanism attribution.

Hash-move-only storage or disabling cutoffs would answer a different question and are rejected
for this experiment. No new store precedes the existing per-move board restoration/abort guard,
and no exclusion-frame store is introduced. Fix the adjacent comments at the floor and no-store
return plus `Docs/EngineContracts.md`'s SEE/LMP paragraph to describe both flag states. That nearby
documentation debt is directly touched; avoid duplicating a store or adding a new helper layer.

### D3: Exercise the changed store with the existing fifty-move-edge fixture

Extend `StratChessTests/SearchLateMovePruningTests.cpp`, reusing its seeded history, fixture index
12 and planted depth-1 EXACT entry. Keep the default-off no-overwrite test. Add the flag-on case
with reverse futility and quiet SEE pruning disabled, so neither bypasses or supplies the result.
Require actual LMP skips, return alpha, replacement of the planted value, depth 2, MAIN, UPPER,
ALL_NODE, and a nonempty best move among the searched piece moves rather than either skipped
pawn push. Require board restoration. Static-eval preservation follows from retaining the
existing common store and its unchanged `storable_static_eval(...)` argument (D1); this fixture
would only compare sentinels, so it adds no separate metadata-control assertion.

Run the existing searched-LOWER-after-skip and no-skip control with the new flag both off and on.
Run the existing depth-3 UPPER fixture with it on as well; its depth-2 children are kept ineligible
by the fixture's index 64. Exercise exclusion with the flag on and a planted parent entry,
requiring no skip and no overwrite. Add a deterministic mid-child abort case at depth 2 with
the flag on, using the same seeded fifty-move-edge fixture. Arm the existing poll-injection seam
for a searched piece-move child after the two skipped pawn pushes; the parent and initial twelve
drawn children each poll before draw adjudication. Require `lmp_skips() > 0` together with
`search_is_aborted()`, restored board and an unchanged planted parent entry. Completed descendant
entries remain allowed. An abort before the first skip does not meet this test's acceptance
criteria; the whole-search LMP abort test alone does not prove parent TT suppression.

### D4: Freeze the baseline, one run and the decision rule

#970 is closed: run `42363219133` compared depth-3 index 12 against 8 at `f864646d`, 26,568 games,
`-0.25 +/- 3.06` Elo, cap at LLR `-2.15`; its rule retains index 8. PR #975 recorded this in
`Measurements/ci-per-change.md`. The implementation baseline includes that decision; the only
change since the original design baseline is that ledger, with no search-path changes.
Fixture indices remain explicit and both match sides inherit the same defaults.
The owner's reserved quiet-SEE-margin follow-up is not displaced by this experiment.

Keep the disposable vehicle in an unmerged draft PR through the lab verdict, using the repository
PR workflow and required validation before dispatch. After explicit owner budget approval,
dispatch one Full waved `strength.yml` run with candidate and reference at the **same pinned
vehicle commit**, same compiler/build configuration and all other effective options identical:
`games=26568`, `shards=18`, `sprt=true`, both time controls `10+0.1`, `threads=1`, `concurrency=3`,
candidate `LateMovePruningDepth2StoreUpper=true`, reference explicitly `LateMovePruningDepth2StoreUpper=false`.
The flag is a UCI check option; numeric 1/0 are rejected. The explicit reference default produces
an expected redundant-default warning. Use `reference_ref=<vehicle SHA>` and the PR branch held
at that SHA as the dispatch ref. Record the run role as
the deciding policy experiment and all effective inputs before dispatch. Run serially with other
lab work. Expected occupancy is roughly 1–4 h and 18/20 CI slots; automatic runner repair can
extend that estimate (`measure-strength`, `reference/strength-lab.md`).

- SPRT H1: advance to a fresh production review; no automatic default flip.
- SPRT H0: park #940 and remove the experimental policy, flag and dedicated tests/plumbing.
- No crossing at the full cap: advance only if Elo minus its reported error bar is above zero;
  otherwise park and remove. An early stop supplies a decision, not an Elo magnitude.
- Time loss, illegal played move, disconnect or stall: discard the batch. Follow the current
  waved automatic repair rules for eligible runner losses; no automatic retry budget, manual
  pooling of surviving shards or additional arm is authorized.

Record pinned revisions, inputs, run link, verdict and resulting decision in
`Measurements/ci-per-change.md`, including discarded attempts, and link it from #972 and #940.
Deferral for budget/capacity remains unmeasured. A non-pass ends this policy, not a tuning series.
Land the evidence through a separate evidence-only PR from fresh main before closing the vehicle
PR, recording the vehicle SHA and preserving run/artifact links and the final design record.
Closing the vehicle PR deletes its remote branch (`.github/workflows/pr-closed-cleanup.yml`).
On a valid non-pass, close without merging its experimental code; no removal PR is needed.
On a pass, a fresh production follow-up selects the approved code; the disposable PR itself
still does not merge. An invalid or budget-deferred run leaves the vehicle parked pending the
owner's decision, with no retry implied.

## Assumptions I cannot verify from the code

**A1 — Net benefit required measurement.** Extra writes, replacement pressure and changed
downstream search could outweigh useful reuse. D4's isolated run accepted H1 (see Final evidence
below); that decision does not establish a gain magnitude or which reuse mechanism caused it.
The historical suppressed-store percentage is not reused as a current rate or benefit forecast.

**A2 — Hosted capacity/runtime and runner-loss recovery.** The 1–4 h estimate and repair delay
come from the current measurement skill, not a new measurement. Effective revision/options and
completion are verified from the dispatched run's logs/artifacts and pooled report before a
verdict is accepted. Run 42849368403 completed successfully in about 2 h 4 min with no runner
repair; its verified comparison and result are preserved below. Recovery remains unexercised.

## Invariants and validation

This artifact is Docs tier; the vehicle is Engine tier and UCI-facing. No engine measurements
are needed to review this design. Before any paid run, the implementation must supply:

| Invariant / risk | Closing evidence |
|---|---|
| Default off preserves search and the planted entry | `Compare-SearchEquivalence.ps1 -After <fresh vehicle exe> -BaselineRef <pinned main SHA>`, Threads=1, identical nodes, best moves and compared iteration/telemetry lines; unchanged depth-2 no-overwrite fixture |
| New path stores alpha and the searched move at effective depth 2 | D3's flag-on replaced-entry case; temporarily remove the new floor and require its value assertion to fail, then restore it; separately disable the flag-on bypass and require the store assertions to fail |
| Searched LOWER, no-skip and depth-3 behavior are preserved | D3's paired flag-state cases plus focused `[search][lmp]`, `[search][tt]` and `[search][quietsee]` tests |
| Exclusion and aborted frames retain no parent results | D3's planted-entry exclusion case and deterministic mid-child abort after actual skips with flag on; board restoration checked |
| Static evaluation and runtime binding are preserved | Source review confirms the unchanged common-store static-eval argument (D1); catalogue/default/domain/JSON/UCI checks in `[tuning]` and `[uci]` |
| Other engine contracts remain intact | Repository full code/domain review and Engine-tier `Validate-PrePR.ps1` gates; build current sources before testing |
| Enabled policy is worth its full time-controlled cost | D4's same-commit on/off lab verdict, including option-advertisement checks; no node-count or nps proxy |

The required paired bench is informational for this experiment: compare pinned main against
flag-off vehicle with `Compare-Bench.ps1`, the same shipping toolchain/build path and 12 measured
rounds plus warm-up. Report its interval and metadata under `measure-strength` quiet-window/build
rules. Its identical-node gate rejects changed trees; enabled extra-store cost is included in
the time-controlled lab comparison. The fresh production review consumes this default-off
overhead evidence; the same-commit lab comparison excludes it and parking removes it. Record
slowdown/uncertainty without independently parking a valid strength candidate on a bench result.
No optional profiling campaign is added.

## Cost

Working estimate: 50–200 implementation/test/doc lines across about 5–7 files, plus this design
and review records; Engine tier, one new UCI catalogue entry, one specialist design review,
one cross-agent design review and one full code review. The repository's code-review planning
benchmark is 170–270k tokens and 3–5 min per run (`.claude/plans/TEMPLATE.md`, #624 pilot;
not a measurement of this change).
The paid run is priced separately in D4. No optional instrumentation or second arm.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Default-off selective alpha store and unchanged consumers (D1–D2) | `pvs()` floor/no-store comments and `Docs/EngineContracts.md`; removed if parked |
| Acceptance, discard and advance/park rule (D4) | #972 declaration and `Measurements/ci-per-change.md` result detail, linked from #940 |
| Bench and lab evidence and production/park outcome | Evidence-only PR's measurement ledger; headline in `Docs/Changelog.md` when the production outcome warrants one |
| Reviewed proposal and dispositions | `StratChessSupport/DesignRecords/972 - lmp-depth2-store-upper{,.domain-review,.review}.md` |

Any implementation change to these approved decisions is recorded here with its reason before
Harvest. Remove the transient plan only after its durable decisions and evidence have homes.

## Final evidence — 2026-11-10

The owner authorized the single Full deciding policy experiment before dispatch. Vehicle
[PR #976](https://github.com/theEscape2207/StratChess/pull/976) remains a draft at
`cae205d82db6acb7f3963761e6c47f2b455e9a99`, with its required CI green. No approved D1-D4
decision changed during implementation or result adjudication.

| Input / identity | Verified value |
|---|---|
| Workflow / role | `strength.yml`, deciding policy experiment |
| Dispatch branch | `worktree-lmp-depth2-store-upper-vehicle`, held at the vehicle SHA |
| Candidate and reference revisions | Both `cae205d82db6acb7f3963761e6c47f2b455e9a99` |
| Candidate option | `LateMovePruningDepth2StoreUpper=true` |
| Reference option | `LateMovePruningDepth2StoreUpper=false` |
| Shared binary SHA-256 | `4628086df9b4de4a81c5d0d4391dafe5002e6e5ebf70e8d759d38fe359feb08b` |
| Build | GCC 15, `ubuntu-26.04`, Release; no extra CMake definitions |
| Budget / stopping rule | `games=26568`, `shards=18`, `sprt=true`, SPRT [0, 3], alpha=beta=0.05 |
| Playing conditions | Both TCs `10+0.1`, Threads=1, concurrency=3 |
| Book / offset | `UHO_4060_v3.epd`, opening offset 0 |
| Additional arms / calibration | None / false |

Comparison preflight passed and resolved only the intended true/false option difference;
source/build inputs and staged binary bytes were equal. This is advertisement/domain validation,
not independent runtime option readback. The explicit reference-default notice was expected.
All 36 played wave match jobs succeeded; repair was unnecessary and waves 3-4 were skipped.
The joined logs contain no fatal-game markers.

| Wave | Cumulative games | Elo +/- error | LLR | Decision |
|---|---|---|---|---|
| 1 | 6660 | +8.14 +/- 6.07 | +2.08 | continue |
| 2 | 13284 | +6.30 +/- 4.31 (at stop) | +2.98 | H1 |

Pooled Ptnml [447, 1533, 2535, 1586, 541], 6,642 pairs. Re-pooling all 18 joined logs with
`pool_pentanomial.py --expect-shards 18 --expect-pairs-per-shard 369` reproduced the report,
including both logged revisions `cae205d`. The accepted H1 is the D4 advance decision; the
optionally stopped estimate is not a fixed-size gain estimate and does not prove IIR avoidance
caused the benefit. The policy still exposes the selective-bound tradeoff in D2.

Implementation evidence is recorded in #976: default-off equivalence against pinned main
`5d9a0e50` (125 compared lines, six positions, depth 12, Threads=1), focused replaced-entry and
independent floor/store falsifications, full Engine-tier gates and search review. The separate
12-round flag-off vehicle/main bench at depth 14 reported an as-built -0.32% nps, 95% t-interval
[-0.57%, -0.07%], with placement unruled. Both shipping clang-cl builds searched identical trees;
this is not a confirmed intrinsic cost and is excluded by the same-commit lab comparison.
Metadata/report/CSVs remain at
`StratChessSupport/Bench/972-lmp-depth2-store-upper-20261110-depth14/metadata.json`.
The initial depth-13 warm-up failed the 200 ms floor (`tactical-4`: 193 ms), produced no verdict,
and remains in the sibling directory without `-depth14`.

D4 disposition: advance to fresh production review, with no automatic default flip. The evidence
lands separately from fresh main before #976 is closed; #976 itself never merges. Production code
selection, its review and any shipping decision belong to a fresh follow-up. The reserved
`QuietSeePruningMargin=30` experiment remains separate; no second lab run or tuning arm is implied.

Evidence links:

- [Completed run](https://github.com/theEscape2207/StratChess/actions/runs/42849368403)
- [Final report](https://github.com/theEscape2207/StratChess/pull/976#issuecomment-6775041009)
- [Comparison and opening book](https://github.com/theEscape2207/StratChess/actions/runs/42849368403/artifacts/13778464696)
- [Joined match logs and PGNs](https://github.com/theEscape2207/StratChess/actions/runs/42849368403/artifacts/13788373337)
- [Pinned toolkit](https://github.com/theEscape2207/StratChess/actions/runs/42849368403/artifacts/13778314600)

The durable result is in `Measurements/ci-per-change.md`, linked from #972 and #940. This retained
copy preserves the final approved proposal and outcome in Git; the local final record is
`StratChessSupport/DesignRecords/972 - lmp-depth2-store-upper.md`, beside its existing reviews
and progress ledger. Source and contract changes remain solely in the unmerged vehicle until
selected by the production follow-up. A production changelog headline remains due there.
