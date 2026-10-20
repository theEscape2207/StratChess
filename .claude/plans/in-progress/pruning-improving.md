# Improving flag for the pruning margins — Design

**Issue:** #851 (#636 child B3, slice 3 of #848)

**Status:** RFP (first PR) measured -2.75 ± 3.54 at relief 50 and did not ship; PR #854 closed
unmerged, its commits at `refs/pull/854/head` (`Measurements/ci-per-change.md`, 2026-10-19). The
cost probe (Validation, step 4) read a median -1.5% nps, unresolved, with -4% resolved in endgames
(`Measurements/local.md`, 2026-10-19). LMP (second PR) measured +1.79 ± 2.82 at cut 4 over 32,000
games and did not ship; PR #862 closed unmerged, its commits at `refs/pull/862/head`
(`Measurements/ci-per-change.md`, 2026-10-20). Frontier futility (third PR) is next.

## Goal

The three shallow pruning guards use one margin whatever the trend of the position. The hypothesis:
when the side to move is doing better than two plies ago, a static eval above beta is more likely to
hold, so reverse futility can cut on less slack; when it is doing worse, late quiets are less likely
to recover, so late move pruning and frontier futility can skip more of them. Other engines read an
improving flag in these guards, not only in LMR; which use carries their gain is not established
here. #848's two LMR uses failed in the lab (offset -1.1 ± 3.6, relief -13.7 ± 3.6;
`Measurements/ci-per-change.md`, 2026-10-18 and 2026-10-19), and PR #849 closed unmerged, so this
work has to bring the eval stack as well.

## Review focus

- **D7, one PR per margin.** Each PR forks from the `main` that already holds every margin shipped
  before it, so the lab's default `merge-base` reference is the right one and each run measures one
  margin. Check that no stage can measure two changes at once.
- **D2, the write gate.** RFP and LMP read the trend at depth 1-3, but the ancestor at ply - 2 sits
  at any depth >= the reader's, so the gate is every depth. Its cost is unmeasured on this design;
  #848's eager-eval probe read -1.9%, placement not controlled.
- **D4, the RFP lab value.** Relief 50 at an improving node is below any margin measured. #502
  showed no gain at a uniform 75 or 150 and a loss at 200; those runs do not forecast this one's
  sign.

## Scope

**This design covers three PRs, one per margin (D7).** The first also brings the eval stack.

**The first PR (RFP) will:**

- port `ThreadData::ply_eval`, `EvalTrend` and `eval_trend()`, the exclusion-frame rule and their
  tests from `worktree-lmr-improving-848` at `6b24787` (also `refs/pull/849/head`), as one commit of
  its own, without the LMR options, the `extra` parameter of `lmr_reduction()` or the
  `record_lmr_reach()` change;
- widen the write gate to every depth (D2) and test it;
- add the RFP option (D4), off by default, and measure it in the lab.

**The later PRs (LMP, then frontier futility)** each add one option (D5, D6), from fresh `main`.

**This work will not:**

- touch LMR. Both uses were measured and failed;
- read the trend anywhere else: null move, singular, quiescence and the RFP depth band stay as they
  are. #502's note to re-screen the band at 4-5 once a flag changes the condition is a later run;
- tune a value. Each option gets one lab run at one value; a sweep is a follow-up only if a run is
  positive but spans zero.

## Decisions

### D1: The trend rule is #848's, unchanged

`eval_trend(ply_eval, ply)` compares ply with ply - 2, falls back to ply - 4 when ply - 2 was in
check, and returns `Unknown` when either side of the comparison is missing. Each margin changes only
on the trend it names; `Unknown` leaves every margin at today's value. Its derivation and tests are
in the #848 doc (git history, `6b24787`) and carry over as they are.

### D2: The stack holds a real eval at every depth while any reader is on

`td.ply_eval[ply]` gets `node_eval()` when `tracks_eval_trend()` is true and the node is not in
check, at any depth; otherwise `kNoStaticEval`. `tracks_eval_trend()` is a private `AIPerplex`
member, true when any option of D4-D6 that exists is nonzero; each later PR adds its option to it.
The write sits where #848 put it: after `in_check` is known, before RFP and the null move, so it
precedes every child.

This is the only gate that loses nothing. #848's argument holds: depth never rises along a path (the
singular extension keeps a child at its parent's depth). So a reader at depth d has its ply - 2 and
ply - 4 ancestors at depth >= d, but not at depth <= 3: an unreduced depth-3 reader's grandparent is
at depth 5. The issue's "depth 1-3" is the old gate (`depth >= lmr_min_depth`, 3) extended down to
the readers' depths, which is depth >= 1, every `pvs()` frame.

A node TT-served its eval pays nothing. The cost is the frames that never evaluate today: PV nodes,
depth >= 4, and the zugzwang-unsafe frames RFP skips, mostly in endgames, where #848's triage probe
found almost all of its -1.9%.

Rejected:
- **A small depth cap (`depth <= K`):** for K near the readers' depths it turns many depth-2 and
  depth-3 readers to `Unknown`, so the margin would act mainly at depth 1 and the lab would measure
  the gate rather than the idea.
- **A material gate for RFP (`has_two_non_pawn_pieces`, side to move):** exact for RFP alone,
  barring promotions, since the side's non-pawn count only falls along a path and RFP needs the same
  floor. It would skip most of the endgame cost. It does not serve LMP or frontier futility, which
  read without that floor, and it is a second gate to keep right. It becomes worth building only if
  the probe in Validation shows a cost large enough to explain a failed RFP run.

### D3: An exclusion frame still writes nothing

The verification frame re-enters at its parent's ply with the same position. Under #848's narrow
gate its write would have been the sentinel and erased the parent's eval. Under D2 it would write
the same position's eval again: same value, but computed fresh, because an exclusion frame skips the
TT probe and so is never served. Skipping the write saves that `Evaluate()` per verification. The
#848 test (the slot keeps the node's eval after a verification) is kept as a value check. It no
longer fails when the skip is removed, and it is not claimed to.

### D4: RFP: a smaller margin at an improving node

```
margin = reverse_futility_margin * depth - (trend == Improving ? relief : 0), floored at 0
```

`TUNING_FIELD(int, reverse_futility_improving_relief, 0, 0, 1000, true,
"ReverseFutilityImprovingRelief", true)`. The floor keeps the invariant that RFP never reports a
fail-high from an eval below beta, whatever the tuning. The lab runs 50: half of one ply's margin,
so an improving node at depth 1-3 cuts at 50 / 150 / 250 instead of 100 / 200 / 300.

Rejected:
- **A full ply off at an improving node (relief 100), a simplified form of the improving-dependent
  margins other engines use:** at depth 1 it cuts at eval >= beta, removing all of depth 1's slack.
  50 is the smaller first step; 100 is one value change if 50 is positive.
- **Also raising the margin at a non-improving node:** a second variable in one run.

### D5: LMP: an earlier cut-off at a non-improving node (second PR)

The minimum legal index becomes `kLateMovePruningMinLegalIndex - cut` (12 - cut) at a
`NotImproving` node. `TUNING_FIELD(int, late_move_pruning_not_improving_cut, 0, 0, 8, true,
"LateMovePruningNotImprovingCut", true)`; the bound keeps the index at 4 or more, well clear of the
first legal move. The lab runs 4 (index 8).

### D6: Frontier futility: a smaller margin at a non-improving node (third PR)

`margin = frontier_futility_margin - (trend == NotImproving ? cut : 0)`, the same value in the
move-loop test and in the `frontier_floor` after the loop, so the fail-low floor matches what was
skipped. `TUNING_FIELD(int, frontier_futility_not_improving_cut, 0, 0, 1000, true,
"FrontierFutilityNotImprovingCut", true)`, floored at 0 in use like D4. The lab runs 50 (margin
150). #529 found margins 150-250 gave the same best move on at least 99.7% of 1,911 positions at
depth 12, so this one is expected to move little; it goes last for that reason.

### D7: One PR per margin, each measured against the main it forks from

Order: RFP, LMP, frontier futility. RFP leads on measured sensitivity (#502: confining it to depth 1
cost -9.3 ± 5.0, margin 200 cost -8.7 ± 5.5); frontier is last on #529's insensitivity; LMP's
sensitivity is unmeasured. The order is not load-bearing: each PR stands alone.

Each PR forks from `main`, which already carries every margin shipped before it, so the lab's
default `reference_ref` (`merge-base`) is the stage reference and the run measures one option. Each
ledger row records the candidate and reference SHAs and the options set.

- **Ship** when Elo minus its error bar is above 0: flip the option's default in a follow-up commit
  on the same PR, re-run equivalence (below) against the flipped build, and merge.
- **Otherwise** close the PR with its ledger row on `main` (as #849 did). If it was the RFP PR, the
  next PR cherry-picks the stack commit onto fresh `main` and swaps its option into
  `tracks_eval_trend()`; nothing else in that commit names RFP.

**Equivalence** is per PR: at its option's default of 0 the build is node-identical to its own merge
base. After a flip, the shipped build is the configuration the lab measured, checked by an
equivalence run of the flipped build against the PR build at the lab's option value.

## Assumptions I cannot verify from the code

- **A1: the wide-gate cost.** #848's triage probe, which evaluated every node not in check as D2
  does, read -1.90% [-2.01, -1.78], placement not controlled (A/A +0.44% [-0.18, +1.06]). Nothing
  gates on it. It is measured only if the run that first turns tracking on fails to ship
  (Validation, step 4).
- **A2: the cost at the defaults is a branch.** With every option at 0 the frame writes the sentinel
  and evaluates nothing. #849 measured -0.35% [-0.46, -0.25] ordered-pair with its option off at the
  narrow gate, which is evidence for the shape, not for this code. Re-checked only if a PR merges
  with its option off, per the regression-check rule for a default-off option.

## Invariants

- With this PR's new option at 0, and earlier shipped settings kept: node counts and best moves
  identical to the merge base at `Threads=1`.
- RFP never returns beta from an eval below beta; frontier futility never skips with a negative
  margin.
- A frame never reads a `ply_eval` slot its own path did not write in this search (#848's invariant).
- While `tracks_eval_trend()` is true, every frame that reaches the stack write, is not in check and
  is not an exclusion frame holds its real eval in `ply_eval[ply]`, at every depth. Frames that exit
  earlier (TT cutoff, draw, abort, horizon) search no child, so nothing reads their slot.
- `frontier_floor` uses the margin the skip test used.

## Validation

**Tier: engine search change, per PR.**

1. **Equivalence:** `Compare-SearchEquivalence.ps1` at the defaults against the merge base, and
   after a flip as D7 states.
2. **Unit tests:**
   - #848's `eval_trend()` cases and exclusion-frame test, ported to a new
     `SearchEvalTrendTests.cpp` since no single guard owns the trend.
   - **The gate (first PR):** with tracking on, a direct `search_node()` at depth 1 and at depth 5
     each leaves `ply_eval[ply] == evaluate()`. Restoring `depth >= 3` fails the first; a
     `depth <= 3` cap fails the second. Any ancestor the fixture bypasses is seeded explicitly.
   - **Per option**, in that guard's existing file (`SearchFutilityTests.cpp`,
     `SearchLateMovePruningTests.cpp`, `SearchFrontierFutilityTests.cpp`): the margin arithmetic
     (applied only on its trend, the 0 floor) and a reaches-the-search node-count test.
   - `SearchTuningTests.cpp`: mirror, default, bounds and UCI lines.
   - Each new test except D3's is shown to fail with its code reverted.
3. **Elo:** one strength-lab run per PR at the value its Decision names,
   `gh workflow run strength.yml --ref <branch> -f candidate_uci_options="<Option>=<value>"`
   (reference: the default `merge-base`), about 3-3.5 h. The owner approves each dispatch.
4. **Cost probe (informational, only if the run that first turns tracking on does not ship):** a
   local build writing the stack under D2 with no reader is node-identical to the merge base
   (checked first), so `Compare-Bench.ps1` applies: an ordered pair from `New-OrderedBuildPair.ps1`,
   `-Control -Rounds 60 -Affinity 4`, about 35 min of quiet, runnable while a lab run is in flight.
   It answers whether the cost could explain the failure, which is what would justify D2's rejected
   material gate. Once a margin has shipped, both sides of a later run pay for the stack, so the
   cost cannot explain that run's failure and the probe does not apply.
5. **Screen (optional, insight only):** `Compare-SearchProfile.ps1 -Seeds 8 -Depth 12 -Positions
   Tests/profile-screen.fen` on profile builds of the PR, at the default against a local
   default-on copy; the RFP bins show where the cutoffs shift by depth, not how many the relief
   caused (the step-2 tests prove reach). `Compare-WallClock.ps1` on separate shipping builds.

## Cost

- **Size:** first PR 50-200 lines: the ported stack (~40 engine, ~100 test), the gate tests and the
  RFP option (~10 engine plus tests), in `ThreadData.h`, `AIPerplex.{h,cpp}`, `SearchTuning.def`,
  the tests and `Docs/EngineGuide.md`. Each later PR is under 50 lines plus tests.
- **Blast radius:** engine tier. No gate, skill or script changes.
- **Review:** one code review plus `search-reviewer` per PR.
- **Measurement:** one lab run per PR, about 3-3.5 h each, sequential across PRs.
- **Optional parts:** the cost probe (~35 min, at most once, only after the first tracking run fails) and the screen with
  wall clock (~45 min per PR). Both explain a result; the lab decides it.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2's every-depth gate and why a cap loses readers | comment at the `ply_eval` write in `pvs()` |
| D3's exclusion-frame rule, now a cost saving | same comment |
| D1's rule | `eval_trend()` comment (ported) |
| `tracks_eval_trend()` as the one list of readers | its declaration comment |
| D4-D6 option meaning, floor and range | `SearchTuning.def` comments, `Docs/EngineGuide.md` |
| D6's shared margin for the floor | comment at `frontier_floor` |
| Lab results, the probe and screens if run | `Measurements/ci-per-change.md`, headline in `Docs/Changelog.md` |

This document merges with the first PR, stays in `in-progress/` for the later ones, and the last PR
deletes it.
