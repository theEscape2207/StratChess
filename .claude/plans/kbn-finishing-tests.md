# KBN finishing coverage — Design

**Issue:** #657
**Approval:** The owner selected this narrowed replacement in the 2026-10-02 conversation.

## Goal

Replace a skipped, depth-sensitive long conversion gate with cheap coverage that production search
finishes short forced KBN mates. General confinement and conversion remain the separate #596 gap.

## Review focus

- D1: actual mate, including every legal defensive reply, rather than a score or engine self-play.
- D2: runtime and sensitivity to a relevant terminal-scoring fault.

## Scope

This change replaces `EndgameConversionTests.cpp` and its coverage description in `Docs/TestDesign.md`.
It changes no production search or evaluation, adds no dependencies, and claims no general conversion
or corner-guidance coverage. Existing KBN evaluation tests retain the corner-guidance contract.

## Decisions

### D1: Short mate through the existing public search seam

Use one hand-verifiable mate-in-one and one mate-in-two, each with horizontal and colour/rank mirrors.
Production `AIPerplex::Search` selects only the winning side's moves. Enumerate every legal defending
reply and require checkmate of the defender within one or three plies, respectively. Checkmate means
in check with no legal moves; a reported mate score alone is insufficient. Accept any move that meets
this bound, rather than pinning a coordinate. The fixture oracle is checked offline with python-chess;
runtime tests need no external oracle or tablebase.

The rejected long self-play gate is sensitive to unrelated move ordering and depth. An ensemble would
cost more while requiring a discriminator not established by the investigation. The owner accepted
this modest finishing coverage rather than general conversion coverage or simple deletion.

### D2: Fixed, isolated and bounded searches

Run the eight fixtures at fixed depth caps 4, 6 and 8, with halfmove clocks 0 and 94. Each winning-side
decision uses fresh search state, one thread, and a 1 MiB table. There are no time limits, random
fixtures, pooled success thresholds or wall-time assertions. Target added Release Catch2 time below
0.25 seconds; measure the actual test rather than extrapolating the UCI probe. Early mate stopping
means the depth sweep checks caps, not independent full-depth trajectories.

## Assumptions I cannot verify from the code

The current Release implementation will find all selected short mates cheaply. The prior UCI probe
passed, but fresh Catch2 runs verify this on the current baseline. Debug/sanitizer cost is checked by
the Linux CI correctness gate. No assertion depends on timing.

## Invariants

- Every fixture/clock/depth must mate; no aggregate allows a failure to disappear.
- Null/illegal choices, stalemate, wrong-side mate, clock exhaustion and exceeding the ply bound fail.
- Every legal defending branch is played; search never chooses a cooperative defensive reply.
- The fast tier includes the replacement. No skipped long conversion gate remains.

## Validation

Engine validation tier applies to test sources. Verify all fixture trees independently offline;
run the focused Catch2 tag, existing KBN evaluation cases, and the required pre-PR gates. Temporarily
replace production checkmate scoring with a draw score, rebuild, and require the new test to fail;
restore production code, rebuild, and require green. Measure focused Release Catch2 durations and
repeat the fixed fixture/depth matrix. Linux Debug with sanitizers is CI's correctness gate. No Elo
match is needed because the final diff changes no engine behavior.

## Cost

50–200 lines of test implementation and two lasting files. No optional diagnostic harness. The fast
tier gains a bounded set of shallow searches. Standards and Spec review cover the final diff.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Finishing contract, all defensive replies, isolated fixed depth | Test names and helper comments |
| Fixtures, scope, corner-evaluation ownership and runtime evidence | `Docs/TestDesign.md` |
| Mutation and validation evidence | PR Test plan |

No new decision beyond the owner's selected option; implementation details preserve that contract.
