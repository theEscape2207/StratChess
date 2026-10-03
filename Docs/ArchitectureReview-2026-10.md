# Architectural assessment — October 2026

**Reviewed baseline:** `e98c4dd9b551c0c536b701a2922da32d08208896` (`main`, 2026-10-03).
**Status:** assessment revised after review and owner agreement on priorities, 2026-10-03.
Proposed implementation activities still require their own design and validation.
**Companion:** [current architecture map](Architecture.md).

## 1. Assessment

The highest-value architectural objective is to shorten the path from a plausible strength idea to
a trustworthy decision. The project already has the expensive part: a working paired-game lab,
useful correctness checks, diagnostics and explicit engine-state contracts. Its main opportunities
are better experiment identity and decision discipline, plus internal modules that make changes
easier to reason about and test.

The code concentration concern is justified, but `AIPerplex` and `Evaluator` need different
treatment. `AIPerplex` combines several kinds of responsibility and exposes much of its internal
state to one large test fixture. Evaluation already has a small external interface, pure term
functions and shared intermediate calculations. Its first improvement should make that existing
structure easier to navigate and extend without breaking its useful computation sharing.

The assessment does not establish that a particular C++ module is today's runtime bottleneck.
It identifies verified structural friction, experiment-integrity gaps and opportunities to probe.
There is no defensible Elo forecast for a refactor based on this inspection.

### Owner priorities used

- Elo is the outcome; nps and fixed-depth timing provide supporting evidence.
- Keep using existing hardware and currently available free services. No new spending is assumed.
- Reversible changes are welcome when their value exceeds implementation and validation cost.
- Return to short cycles after this broader assessment.
- Singular-extension tuning and lab runs belong to a parallel session.
  [PR #703](https://github.com/theEscape2207/StratChess/pull/703) has merged; subsequent lab work
  should use its multi-arm implementation. This review does not dispatch or alter those runs.

## 2. Evidence and limits

Read the current search/evaluation interfaces and execution paths, state and timing modules,
representative fixtures, build topology, measurement scripts and workflow, documentation and recent
source history. Checked issue #95's history and completed run, plus existing work on TT costs and
evaluation tuning. PR #703 was read as background, not subjected to another PR review.

Executed the pooling script's seven arithmetic self-test cases and the option validator's eighteen
checks: all passed. Inspected the completed singular run's job timings and compiler logs. No engine
build, fresh performance measurement, new match or game-corpus analysis was performed. Engine
correctness, race freedom and test coverage are not newly certified by this report.

Evidence labels below:

- **Verified:** directly supported by inspected code, logs or recorded results.
- **Inference:** a plausible effect of that structure, not a measured cost.
- **Proposal:** a candidate intervention with a falsifiable acceptance condition.

The initial evidence uses the baseline above. Review follow-up checked the merged lab capability
at `7b6c6b6`; documentation corrections and agreed process guidance are included with this report.
Preflight, pooling and engine-module proposals remain unimplemented.

The [August review](ArchitectureReview-2026-08.md) remains historical evidence. Its addendum explains
why generic library/interface recommendations were revised. Current code has already removed the
legacy search inheritance and added concrete search ownership; those old findings must not be
reissued as current defects.

## 3. What singular extensions teach us

The case is stronger than a generic warning that benchmarks are imperfect:

1. The mechanism landed in September with exclusion-search tests and a disabled shipping path.
2. Discussion explicitly said timed Elo was the criterion, but substantial fixed-depth cost still
   motivated cost attribution and configuration work before measurement.
3. On October 2, the current-tree bench recorded roughly **+50.1% fixed-depth wall time** and
   **+49.9% searched edges**, with approximately flat time per edge.
4. The first lab dispatch unintentionally enabled the feature on both sides because the compile
   setting also changed the runtime default. It was cancelled and replaced; its data was not used.
5. The corrected comparison measured **+22.90 ± 3.60 Elo**, 19,980 games at 10+0.1, same source and
   build configuration, feature explicitly on versus off. Default-on shipped in #701.

Sources: [issue #95 and dated comments](https://github.com/theEscape2207/StratChess/issues/95),
[completed run 37020636422](https://github.com/theEscape2207/StratChess/actions/runs/37020636422),
[per-change ledger](../Measurements/ci-per-change.md).

**The process failure was allowing a diagnostic to delay the deciding experiment.** More nodes at
the same nominal depth cannot establish whether selective extra search improves timed play. A
timed Elo result already includes the cost of the feature. Subtracting a separately estimated speed
penalty from that result would count cost twice.

The positive result also does not reveal the feature's hypothetical "gross move-quality Elo" or
prove that its timed cost disappeared. Neither quantity was isolated. It establishes a net gain
under the match conditions. Likewise, it does not prove the September implementation on its older
baseline would have gained 22.9 Elo; other search changes landed in the interval.

**Agreed guidance:** when a behaviour-changing candidate meets correctness requirements and the
comparison is valid, prioritise its deciding lab run within the agreed budget. Use local work to
answer a named question with bounded effort. Optional cost attribution can run alongside or after
the strength experiment; it is not a prerequisite. A decision to defer a match should say "not
measured because of capacity/budget/priority", rather than imply the feature has failed on strength.
This guidance is now recorded in the [measurement skill](../.claude/skills/measure-strength/SKILL.md).

This changes how existing tools are used. It does not require inventing another Elo predictor.

## 4. Trustworthy strength results

### 4.1 Experiment identity is distributed across several artifacts

**Verified.** The workflow resolves source revisions, applies build defines to both sides, validates
requested options against advertised names/types/ranges, stages one toolkit, runs disjoint opening
slices and pools pentanomial counts. These are substantial strengths.

However, [validate_uci_options.py](../.github/scripts/validate_uci_options.py) queries `uci` and checks
the requested text. It does not apply options and confirm their effective values, compare the two
resolved configurations, or even query a side with no overrides. Equality with a default is a
warning. The two sides are checked independently. The #95 cancelled null test is a real example
of what remains possible despite that validation.

The report lists requested options, a fixed toolchain label and source references. The toolkit has
one-day retention. Build logs carry compiler details, but there is no single durable structured
record joining binaries, effective defaults/overrides, conditions, opening ranges and results.

**Proposal: compare resolved configurations in preflight.** Query both binaries even when one side
has no overrides, combine advertised defaults with validated overrides, and show the difference
being measured for each candidate arm against the reference. Compare source/build identity and
playing conditions too: an intentional time-control difference is not a null comparison. Refuse
an unintended identical comparison before shards start; allow explicitly declared calibration.
Retain the resolved comparison in the existing run output and link it from the ledger.

Same SHA is not necessarily a null test; different override strings are not necessarily different
effective configurations. Advertised defaults plus overrides describe the intended effective
configuration, not proof that every historical binary applied it. Use acknowledgments where
available and state that verification limit when they are absent.

**Acceptance:** a fixture replaying #95's default-on setup fails before dispatching shards;
different effective options and intentional time-control comparisons remain possible; an explicit
calibration remains possible; every arm has a retained, readable resolved comparison.

Defer a comprehensive structured manifest until a concrete reconstruction or automation need
justifies it. A second failed experiment is not a prerequisite for revisiting that choice.

### 4.2 Completion checks can become independent of runner behaviour

**Verified.** Aggregation requires successful match jobs and the expected number of log files.
[pool_pentanomial.py](../.github/scripts/pool_pentanomial.py) takes the last count line from each
log. It does not receive the expected pair count per shard or prove that each last line represents
the planned number of games. The opening check compares each shard's first FEN; the launch
arithmetic provides the main guarantee that the full ranges are disjoint.

This is a missing independent assertion, not evidence that a published batch was incomplete. The
singular run's recorded 9,990 pairs match the planned total.

**Proposal:** check planned versus actual pairs per shard/arm, shard identity and assigned opening
range as part of result ingestion. Include truncated-but-parseable logs in the parser tests. This
is a small strengthening of the current lab. Combine it with the preflight work in one
experiment-integrity slice using the merged multi-arm routing, with planned counts per shard and
separate pooling per arm. Complete fixtures must reproduce existing results; a parseable truncated
shard must fail independently of the runner's exit status.

### 4.3 Improve decisions per lab hour

**Verified.** In run 37020636422, build took about 2 minutes 48 seconds; setup-to-final aggregation
took about 3 hours 14 minutes. Match jobs occupied almost all that time. Even eliminating the build
entirely would have changed that run's latency by less than 2%. Compiler caching is useful for
development throughput; it is not the main answer to the three-hour Elo wait.

**Already available:** PR #703's multiple arms and opening offset. Use that implementation.
Screening spreads a fixed game budget over more questions; it
does not create more precision. For illustration, splitting a 19,980-game batch into three equal
arms would widen a ±3.6 interval to roughly ±6.2 per arm if pair variance stays similar. This follows
the existing pooling formula's square-root dependence and is not a guaranteed future error bar.

**Agreed recording convention:** use existing issue/design notes to declare the question, smallest
effect worth resolving, diagnostic/screen/confirmation role, stopping rule and budget before
dispatch. Link that declaration from the appropriate ledger's row detail and retain every tried
arm, including weak/inconclusive ones. [Measurements/README](../Measurements/README.md) now carries
the convention; no new artifact type is needed. A screen favouring one setting is selection
evidence; a fresh confirmation estimates that selected setting's gain. Fresh opening offsets help
avoid reusing selection data, though they do not make conclusions universal across books or opponents.

A later study could compare fixed small batches with a properly designed sequential test for
large-effect screening. It needs calibrated error rates and a central stopping design across
shards. It is not a quick YAML optimization. Repeatedly inspecting an ordinary fixed-batch interval
and stopping when it looks positive is not the same experiment.

### 4.4 Define the strength claim's operating conditions

Current lab evidence primarily concerns GCC/Linux, `Threads=1`, 10+0.1 and the selected opening book
against a specific reference. Shipping builds use clang-cl/Windows. Matching toolchains within a
run prevents one confound; it does not prove that a performance-sensitive change has the same
effect across platforms, thread counts or time controls.

Use existing conditions as the working target. For a material platform-sensitive optimization or
a change to time/SMP behaviour, name the additional confirmation question explicitly. Do not demand
an enormous cross-product for every small change. Multi-arm follow-ups are already using the lab;
additional measurement scheduling remains the owner's choice.

## 5. Search structure and testability

### 5.1 AIPerplex is useful externally and crowded internally

**Verified.** `Search(root, limits, observer) -> SearchResult` is a useful external interface:
caller-owned input, owned worker state, per-call limits and returned results. `SearchPlayer` adapts
it to Game without restoring the old inheritance coupling. The external interface should survive
an internal restructure.

Inside [AIPerplex.cpp](../StratEngine/AIPerplex.cpp), 2,101 lines include:

- launch/stop/join and the immediate-stop handshake;
- game reset, tuning, TT allocation and validity;
- helper scheduling and result aggregation;
- iterative deepening, aspiration and interrupted-result acceptance;
- PVS, quiescence, ordering and pruning interactions;
- telemetry and logging.

The PVS function spans roughly 600 lines including substantial contract comments. Line count is
orientation evidence, not a complexity score. Its real difficulty is the number of state and order
constraints that every new heuristic must preserve.

**Inference.** A change to iteration acceptance or cancellation requires readers to filter through
unrelated recursive-search machinery; different experiments also edit the same files. The 691-line
[SearchTestFixture](../StratChessTests/SearchTestFixture.h) is evidence of a broad internal test
surface: it writes tuning fields and worker state directly, configures draw context, seeds TT/PV
state and calls private recursive functions. These tests can be valuable, but the fixture is also
carrying knowledge of initialization and ownership that production normally hides.

### 5.2 Recommended first structural probe: iteration decisions

Before selecting the pilot, identify a concrete upcoming change or recurring test/review friction
that it should remove. A time-management feature is one useful trigger, but demonstrated fixture
or ownership complexity can justify a standalone behaviour-preserving extraction. Keep heuristic
changes separate so their effects remain attributable. If no useful scenario emerges, defer the pilot.

Extract the coherent decision around completed/interrupted iterations and soft-limit continuation
into a small internal module. Its inputs should be the observations and retained result state it
actually needs; its output should explain accept/reject/continue and the next retained state.
Keep search execution, clock acquisition and protocol emission in their current owners initially.

Why this first: it has a natural input/output seam, existing focused cases, and runs per iteration
rather than per node. The pilot can prove whether explicit dependencies reduce fixture setup and
review effort before committing to a larger split. Extracting one trivial predicate would provide
little value; the module must own a complete decision and its invariants.

**Acceptance:** existing decisions remain identical for representative and edge-case input tables;
tests exercise the module without constructing an engine/TT; public search tests still cover result
selection after abort; finite-corpus search equivalence, a paired shipping-build benchmark and
existing runtime gates pass. Node identity does not rule out speed changes from code placement or
optimizer choices. Report what the pilot removes from the broad fixture. If the new interface needs nearly all of AIPerplex,
the seam is wrong and the pilot should be revised or discarded.

### 5.3 Revisit lifecycle and the recursive kernel after pilot evidence

The launch handshake and owned thread are another coherent responsibility. An internal lifecycle
module could own stop-before-start, join-before-rearm and completion ordering, using the existing
deterministic launch barrier tests. The exact home of the stop latch needs design: duplicating it
between this module and SearchControl would make the interface worse.

A recursive search module is a larger later candidate. First write down frame inputs and the
meaning of exits: completed bound, terminal result, exclusion result, selective fail-low and abort.
Trace those through make/unmake and persistent writes. Several of these currently travel as an
`int` plus surrounding state. This does not by itself justify a tagged return type on every node;
the first deliverable is an enforceable contract and focused tests, with code shape chosen after
the cost is understood.

Keep the hot loop's ordering visible. A virtual object for each heuristic, general dependency
injection framework, or wholesale micro-module split has no demonstrated consumer benefit here.
Splitting source files may help navigation, but only ownership and smaller interfaces improve
testability. Separate compiled libraries can be revisited if a concrete need appears; the old LTO
and build-topology decision is context, not an irreversible prohibition.

## 6. Evaluation structure and experiment support

**Verified.** [Eval.cpp](../StratEngine/Eval.cpp) is 1,267 lines and
[Eval.h](../StratEngine/Eval.h) is 903. Much of the header is weights, tables, explanatory comments
and helpers. The implementation already builds an `EvalContext`, shares attack generation and
evaluates pure terms. `Evaluate` and `Breakdown` are two meaningful consumers, and blended-term
tests already use the public breakdown. This is a useful foundation, not an evaluator that must
first be made testable from scratch.

Three distinct opportunities, with different triggers:

1. **Navigation and ownership.** Group private parameter data, context/feature preparation,
   endgame classification and term families. Preserve the small external interface. An initial
   endgame-classification extraction or one term-family extraction can establish the pattern when
   an upcoming change or repeated fixture/navigation friction supplies a concrete scenario. Treat
   this as an alternative to the first search pilot, not a second mandatory extraction.
2. **Explicit term registration.** `EvalTerm`/`EVAL_TERMS` name breakdown rows, but
   `RawWhitePov` and `Breakdown` separately spell out term calls. Adding a term requires maintaining
   both. Existing population and row/total consistency tests reduce the correctness urgency;
   they cover their exercised cases, rather than proving all future terms are wired correctly.
   Defer registration machinery until actual maintenance friction warrants it. If revisited,
   preserve the fused king-pawn-cover calculation and exact blending order. Term count alone is
   not a useful trigger, and a uniform interface must not duplicate context or attack generation.
3. **Parameter experiments.** Weights are mostly compile-time data rather than an explicit offline
   parameter model. Existing [#117](https://github.com/theEscape2207/StratChess/issues/117) covers
   evaluation tuning. Park parameter inventory/export until that work has a concrete consumer and
   is ready to use it. `Breakdown` alone is not a linear feature export. When needed, start with one
   family and account for blending, scaling, gating and integer rounding before expanding.

**Acceptance for a structural pilot:** corpus scores and breakdown totals remain exact, colour
symmetry and independent term expectations remain covered, and the change requires less shared
fixture knowledge. Measure shipping-code cost appropriately; an exact-score refactor can still
alter code placement or optimizer choices. Parameter changes and structural changes need separate
comparisons so the source of any strength difference remains identifiable.

The evaluator also has search-specific draw-score state, written before workers start. An immutable
per-search evaluation context is a possible future representation, especially if a second real
evaluator arrives. There is no evidence here that replacing the current guarded arrangement is
urgent or free.

## 7. Tests, vocabulary and durable knowledge

### Preserve the distinction between contract tests and chess preferences

The repository already has meaningful tests: legality/perft, state restoration, TT behaviour,
abort handling, score symmetry, term semantics and protocol lifecycle. The goal is easier access
to the correct test surface, not increasing the count of tests or deleting all friendship.

Use public search tests for observable behaviour and real internal module interfaces for narrow
decisions. Keep selected TT/abort interaction tests that need deep state, but avoid teaching every
new test the whole search initialization sequence. Helpers that reproduce production policy need
independent expectations so they cannot merely reproduce the same error.

Some eval assertions use private weight constants, while others pin literal values or independently
computed outcomes. Classify these as rule/invariant tests, formula-wiring tests or current-parameter
characterization. That makes future tuning intentionally update the right expectations without
weakening actual correctness checks. A tactical preference changing is a question to investigate;
it is not automatically an Elo regression.

The test binary includes profile instrumentation and differs from production. Retain targeted
production-binary and abort/SMP checks around any extraction; a fixed-depth single-thread equivalence
result does not cover those paths.

### Clarify vocabulary without an immediate rename programme

- [CONTEXT](../CONTEXT.md) defines a quiet move as one that removes nothing.
  It already permits treating promotions separately where needed. `ThreadData::is_quiet` uses that
  narrower meaning: neither capture nor promotion. The glossary now names it **history-eligible
  quiet**. This clarifies usage without renaming call sites.
- `MoveGenerator::ComputeLegalMoves` is documented in its header as pseudo-legal generation.
  Callers must use `DoMove` to filter self-check. The misleading name has a reading cost, but its
  header states the contract. Defer a mechanical rename until concrete caller confusion or related
  interface work makes the value exceed the churn.

These are findings about language, not newly discovered legality bugs.

### Documentation needs an explicit job for each artifact

The initial inspection found the engine guide saying Move equality ignores flags and listing
implemented search features as future work. Those passages are now corrected, and the guide links
to the dedicated architecture map. CI documentation and the recording convention now distinguish
same-source option comparisons from null calibration. These cheap corrections were completed
independently of the deferred naming work. Historical review/issue bodies still need to be read in
their original context; current contracts and the map provide the entry points.

Recommended division:

| Artifact | Job |
|---|---|
| `CONTEXT.md` | Resolved domain vocabulary only. |
| `Docs/Architecture.md` | Responsibilities, ownership, execution and links to evidence. |
| `Docs/EngineContracts.md` | Non-obvious obligations that changes must preserve. |
| Dated reviews / measurement records | Historical observations, scope and uncertainty. |
| Task design documents | Alternatives, decisions and execution for a particular change. |
| Optional ADRs | Rationale for selected consequential trade-offs that existing records do not make durable. |

No historical ADR backfill or mandatory ADR layer is proposed. When a consequential trade-off is
made, first retain its alternatives, evidence and revisit conditions in the task's design record.
A sparse ADR can preserve rationale that would otherwise become hard to find; link the relevant
map/contracts instead of copying their descriptions. Do not wait for rationale to be lost before
recording it. Routine reversible module/file organization can remain an ordinary design decision.

## 8. Candidate follow-up activities

Ordered by likely value for the stated goal, not by forecast Elo. Effort bands are initial scoping
judgements, excluding matches and unexpected findings; each implementation still needs the normal
repository design/review workflow.

| Order | Activity | Smallest useful slice | Evidence of success | Coordination / effort |
|---|---|---|---|---|
| Done in docs | Repair entry points and clarify measurement guidance | One architecture map, corrected guide/CI wording, glossary clarification, existing-ledger convention and lab-priority guidance | Readers can locate ownership/contracts; same-source comparisons and experiment roles are distinguished | No engine or lab implementation change |
| 1 | Strengthen experiment integrity | Compare resolved configurations per arm; retain the comparison; assert expected pairs and shard/arm routing | #95's unintended null and parseable truncated shards fail; complete fixtures reproduce results; intentional comparisons/calibrations remain possible | Use merged #703 routing; small-to-medium |
| 2 | Exercise the agreed decision convention | Apply the existing ledger/skill guidance to the next eligible experiment | Role and stopping rule recorded before dispatch; all arms retained; optional cost analysis does not delay the deciding run | Coordinate with active lab work; small |
| 3 | Pilot a search module | Iteration acceptance/continuation with explicit inputs/outputs, justified by a concrete feature or recurring friction | Focused tests avoid constructing AIPerplex; fixture dependencies shrink; abort coverage, equivalence and shipping-build bench remain sound | Separate from tuning; medium |
| Alternative to 3 | Pilot evaluation organization | One internal family with the existing score/breakdown interface, chosen for demonstrated friction | Exact corpus results and simpler fixture dependencies | Choose this instead if its scenario is stronger; medium |
| Later | Expand only after pilot evidence | Lifecycle ownership or a search-frame contract with a named consumer | A specific change becomes easier to implement/test | Pick one; medium-to-large |

For a pilot, record a short before/after account of fixture setup, dependencies and review difficulty.
No phase-by-phase time logging or new throughput tracking system is required. Comprehensive
manifests, term-registration machinery, mechanical renames and parameter export remain deferred
until the triggers described above justify them.

## 9. Opportunities to revisit after the first cycle

- **TT synchronization:** an existing measured-cost investigation in
  [#250](https://github.com/theEscape2207/StratChess/issues/250). The table already has a coherent
  interface. This review supplies no new lock-contention measurement and does not justify a rewrite.
- **Time management:** the iteration-decision pilot opens a useful experimental seam. Changes here
  need timed games; fixed-depth tests cannot establish their strength.
- **Evaluation fitting / NNUE:** possible future work under the existing spending constraint, with
  separate costs for data, fitting, integration and validation. A second evaluator could justify an
  abstraction; speculative future variation alone does not require it today.
- **Tablebases:** also a separate value/cost question including disk, downloads and probe integration.
  There is no storage or feasibility assessment in this review.
- **Sequential strength testing / lab scheduling:** potentially higher experiment throughput, but
  only after experiment identity and current multi-arm results are understood. Preserve explicit
  statistical and budget decisions rather than silently trading precision for a faster answer.

## 10. Suggested next conversation

The owner agreed to **one small experiment-integrity slice**, followed by **one bounded structural
pilot justified by demonstrated friction**, as the direction for successive short cycles. #703 has
merged; coordinate subsequent implementation and dispatch with the active lab work. The search
iteration seam is the leading pilot candidate, with an evaluation family as an alternative if it
has the stronger scenario. Neither requires new hardware or paid services. Judge each by the
obstacle it removes before expanding the programme.

Open decisions to bring back:

1. What minimum gain is worth confirming for a simple change versus a costly new subsystem?
2. Which upcoming change or recurring fixture/review difficulty provides the first pilot's concrete
   scenario: iteration decisions or an evaluation family?
3. Which strength conditions should define success beyond today's lab default, if any?

## Source anchors

All code anchors refer to the reviewed revision, not a moving branch:

- [Search launch and root orchestration](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/StratEngine/AIPerplex.cpp#L149-L481)
- [Iterative deepening](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/StratEngine/AIPerplex.cpp#L482-L613)
- [PVS and its state/write ordering](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/StratEngine/AIPerplex.cpp#L661-L1267)
- [Evaluation aggregation and diagnostic breakdown](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/StratEngine/Eval.cpp#L1137-L1267)
- [Search tuning catalogue](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/StratEngine/SearchTuning.def)
- [Production/test target configuration](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/CMakeLists.txt#L359-L435)
- [Strength workflow](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/.github/workflows/strength.yml)
- [Option preflight](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/.github/scripts/validate_uci_options.py)
- [Paired result pooling](https://github.com/theEscape2207/StratChess/blob/e98c4dd9b551c0c536b701a2922da32d08208896/.github/scripts/pool_pentanomial.py)
