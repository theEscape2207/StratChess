# Depth-2 LMP UPPER-store production handoff — Design

**Issue:** #940; completes the #972 experimental handoff.

## Goal

Ship the selected depth-2 LMP store policy after the accepted H1 recorded by #977,
without inheriting the disposable vehicle's branch or starting another experiment.

## Review focus

- D1: retain the measured selective policy and its searched-move, effective-depth and abort contracts.
- D2–D3: default-on option binding and the separate, qualified flag-off bench cost.

## Scope

Select the reviewed search, fixture, tuning, UCI tests and contracts from vehicle
`cae205d82db6acb7f3963761e6c47f2b455e9a99` onto fresh main `a9b31f72`.
Update shipped defaults, test expectations, contracts and the production changelog.
No pruning thresholds, consumers, TT layout, extra measurement or other experiment changes.

## Decisions

### D1: Preserve the reviewed policy

Keep the vehicle's common-store route: completed effective-depth-2 LMP skipped fail-lows
floor at entry alpha and store MAIN/UPPER/ALL_NODE with a searched move. Searched cutoffs
retain LOWER, exclusion and aborted frames retain no parent results, and static-eval
metadata uses the existing common-store argument. Depth-3 and no-skip behavior stay intact.
These are selective bounds: shallower main search and quiescence can consume them without
matching skips; the move can prevent IIR and staged generation. A hash-move-only policy
would change the measured hypothesis and is rejected.

### D2: Retain the tuning option, default true

Keep `LateMovePruningDepth2StoreUpper` as a normal boolean tuning option, JSON binding
`late_move_pruning_depth2_store_upper`, default true. False preserves the old depth-2
no-store policy. Retaining it preserves the reviewed paired controls and explicit rollback,
consistent with the adjacent depth-3 switch; removing it saves a flag read but changes the
measured implementation and discards those controls. Check values remain true/false and
changing the policy clears the TT through existing SetTuning behavior.

### D3: Reuse the accepted decision with its cost qualification

Run `42849368403` accepted H1 at 13,284 games, LLR +2.98; +6.30 +/- 4.31 is at stop,
not a fixed-size gain estimate. The retained #972 design and measurement ledger on main
preserve the checked inputs and artifacts. The separate default-off vehicle/main bench
reported -0.32% nps, 95% t-interval [-0.57%, -0.07%], placement unruled. This is an as-built
cost, not confirmed intrinsic overhead, and is excluded by the same-commit lab comparison.
Accept that qualified small overhead in exchange for the accepted policy decision; do not
claim a new production-binary speed result or that IIR avoidance explains the gain.

## Assumptions I cannot verify from the code

The accepted lab validity and bench result are established by #977's evidence record and
#976's metadata, not remeasured here. Verify their exact qualifiers against the retained
record and ledger before review. No new runtime or Elo magnitude is assumed.

## Invariants

D1's tested boundaries hold in both option states; the production default activates the
same path as the measured true arm. Existing consumers and unrelated defaults are unchanged.
The disposable #976 never merges. #972 closes after a linked production handoff; #976 closes
only after the selected code is preserved in the production PR; #940 closes on that PR's merge.

## Validation

Engine tier: focused search/LMP/TT/quiet-SEE/tuning/UCI tests, full pre-commit and pre-PR gates,
full Standards/Spec review and search-specialist review. Adapt explicit false controls and
two-way JSON/UCI checks so a default-true value cannot make them vacuous. Compare the selected
search code directly with the pinned vehicle. No Elo run is needed: D3 reuses the accepted H1;
no new bench or equivalence claim is made for the changed default.

## Cost

50–200 selected implementation/test/doc lines across about eight files; Engine tier, one
advertised option. Existing policy/design evidence is reused; full two-axis and search review
remain required. No optional experiments or profiling.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 selective store, consumers and lifecycle contracts | `pvs()` comments and `Docs/EngineContracts.md` |
| D2 retained option and default | `SearchTuning.def`, two-way tests and production changelog |
| D3 decision and qualified cost acceptance | Production changelog and PR body; existing ledger stays authoritative |
| Handoff and vehicle retirement | Linked comments on #972, #940 and #976 |

Self-review completed against fresh main and pinned vehicle: no unresolved choices or
placeholders; all invariants have source/test/review or lifecycle evidence above. This is a
production addendum to the converged experimental design; its original retained record is unchanged.
