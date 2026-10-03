# Iteration acceptance and continuation pilot — Design

**Issue:** #706

**Baseline:** `origin/main` `ec142137517855d62a6eba1318d065bf2a9ed651` (includes #710)

**Status:** selection/design checkpoint; production edits await owner approval and cross-agent review.

## Goal

Make the complete main-thread iteration decision testable with values, without constructing the
engine. Today six quality tests and two mate-stop tests in `SearchIterationTests.cpp` construct
`AIPerlexTestFixture`: a Board, `AIPerplex`, a 1 MiB TT, evaluator and worker state, including a
copy into `td_.board`. They reach private helpers through friendship and aliases. The quality
tests also supply derived `move_changed` and `completion_ratio` themselves, so they do not test
their production calculation. Meanwhile the one-extra-depth soft-limit rule has no focused test
in that file. This is a concrete fixture and review obstacle, independent of a new strength feature.

## Review focus

- **D2/D3:** do the two transitions preserve observer-before-soft-limit ordering and all retained
  state updates, including the interrupted branch's different `last_iteration_move` update?
- **D1/D4:** does moving the complete policy remove enough engine/fixture knowledge to earn this
  interface? Reject the pilot if tests still require engine, Board or TT construction.
- **D5/Assumption A1:** can the acceptance gates distinguish changed behaviour from code-placement
  cost without starting a strength experiment?

## Scope

**This change will:** extract acceptance, retained-result updates and soft-limit/mate continuation;
replace the eight private-helper policy tests with tests through the production interface; add
sequence and threshold cases; retain integration coverage; update ownership and test documentation.

**This change will not:** change heuristics, tuning values or configuration exposure; extract
evaluation, lifecycle, recursive search, aspiration, emergency move selection, clock acquisition,
PV storage, output or SMP orchestration; introduce virtual interfaces, new dependencies or a
general injection framework; implement #103, #117 or the broader split in #405.

## Decisions

### D1: A value-only internal module owns the full policy

Add `StratEngine/IterationPolicy.h/.cpp`, using the existing `Engine` namespace convention for
pure calculations. It owns decision rules and state transitions. `AIPerplex` remains the driver:
it executes search, collects raw observations, applies returned state and performs side effects.
Reject a mere relocation of `assess_iteration_quality()` or `should_stop_early()`: that leaves
derived-input calculation, retained-state updates and the untested continuation decision outside
the seam. Reject a policy taking `AIPerplex`, `ThreadData`, `SearchControl` or full `SearchTuning`.

The types below are values, not an externally supported search interface:

| Type | Fields / meaning |
|---|---|
| `IterationSample` | `int depth`, `Move current_move`, `int current_score`, `int64_t nodes_searched`, `int pv_length`, `bool interrupted`. Nodes are the main-tree delta for this iteration, excluding quiescence, as today. |
| `IterationThresholds` | `int64_t min_nodes_threshold`, `double min_completion_ratio`, `double min_pv_ratio`, explicitly supplied from the three existing tuning fields. No second catalogue of defaults. |
| `IterationState` | Existing `SearchState` fields and initial values: empty `best_move`, zero `best_score`, zero `depth_completed`, zero `nodes_at_completed_depth`, empty `last_iteration_move`, true `search_was_stable`; add false `extra_depth_used`. |
| `IterationMetrics` | Existing metrics fields; derive `move_changed`, `score_delta` and `completion_ratio` from sample and previous state. Used by policy and existing logging. |
| `IterationAssessment` | `decision` (`IterationDisposition::COMPLETED`, `ACCEPTED_INTERRUPTED`, `REJECTED`), existing five-valued `RejectionReason`, `metrics`, and `next_state`. |
| `IterationContinuation` | `stop_reason` (`NONE`, `SOFT_LIMIT`, `MATE`) and `next_state`. `NONE` means the driver may start another depth if its depth cap allows it. |

Only these two operations implement decisions; both are allocation-free and `noexcept`:

```cpp
IterationAssessment assess_iteration(const IterationSample& sample,
                                     const IterationState& previous,
                                     const IterationThresholds& thresholds) noexcept;
IterationContinuation continue_iteration(const IterationMetrics& completed,
                                         const IterationState& accepted,
                                         bool soft_limit_reached) noexcept;
```

`continue_iteration` is called once only for a completed acceptance, with that assessment's
metrics/state. Document and Debug-assert `!completed.interrupted`; no runtime validation layer.
The driver holds one local `IterationState` per search, explicitly passes it and assigns returned
state. No policy object, hidden clock, callback, pending state or inter-search cache is needed.
Emergency handling may subsequently amend this local state as today.

### D2: Preserve acceptance rules, arithmetic and state distinctions

`assess_iteration` derives all three metrics before making a decision, using the existing exact
Move equality, subtraction and double division. The denominator is previous main-tree nodes;
when it is not positive the ratio is `1.0`. A non-interrupted iteration is accepted regardless
of quality thresholds, even with an empty move. Only interrupted observations apply these ordered
checks: empty move or nodes below threshold; previous depth and positive denominator with ratio
below threshold; PV below `max(1, int(depth * min_pv_ratio))` with a previous depth; changed move
with a previous depth. A drawn score has no special rejection rule.

Either acceptance copies move, score, depth, main-tree node delta and `!move_changed` stability
into retained state. Only completed acceptance updates `last_iteration_move`. Rejection preserves
every retained field. Assessment never changes `extra_depth_used`. Interrupted acceptance and
rejection stop without a continuation call; rejection emits no iteration observer snapshot.

### D3: Preserve the post-observer continuation transition

The production sequence stays: gather sample (including one `StopRequested()` observation),
assess, log diagnostic metrics, apply accepted state, log acceptance/completion, emit observer,
then acquire `ShouldStopIteration()` and call `continue_iteration` for completed acceptance only.
Computing a pure assessment before diagnostic logging introduces no side effect or clock read;
diagnostic logging still precedes state application and acceptance/rejection output.

If the soft limit is reached and either the move is unchanged or the extension was already used,
return `SOFT_LIMIT` without checking mate. Otherwise a reached soft limit consumes the one extension,
then `abs(score) >= GameValues::Mate_Threshold` returns `MATE`; otherwise return `NONE`. Consuming
the extension even when the same iteration finds mate preserves existing ordering. Neither depth
nor PV length is an early-stop signal. `AIPerplex` emits the existing mate log only for `MATE`.
The depth cap remains the loop's owner, and helper loops do not use this policy.

Reject one decision with an early soft-limit snapshot: the callback can consume time or call
`Stop()`, and today `ShouldStopIteration()` runs after it. Do not add a second interruption check
or a new stop-before-next-depth check as part of this refactor.

### D4: Replace policy fixture access, retain production coverage

Move the six assess and two mate-stop cases into `StratChessTests/IterationPolicyTests.cpp`,
including `IterationPolicy.h` and Catch2 directly. Use encoded Move values, not `AnyLegalMove()`;
the module compares encodings, while legality remains covered by public search/PV tests. Build
previous state by chaining assessments for sequence cases, rather than fabricating derived metrics.
Remove fixture `assess`, `stop_early`, `Metrics` and `RejectionReason` aliases and obsolete private
helpers/types. Keep the fixture's state alias for emergency tests, pointing at `IterationState`.

Before: the drawn-score case builds engine/TT/Board and supplies nine metric fields plus prior
state, including a manually selected ratio and changed flag. After: feed a completed move/score
sample, then an interrupted sample with the same encoded move and score zero; assert interrupted
acceptance and returned state. Change only the second move to assert `MOVE_CHANGED`. The same
test crosses the interface production uses and derives the comparison/ratio inside the module.

Record actual before/after setup and dependencies in the issue/PR. Acceptance is eight migrated
tests requiring no engine, Board, TT, clock or friend access, plus direct sequence coverage for
continuation. Do not claim a measured reduction in review time or suite time. Remove stale helper
coverage comments in the touched test file and consolidate the duplicated acceptance-state writes
inside the module; leave unrelated fixture/pruning cleanup alone.

### D5: One behaviour-preserving implementation PR after review

The design checkpoint is Docs tier. Implementation is Engine tier. No Elo match is required:
this pilot claims improved testability, no direct strength gain and no changed search behaviour.
Finite-corpus equivalence alone cannot prove all abort schedules or SMP, so keep focused public
result-selection/lifecycle coverage and existing SMP checks as independent gates. A shipping-build
bench is mandatory even though no per-node work is added.

## Assumptions I cannot verify from the code

**A1:** compiled function placement/optimizer choices leave shipping nps acceptable. Not verified;
settle after implementation with D5's paired benchmark, including placement attribution for a
negative result. There are no external tool/client behaviour assumptions needed for the policy.
Maintainability benefit is a falsifiable pilot outcome, not an assumed time-saving measurement.

## Invariants

- Acceptance/rejection order, numeric comparisons, ratio/PV rounding, exact move comparison and
  retained-state updates remain those in D2, including first-interrupted-depth behaviour.
- The one-extension rule, mate boundary and soft-limit precedence remain those in D3; policy state
  is local to one main-thread search and never shared with helpers or retained between calls.
- Observer/log order, PV ownership, recursive abort/unwind guards, emergency move selection,
  authoritative post-join totals and public search/lifecycle interfaces remain intact.

## Validation

- Direct table/sequence tests pin each rejection and precedence when multiple checks fail; equality
  at node/ratio/PV thresholds and the next failing value; absent/zero denominator; no prior depth;
  completed observations bypassing all quality checks; drawn-score acceptance; move flag differences;
  accepted/rejected retained state; positive/negative mate boundaries and scores just inside them.
- Continuation sequences cover changed/unchanged moves before and after the soft limit, one extension
  only, fresh-state reset, soft-limit precedence over mate and extension consumption on mate. Use
  explicit test thresholds; production maps the three tuning values once per main-thread search.
- Retain and run `[search]`, `[smp]`, `[uci]`, `[service_api]` and `[pv]`: public node-budget abort
  selection, stale aspiration PV rejection, emergency fallback, repetition PV depth, observer
  snapshots and immediate-stop/lifecycle/aggregation coverage. Add a deterministic public search
  case from the starting position at a depth-4 cap, with an observer calling `Stop()` after depth 1,
  asserting its accepted move/score/depth survive and no later depth is published; keep this distinct
  from value-only policy tests.
- Run required pre-commit and Engine-tier pre-PR validation (build, extended tests, tactical stability
  and self-play), the repository code review and `search-reviewer`. Keep Linux Debug/sanitizer CI
  and shipping Windows CI enabled through the repository workflow.
- Run `Compare-SearchEquivalence.ps1 -After <own-worktree shipping exe> -BaselineRef origin/main`:
  every compared iteration/info line and best move must match, stripping only time. Record the
  pinned baseline; revalidate if it changes during PR sync.
- Follow `measure-strength/reference/regression-check.md`: separately build the baseline using its
  own `build.ps1 main` and the same shipping clang-cl settings; run six baseline/candidate bench
  pairs on an idle machine, discard warm-up, require per-position nodes to match, report kept-pair
  nps delta mean/standard deviation/range. A spread at or above zero closes the cost gate. For a
  negative delta, relink with shared `/ORDER`, rerun and resolve any surviving slowdown before
  shipping. Do not use the equivalence-cache executable for speed or credit noise as a strength gain.

## Cost

Estimated, not measured: over 200 changed lines, roughly 10–12 source/test/doc files (new policy
pair and focused test; AIPerplex pair; existing iteration tests and shared fixture; architecture,
contracts, test map and changelog; plan harvest/removal). Engine tier with existing equivalence/bench instruments; no
new scripts, external services, hardware or paid strength runs. Cross-agent design review is routed
by the owner, using Terra or at most Sol-class per #706. Implementation code review has two axes
plus `search-reviewer`; actual token/time cost is unknown until run. No optional production additions.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 responsibility, inputs and main-thread lifetime | `Docs/Architecture.md` module/ownership tables |
| D2 arithmetic, state distinctions; D3 callback/clock ordering | `IterationPolicy.h` contract and `Docs/EngineContracts.md` |
| D4 direct test surface and retained integration protection | `Docs/TestDesign.md` and focused tests |
| Pilot before/after result; D5 equivalence and runtime outcome | #706 / PR body and `Docs/Changelog.md` |

Record any approved decision changed during implementation here with its reason. Once harvested,
remove the plan in the implementation PR if no inbound references or deliberate spec role remain;
otherwise apply `Docs/Workflow.md`'s named plan state. The design's committed revision preserves
the review record. Keep #706 `ready-for-human` until the owner approves this scenario, contract and
cost and the cross-agent review has no unresolved Blocking finding.
