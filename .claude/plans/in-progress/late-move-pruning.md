# Depth-two late move pruning — Design

**Issue:** [#547](https://github.com/theEscape2207/StratChess/issues/547)
**Status:** In progress — owner approved implementation 2026-09-14; the local screen and any strength run still need owner approval.
**Date:** 2026-09-14

## Goal

Reduce work spent searching late quiet moves at depth two without losing enough tactical accuracy
to offset the time saved. Current frontier futility operates at depth one and default late move
reductions begin at depth three. Late move pruning (LMP) would omit selected depth-two moves
entirely, so its risk is greater than a reduction that can trigger a full-depth re-search. The
shipping objective is measured positive playing strength; lower fixed-depth search cost is only
an admission criterion for a strength experiment.

The [triage](https://github.com/theEscape2207/StratChess/issues/547#issuecomment-5656385446)
records credible headroom and tactical risk. Its index-eight prototype reportedly saved 17.7%
median wall time at depth 12. Index twelve retained 18.8% trace-attributed node headroom with
lower endangered-cutoff rates, but was not a measured runtime or Elo result. These observations
justify one conservative candidate, not a threshold sweep.

## Scope

**This change will:**

- Introduce one independently gated candidate: parent depth exactly two, zero-based legal move
  index at least twelve, with the exemptions and persistence policy below.
- Preserve shipping behavior when compiled out, and provide isolated diagnostic counts.
- Establish correctness, tactical and fixed-depth evidence before a controller proposes a strength
  run to the owner.

**This change will not:**

- Expand to depth one or three, add evaluation/history/SEE conditions, tune the threshold, or
  change move ordering, LMR, frontier futility, null move or quiescence.
- Bundle #504, #529, #545, #399 or a #347 fix into the comparison.
- Treat deeper-main agreement as ground truth, or speed/node savings as an Elo estimate.

## Decisions

### D1: One fixed candidate, protected by explicit eligibility

Follow the singular-extension build pattern in `CMakeLists.txt`: the CMake option
`STRAT_LATE_MOVE_PRUNING` defaults OFF for the engine. When ON, it defines both
`STRAT_LATE_MOVE_PRUNING=1` and `STRAT_LATE_MOVE_PRUNING_DEFAULT_ON=1` on the engine target.
`SearchTuning::late_move_pruning_enabled` defaults from the latter macro, whose fallback is zero.
The compile gate leads the runtime predicate so shipping engine builds pay no new search work.

Always define `STRAT_LATE_MOVE_PRUNING=1` on `StratChessTests`, without the DEFAULT_ON macro.
Focused tests enable the runtime flag per case; ordinary tests retain their defaults. This puts
enabled coverage in existing required CI jobs without another build leg. Use compile-time
constants for depth two and legal index twelve. No JSON or UCI tuning route is added: experimental
UCI binaries enable the feature through the engine's DEFAULT_ON definition.

Eligibility requires all of:

- Depth exactly two, a non-PV null-window frame (`beta == alpha + 1`), not in check and not an
  exclusion frame. Explicitly require the null window rather than assuming every future non-PV
  caller supplies it.
- Both window endpoints strictly inside the mate-score range, using `GameValues::Mate_Threshold`.
- Zero-based legal move index at least twelve: the thirteenth legal move or later.
- No capture, promotion, hash move or either live killer move.
- After successful make: the move does not give check and `td.check_draws(ply + 1)` is false.

Extract `late_move_pruning_eligible(depth, alpha, beta, is_pv_node, in_check,
is_exclusion_frame)` for the compile/runtime and node guards. Keep legal-index and move guards
in the loop. The hash-move exemption is defense in depth: current ordering places a legal hash
move first, so it cannot reach the pruning threshold.

The count is the existing legal-move count, including captures, exempt moves and prior skips;
pseudo-legal moves rejected by `DoMove` do not advance it. It is not a quiet-move count. Quiet pawn
moves and castling receive no additional exemption in this candidate. A new exemption changes the
candidate and must be declared before measuring it.

Index eight is rejected for the first experiment because the recorded cutoff-exposure and root
agreement evidence already suggest a more aggressive risk profile. Quiet-only counting and a
history threshold are plausible alternatives, but would introduce another selection experiment.

### D2: Make/unmake remains necessary to preserve checks and immediate draws

Compute cheap node and move eligibility on the parent board. Only successful `DoMove` advances
the legal index; confirm post-move check and draw exemptions before skipping. Undo every skipped
move before continuing, and do not enter its recursive search or increment `nodes_searched` for
that edge. Maintain a frame-local `lmp_skipped` flag only for actual skips.

Skipping before make would save more work but cannot preserve these exemptions with the existing
API. Do not add an approximate check or draw detector. Keep frontier and LMP skip state separate:
they cannot trigger in the same frame at their declared depths, but can occur in ancestors and
descendants of each other.

The first legal move is always searched, so LMP cannot manufacture a no-move terminal result.
Protected moves remain searchable after the threshold; this is a per-move filter, not a break out
of the move loop.

### D3: Completed selective fail-low returns entry alpha without a TT store

Freeze the following conservative policy for the first candidate:

| Frame outcome | Return and persistence |
|---|---|
| No LMP skip | Existing return and TT behavior |
| LMP skip, then an actually searched beta cutoff | Existing searched fail-high score and LOWER store; existing killer/history updates after the abort guard |
| LMP skip, completed fail-low | Return `original_alpha`; suppress this frame's TT store entirely |
| Aborted frame | Existing abort unwind result; never substitute the completed fail-low policy |

Perform the completed fail-low handling before generic TT classification. Under D1's integer
null window there is no interior score: a completed frame either fails low or cuts off. Do not
create an EXACT entry from an incomplete move set. Preserve terminal handling for frames with no
legal moves; such a frame cannot have an LMP skip.

Returning entry alpha is a fail-hard selective result, not a mathematical proof that every
omitted move was below alpha. It avoids returning an unjustifiably low fail-soft value from the
searched subset. Suppressing the store limits direct reuse but does not make the parent or any
ancestor exact, remove previously existing entries, or repair a missed quiet defense. LOWER from
a searched cutoff retains the engine's existing selective-search meaning, not a new exactness
guarantee. The shipped reverse-futility early return provides a precedent for fail-hard selective
results without a local TT store; it is not evidence that this new pruning rule is safe.

Do not copy frontier futility's `static_eval + margin` floor: pure count pruning supplies no such
evaluation predicate and should not force the lazy evaluator to run. Normal selective UPPER
storage might preserve more TT utility, but is rejected for this initial candidate because it
would persist a fail-low based on an omitted move set. Suppressing all stores, including searched
cutoffs, would discard useful evidence unnecessarily. No move-only TT API or replacement-policy
change belongs here.

### D4: Preserve abort and diagnostic contracts

Every recursive sequence still restores the board and checks `IsAborted()` before result writes.
Do not move ordinary TT/PV/killer/history writes above that guard. A skip performs no recursive
search and must restore the board, but needs no additional abort read: a stop arriving during a
tail of skips invalidates no unfinished child result. The caller still checks abort after the
return. Keep deterministic abort tests around recursive unwind; do not require a timing-dependent
test that injects a stop during a skip tail.

Mirror `frontier_futility_skips`: one thread-local `int64_t` counter, reset with search state and
aggregated into `SearchResult` after workers join. Increment only for actual skips inside the
feature gate. Counts measure attempted work and may survive abort. No second diagnostic gate,
equivalence variant, shared hot-path atomic or new TT bit is needed. Keep the same counter in
timed and lab candidates so its cost is included; no unmeasured claim of zero enabled overhead
is made. Keep skipped edges out of main/QS node counts. The compiled-out path has no increment.

### D5: Independent experiment on a fresh baseline

Source inspection used local `19ff12ba77dca7b0c84de18da6de721e75b1f0d6`. Live main on 2026-09-14
was `30a5d468be9ca5a8af25daf3fce03d166a637fe5`; the comparison shows #544/#550 changed TT aging
and same-key replacement, while the `pvs()` move loop was unchanged. Therefore the old numerical
results are motivation, not an acceptance baseline. The controller must start from then-current
`origin/main`, inspect intervening search changes and rebuild both sides of the comparison.

[#548](https://github.com/theEscape2207/StratChess/issues/548#issuecomment-5656894688) reported
2.86% lower median wall time, eight of nine faster rounds, and 8,345 additional depth-two reverse
futility cutoffs on its candidate trace. All additional cutoffs required shallower TT entries.
This makes #545 worth a separate design; it does not measure LMP's residual headroom. Neither
production #545 nor another overlap spike blocks this experiment. If it lands before #547,
rebase and repeat this candidate's screen.

[#549's result on #347](https://github.com/theEscape2207/StratChess/issues/347#issuecomment-5659300126)
supports parking the rule-50 guard for insufficient observed ordinary-corpus exposure. Eight
replayed games reached a maximum clock of only 53; the constructed clock-90 case had substantial
exposure. The defect remains a documented confound. Preserve immediate-draw exemptions and add
near-boundary screening, without claiming LMP fixes cross-clock TT reuse. A #347 implementation
is not a prerequisite; #545's broader TT reuse still requires its own accepted context policy.

## Assumptions I cannot verify from the code

- **Magnitude and reproducibility of original headroom:** reporter evidence, not reproduced for
  this design. The original prototype differs from D3. Recover its patch/drivers and corpus, or
  regenerate a minimal reproducible record; measure this exact candidate on the fresh baseline.
- **Index twelve is a useful strength tradeoff:** unknown. Guard tests and tactical screening can
  reject it; only an isolated strength measurement can support shipping it.
- **Targeted expected outcomes and runtime fit the bounded screen:** not verified. Before launch,
  the controller freezes a small corpus, justified acceptable moves/outcomes, time budgets and
  repeat protocol. The 250-opening agreement screen is optional diagnostic evidence only; omit
  it if its corpus/driver is unavailable or it cannot fit the time budget.
- **A deeper main search identifies better moves:** not assumed. Agreement and score differences
  are diagnostics; document any independent reference and its limits.
- **The current lab budget and configuration fit the owner's schedule:** not verified. The
  controller consults `measure-strength` and current CI configuration before requesting a run.

## Invariants

- The compiled-out build preserves node counts, scores, PVs and best moves at `Threads=1`.
- Depth, window, mate, check, exclusion and move exemptions are conjunctive; changing another
  pruning option cannot enable LMP outside them.
- Legal index advances once per successful make, including exempt/skipped moves, and never for
  rejected pseudo-legal moves. Every successful make has exactly one undo.
- A local skip writes no PV, killer or history result, and its edge is not a searched node.
  Ancestors may record the selective return: negated entry alpha equals the caller's beta for a
  null-window child, so the caller may cut off and store LOWER/update killer and history normally.
  Store suppression is local to the skipped fail-low frame, not propagated through ancestors.
- A child-search abort takes the existing unwind guard before new completed-result handling;
  no unfinished child result is retained. A completed skip tail requires no additional abort read.
- A skipped fail-low frame writes neither UPPER nor EXACT; an actually searched cutoff retains
  existing LOWER behavior. Frames without LMP skips retain their existing contract.
- No TT layout, clock identity, search-window narrowing or frontier-floor change is introduced.

## Validation

This is a search-behavior change. Enabled search equivalence with main is not expected. Apply the
repository search validation tier, with Linux Debug/sanitizers and shipping Windows checks both
required before merge. Consult `Docs/TestDesign.md` before writing tests and use the existing
search fixture rather than a parallel harness.

Direct predicate tests cover runtime enablement, depth 1/2/3, PV and non-null windows, in-check
and exclusion frames, and mate endpoints. Search-level tests exercise index 11/12, illegal moves
ahead of the threshold, protected moves and prior skips contributing to the index, first legal
move and terminal positions, captures (including en passant), promotions, both killers, giving
check, immediate repetition/fifty-move draws, board/history/hash restoration and recursive abort.
For persistence, seed identifiable TT state and prove no store after a skipped fail-low, including
when searched scores fall below entry alpha; prove searched LOWER storage after a skip and
unchanged storage with no skip. Keep frontier floor regression coverage. Tests must show that
removing the relevant guard or store suppression breaks the intended assertion, rather than
merely producing a plausible move. The redundant hash-move guard is exempt from that obligation:
preserve it and existing hash ordering coverage, without inventing an unreachable late hash move.
Run the full required suites and tactical stability without
raising tactical depths to conceal a new failure. Dispatch `search-reviewer` after implementation.

The bounded local screen uses **one frozen candidate and at most one working day**, with no match
or lab dispatch:

1. Recover provenance; establish compiled-out equivalence; pass focused and tactical
   checks before timing.
2. Run nine paired, interleaved rounds at each of depths 12 and 14, alternating which build runs
   first. Use Release clang-cl on the same host, `Threads=1`, equal Hash and identical TT-reset
   policy. Capture corpus hashes, executable hashes, build flags, exact commands and per-position
   results. Report median paired percentage wall-time change, its range and faster-round count;
   also report each build's median. Do not switch between these statistics to obtain a pass.
3. Compare a small, frozen set of targeted quiet-defense, sparse-endgame and near-rule-50
   positions under equal wall-time budgets. Predeclare acceptable moves/outcomes with a tactical
   justification or independent reference, budgets and a repeat protocol. Park on a reproducible
   loss of an accepted outcome that the baseline retains at the same budget. An already failing
   baseline or uncertain expected outcome is a limitation, not a new candidate failure.
   The optional 250-opening screen records aggregate agreement and score diagnostics only; it
   does not require explaining every changed move and has no agreement-percentage pass threshold.

Wall time here measures cost to reach a fixed nominal depth in different trees. Record main/QS
nodes as search-work diagnostics and nps separately as throughput; neither is playing strength.
The basic skip counter remains in timed binaries; detailed traces stay out.

**Decision rule:** park on a reproducible contract violation or new tactical failure. Also park
if neither depth has a positive median paired wall-time saving. If both depths save time and all
correctness gates pass, present the remaining quality risk to the controller for an owner decision
on the lab. One saving depth, noisy/unstable timing, incomplete coverage or exhausted time budget
is ambiguous and returns to the controller. It does not authorize threshold tuning, a second
candidate or automatic extra runs. Re-measure after a material baseline or candidate change.

This wall-time rule is deliberately a sanity check, not a minimum worthwhile gain. No percentage
floor is inferred from the differently configured index-eight prototype. A tiny positive median
does not prove useful savings, and spread inconsistent with a stable saving is ambiguous. Even
a clear timing pass only licenses an owner decision on spending the lab; strength is the actual
shipping gate.

**Shipping requires an owner-approved CI strength lab** against the candidate's merge base, using
the actual enabled binary with its basic skip counter. Set workflow input `cmake_defines` to
`-DSTRAT_LATE_MOVE_PRUNING=ON`; `strength.yml` forwards it through `CMAKE_DEFINES` to both builds.
This isolates the feature only when the reference lacks the option (unused CMake cache arguments
normally warn rather than fail) and D1 enables the candidate engine by default. Verify both build
logs and effective configurations. If an experimental default-off PR has already landed and the
reference recognizes the option, this input would enable both engines and invalidate the contrast;
the controller must arrange a distinct baseline configuration or a default-enablement comparison
before dispatch. A default-off-versus-default-off run also measures nothing. Follow `measure-strength`
for compiler parity, budget, run validity and interpretation, and `Measurements/README.md` for
recording. Report the interval and conditions. A negative or inconclusive result does not establish
a gain. The nominal lab cost is about three hours and 18 of 20 CI slots, subject to confirmation.
No Elo match is needed to deliver or review this design document.

## Controller handoff

This section is retained because execution is explicitly being handed to another agent. The
controller owns exploration and verification, closes interim decisions, and supplies an implementer
with a bounded edit list and explicit worktree-relative binary paths. Do not delegate unresolved
search-policy choices to the implementer.

| Checkpoint | Controller responsibility |
|---|---|
| Before implementation | Establish a fresh task branch/worktree from `origin/main`; commit this design before publishing it for review; obtain design review and resolve D1–D4 objections, especially persistence. Record accepted changes before coding. |
| Before local screening | Verify baseline changes, candidate/build flags, targeted expected outcomes and exact budgets; freeze the experiment and confirm all prerequisite tests pass. |
| After screening | Record pass, park or ambiguity with evidence. Resolve interim questions; consult the owner for changed scope or measurement expenditure. |
| Before PR and lab | Follow `open-pull-request`, complete required validation and specialist review, and prepare a concrete reviewable candidate. Coordinate explicit owner approval and CI capacity for the isolated strength run. |
| Before enabling/merging | Require evidence for this exact algorithm and configuration, harvest durable rationale/results and reconcile any default-on change with the measured build. An experimental default-off PR is not completion of the strength objective. |

The controller may separate an experimental PR from enabling the feature, or keep one PR open
through measurement. Either way, retain the gate until the strength decision and do not merge an
enabled candidate solely because the local screen passed. This document creates no automatic
authorization for a PR publication, paid run, multi-hour local match or production enablement.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Legal indexing, exemptions and make/unmake requirement | Concise source comments at LMP eligibility/skip path; boundary tests |
| Selective fail-low return and TT suppression, including limits | `Docs/EngineContracts.md` search internals; return/store source comment and TT contract tests |
| Abort and diagnostic counting semantics | Skip/unwind source comments; restoration/abort tests |
| Test purpose and falsification coverage | `Docs/TestDesign.md` search coverage map |
| Frozen candidate, baseline, commands, corpora and local results | Durable report under `Measurements/` using its recording conventions; issue/PR links |
| Strength verdict and uncertainty, or reason for parking | Appropriate measurement ledger and `Docs/Changelog.md` if shipped; #547 outcome |
| Any approved departure from this design | Update this table with the change and rationale before final review |
| Minor implementation departures (below) | This table until harvest; PR body |

Implementation departures (minor, within Scope, no Decision changed):

- `info string lmp skips` is emitted over UCI when non-zero, mirroring frontier skips, so timed and lab
  binaries expose the D4 counter without a harness change.
- No depth-3 *node* test: its depth-2 children are eligible and skip legitimately. Depth 3 is covered by
  the direct predicate test.
- "Prior skips advance the index" has no count-based test: a frozen index at the threshold would skip
  the same moves. It is structural (the index increments at `DoMove`, before the skip). Exempt moves
  advancing the index is covered by the index-11/12 boundary case, where captures and promotions sort
  ahead.
- The two fail-low persistence cases turn reverse futility off, which would otherwise return before
  the move loop in that position.

Pre-screen evidence (2026-09-14, clang-cl, base `30a5d46`): compiled-out engine IDENTICAL to main at
depth 12 on the six built-in positions; every one of 17 guard mutations fails at least one `[lmp]`
test; with the test binary built DEFAULT_ON, `[tactical]` and `[tactical_full]` pass, and the only
other failures are the flag-default case and a reverse-futility case expecting a depth-2 UPPER store,
both expected under D1/D3. `search-reviewer`: no blocking findings. Its screen note: `check_draws`
does not see a quiet move that stalemates the opponent, so such a move can be skipped (frontier
futility shares the gap); include one such position in the targeted quality set.

Local screen (2026-09-14, clang-cl Release, candidate `627c210` built `-DSTRAT_LATE_MOVE_PRUNING=ON`
against the same commit built OFF, Threads=1, default Hash, Run-Bench built-in 8-position set):

| Depth | Median paired wall time | Range | Candidate faster | Median ms base / cand | Nodes | nps |
|---|---|---|---|---|---|---|
| 12 | −29.0% | −33.6..−27.6% | 9/9 | 4017 / 2852 | −31.5% | −3.4% |
| 14 | −18.7% | −24.2..−15.9% | 9/9 | 12380 / 10056 | −21.4% | −3.4% |

Rounds interleaved, first build alternating per round and depth. Per position it is not uniform: at
depth 14 `closed-mid` is +47% and `tactical-5` +12% slower, `rook-endgm` −60%. Best moves differ on
two or three positions per depth, which is expected in different trees and not a verdict.

Targeted set: 12 positions from the #544 strength-lab games (run 34788035846), sampled with a fixed
seed, 4 each near-rule-50 (clock 70–98), sparse (≤8 men) and quiet-defense (mover −300..+100 cp).
Kept only if Stockfish (depth 18, multipv 4) had a quiet best move ≥120 cp ahead of the runner-up
and a depth-26 result that was not a forced mate. Accepted moves are those within 40 cp of the
depth-26 best. Frozen before any StratChess run. At movetime 250 and 2000 ms, 5 fresh-process
repeats per build: no candidate loss (flag rule: baseline ≥4/5 and candidate ≤2/5). 11 of 12
positions were 5/5 for both builds at both budgets. One quiet-defense position (`d3d6`) was 0/5 for
both, so it is a limitation, not a finding. The set is therefore weak evidence: nearly every
position was easy for both builds.

Stalemate position: not included. 3M random low-material positions gave none where a single quiet
stalemating move is the only non-losing move (Stockfish, depth 26). At the root such a move is
searched as a PV move, which LMP never prunes, so a root position would not exercise the gap anyway.
The gap stays a documented limitation for the lab.

Decision rule outcome: both depths save time and every correctness gate passed, so this goes to the
owner for a lab decision. The screen does not estimate strength.

Review reconciliation (2026-09-14): adopted the existing compile/runtime test-gate pattern,
direct node-predicate tests, hash-guard falsification exception, frontier-style counter and abort
handling. Clarified ancestor persistence, the timing sanity gate, targeted quality criteria and
the shared lab build input. The controller handoff remains while this artifact serves as the spec
being handed off; migrate execution-only material to working notes when that handoff is complete,
before final PR preparation. Preserve durable validation criteria and provenance destinations.

Keep the document through design review. Follow `Docs/Workflow.md` for state transitions and delete
only after harvest is complete, no inbound references remain and it no longer serves as a spec.
Controller run ordering and temporary command logs belong in working notes rather than permanent
engine documentation.
