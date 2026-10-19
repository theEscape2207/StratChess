# Improving flag for LMR — Design

**Issue:** #848 (#636 child B3)

## Goal

LMR reduces every late quiet the same, whatever the trend of the position. When the side to move
is doing worse than it was two plies earlier, its late quiets are less likely to cut. Reducing them
one ply more saves depth where it is least likely to be needed. #636 found LMR re-searches at 11.2%
of nodes, and the last two LMR changes both shipped (B1 at +15.1 ± 3.2). This change adds the flag
and a tunable reduction offset, off by default, so one lab run can decide it on the binary that
merges.

## Review focus

- **D2, the exact eval gate.** The claim is that storing an eval only at frames with
  `depth >= lmr_min_depth` loses nothing, because depth never increases along a path. If any path
  can raise depth, a reading node would see a sentinel where a real eval belonged.
- **D2's exclusion-frame rule.** A singular verification frame shares its parent's ply and must not
  overwrite the slot. A test covers it.
- **D4, where the offset enters `lmr_reduction()`.** Before the cap, so the cap and the history
  shift keep their meaning. Check that the profile build's `record_lmr_reach()` still isolates the
  history shift.
- **Assumption A1:** the per-node cost at offset 0 is a store and a branch, below the bench's noise.

## Scope

**This change will:**

- add a per-ply static-eval stack to `ThreadData`, written by every `pvs()` frame before it
  searches any child;
- add `lmr_improving_offset` (UCI `LmrImprovingOffset`, default 0), which raises R by that many
  plies at a node that is not improving;
- add tests for the improving rule, the offset in `lmr_reduction()`, and the search being reached.

**This change will not:**

- use the flag in reverse futility, LMP or frontier futility. Those are later #636 children, each
  measured on its own;
- change the default. A follow-up flips it to 1 if the lab passes, in the same PR as the
  measurement record;
- touch quiescence, which never reads the flag.

## Decisions

### D1: The improving rule

A node at `ply` is **improving** when `eval[ply] > eval[ply-2]`.

- If `eval[ply-2]` is the sentinel (`kNoStaticEval`: the ancestor was in check), compare with
  `eval[ply-4]`.
- The fallback reads `eval[ply-4]` only when `ply >= 4`.
- In every other case the node counts as improving, so it gets no extra reduction:
  - `ply < 2`;
  - `ply` is 2 or 3 and `eval[ply-2]` is the sentinel;
  - both ancestors are the sentinel.

`Evaluate()` scores from the side to move, and ply and ply-2 have the same side to move, so the
values compare directly. That holds under a null move too: the null child is at ply+1, and ply+2
again has the original side to move.

Rejected:
- **Comparing with ply-1:** it is the opponent's view, and the sign flips with the tempo.
- **Treating a missing ancestor as not improving:** it would reduce more on no evidence.

The rule is a pure function, `is_improving(const int16_t* evals, int ply)`, so it can be unit
tested.

### D2: When the stack is written

Every frame that gets past the TT probe, except an exclusion frame (below), writes `td.ply_eval[ply]` once, right after `in_check` is
known and before reverse futility and the null move.

- **Real eval:** written when `lmr_improving_offset != 0 && !in_check && depth >= lmr_min_depth`,
  using `node_eval()`, so a TT-served eval is reused.
- **Otherwise:** `kNoStaticEval`.

This is exact, not a heuristic. A frame only reads the flag where LMR applies, which needs
`depth >= lmr_min_depth`. The only extension (singular) keeps a child at its parent's depth, and
nothing raises depth. So every ancestor of a reading frame also had `depth >= lmr_min_depth` and
stored a real eval, unless it was in check.

The early exits (backstop, abort, draw, horizon, TT cutoff) return before searching any child.
So no slot a descendant reads can be stale.

Writing the sentinel unconditionally costs one store, and it means the stack never needs clearing.

Rejected:
- **Evaluating at every node not in check:** the triage probe measured that at -1.90% nps
  ([-2.01, -1.78], placement not controlled), almost all of it in endgames. There reverse
  futility's zugzwang guard means shallow frames rarely evaluate today. Those shallow frames can
  never be read, so the cost buys nothing.
- **Leaving `node_eval()` lazy and reading whatever happens to be computed:** that makes the flag
  depend on which pruning guard happened to run.

**An exclusion frame writes nothing.** It re-enters `pvs()` at its parent's ply with the same
position, and at a reduced verification depth that is often below `lmr_min_depth`. If it wrote, the
sentinel would overwrite the parent's real eval. Every later child of the parent would then lose its
ply-2 value. By skipping the write, its own descendants read the parent's value, which is the eval
of the same position.

### D3: Where the stack lives

`int16_t ply_eval[MAX_PLY]` in `ThreadData`, next to `killers` and indexed the same way. It is
`int16_t` because that is the TT's `static_eval` type and `kNoStaticEval` is the sentinel already.
It is not cleared per search, because D2 writes every slot before any reader.

### D4: How the offset enters R

`lmr_reduction()` gains an `int extra` parameter (0 or `lmr_improving_offset`), added to the raw
base before the existing `max(1, ·)` and the `depth-2` cap:

```
base = min(max(1, int(raw) + extra), cap)
```

The history shift and its clamp then apply as today.

- Adding it before the cap keeps R inside `[1, depth-2]` whatever the tuning.
- The history shift can still reduce R at a non-improving node.

`record_lmr_reach()` passes the same `extra` to its base call, so its counts still describe the
history shift alone.

The profile output gets no new counter. A count of non-improving nodes would only measure
eligibility, not reach: at depth 3 the cap is 1, so `extra` cannot change R there. Measuring reach
means comparing the final R with and without `extra`, and adding a row means changing the parser,
the report, its tests and the EngineGuide. That work is worth doing only if the lab result needs
explaining, and then as its own change.

Rejected:
- **Adding the offset after the history clamp:** the history shift would then apply before the
  offset and could no longer cancel it. For example, at depth 6, move index 20 and history
  quotient 1, the pre-cap placement gives R = 3 and the post-clamp placement gives R = 4. Placing
  it first keeps history's priority: a quiet with good history is still reduced less.
- **A separate log-table row for non-improving nodes:** more constants to tune, for a first
  measurement.

### D5: The option

`TUNING_FIELD(int, lmr_improving_offset, 0, 0, 3, true, "LmrImprovingOffset", true)`. The range
0-3 bounds this first experiment, which measures 1. Widening it is a one-line change if a later
tune wants more.

At 0 the frame computes no extra eval, and the reading code is skipped, so the search is
node-identical to the merge base. The lab measures 1 with `candidate_uci_options`.

### D6: Relief at improving nodes (added after the first lab run)

The offset-1 lab run measured -1.1 ± 3.6 Elo. The screen explains why it barely moved anything: 94% of
reduced quiets already have a base at the `depth-2` cap, and history moves only about 5% of them, so
a pre-cap `extra` changes R for very few moves. The approved variant runs the other way:
`lmr_improving_relief` plies are taken off R at an improving node, **after** the history clamp, with
a floor of 1. Placed there it reaches the capped majority.

- **Evidence rule:** `is_improving()` becomes `eval_trend()`, returning `Improving`, `NotImproving`
  or `Unknown`. The offset applies only on `NotImproving` and the relief only on `Improving`, so a
  missing value earns neither. Offset behaviour is unchanged: `Unknown` already gave no extra.
- **Option:** `TUNING_FIELD(int, lmr_improving_relief, 0, 0, 3, true, "LmrImprovingRelief", true)`.
  The stack is written when either option is nonzero.
- **Profile:** `record_lmr_reach()` gets R before the relief, so its counts still describe the
  history shift alone.
- **Measurement:** one lab run at `LmrImprovingRelief=1` (offset 0) against the merge base, with the
  same ship rule. Equivalence at the defaults must still be identical. No screen or wall clock: the
  lab decides, and both run only if the result needs explaining.

Rejected: a signed offset. The UCI parser rejects a sign, and a pre-cap negative offset would be
swallowed by the cap just like the positive one.

## Assumptions I cannot verify from the code

- **A1: the offset-0 cost is within bench noise.** The added work per frame is one compare and one
  `int16_t` store. Verified by an ordered-pair `Compare-Bench.ps1 -Control` against the merge base;
  the verdict must be "No slowdown". That verdict allows up to 0.5% cost, so report its
  interval, not "zero overhead".
- **A2: the eval-at-depth≥3 cost is small at offset 1.** It is not separable from the tree change,
  so `Compare-WallClock.ps1` and the lab measure it together. No separate gate.

## Invariants

- At `lmr_improving_offset = 0`: node counts and best moves are identical to the merge base at
  `Threads=1`.
- R stays in `[1, max(1, depth-2)]` for every offset in range.
- A frame never reads a `ply_eval` slot that its own path did not write in this search.
- The unwind invariant is unchanged. `ply_eval` is scratch written before any child is searched,
  in the same class as `cont_key`, so it needs no abort guard.

## Validation

**Tier: engine search change.**

- **Equivalence:** `Compare-SearchEquivalence.ps1 -After <PR build>` with the default 0, which must
  be node-identical.
- **Speed:** an ordered pair (`New-OrderedBuildPair.ps1`) through `Compare-Bench.ps1 -Control`,
  which must read "No slowdown" (A1).
- **Unit tests** (`SearchLmrTests.cpp`):
  - `is_improving()`: higher, equal, lower; a ply-2 sentinel falling back to ply-4; both
    missing; `ply < 2`; and `ply` 2 and 3 with a sentinel at ply-2.
  - `lmr_reduction()` with `extra`: it raises R by `extra` below the cap, never passes the cap,
    and the history shift still applies.
  - **Exclusion frame:** after a singular verification at a node with `depth >= lmr_min_depth`,
    `ply_eval[ply]` still holds the node's real eval. Use a fixture position known to trigger
    singular extensions, and a test-only accessor. The verification depth must be below
    `lmr_min_depth`, so that removing the skip makes the test fail.
  - **Reaches the search:** offset 1 changes `mainnodes()` against offset 0 on the existing LMR
    fixture position, at depth 8.
  - **Falsification:** each test is shown to fail with its code path reverted
    (`feedback_falsify_new_regression_tests`).
- **`SearchTuningTests.cpp`:** the mirror struct, the default, and the JSON round trip.
- **Screen** (insight, not a gate, about 25 min):
  - Run `Compare-SearchProfile.ps1 -Seeds 8 -Depth 12 -Positions Tests/profile-screen.fen` on two
    `-DSTRAT_SEARCH_PROFILE=1` builds of the PR: the PR as is (offset 0), and a local-only copy
    with the default set to 1.
  - The offset-0 build is the tree-shape reference, which is valid once equivalence has passed.
    Record that provenance with the row.
- **Wall clock** (about 17 min): `Compare-WallClock.ps1` on shipping builds, never profile builds,
  so the instrumentation is not timed.
  - Baseline: the exact merge base.
  - Candidate: the local default-1 shipping build, so the run times both the stack and the tree
    change.
  - Neither script accepts a UCI option, which is why the default-1 build exists.
- Both rows go to `Measurements/ci-per-change.md` next to the lab row.
- **Elo:** one strength-lab run, already approved:
  `gh workflow run strength.yml --ref <branch> -f candidate_uci_options="LmrImprovingOffset=1"`
  against the merge base.
  - **Ship** if Elo minus its error bar is above 0: flip the default to 1 in a follow-up commit on
    the same PR, re-run equivalence against the offset-1 build, and record the result.
  - **Otherwise park:** merge with the default at 0 only if the option is worth keeping, else
    close the PR. That is the owner's call when the result is positive but spans zero.

## Cost

- **Size:** 50-200 lines across `ThreadData.h`, `AIPerplex.{h,cpp}`, `SearchTuning.def`,
  `SearchLmrTests.cpp`, `SearchTuningTests.cpp`,
  `SearchTestFixture.h` (a setter) and `Docs/EngineGuide.md` (the option).
- **Blast radius:** engine tier. No gate, skill or script changes.
- **Review:** one code review, plus `search-reviewer`.
- **Measurement:** the ordered-pair bench takes about 15 min of quiet, the screen about 25 min and
  the wall clock about 17 min. The lab run, already approved, takes about 3 h and 18 of 20 CI slots.
- **Optional parts:** none. The reach counter was dropped in review (D4).

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1's fallback rule and its same-side reasoning | comment on `is_improving()` |
| D2's exact-gate argument (depth never increases) | comment at the `ply_eval` write in `pvs()` |
| D3's no-clear reasoning | comment on `ThreadData::ply_eval` |
| D4's before-the-cap placement | `lmr_reduction()` header comment |
| D5's option meaning and range | `SearchTuning.def` comment, `Docs/EngineGuide.md` |
| D6's after-the-clamp placement and the evidence rule | comment at the relief in `pvs()`, `eval_trend()` comment, `SearchTuning.def` |
| Lab, screen and wall-clock results | `Measurements/ci-per-change.md`, headline in `Docs/Changelog.md` |
