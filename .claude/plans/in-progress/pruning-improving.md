# Improving flag for the pruning margins — Design

**Issue:** #851 (#636 child B3, slice 3 of #848)

## Goal

The three shallow pruning guards use one margin whatever the trend of the position. When the side to
move is doing better than two plies ago, a static eval above beta is more likely to hold, so reverse
futility can cut on less slack. When it is doing worse, late quiets are less likely to recover, so
late move pruning and frontier futility can skip more of them. Other engines take most of the
improving flag's gain here, not in LMR. #848's two LMR uses failed in the lab (offset -1.1 ± 3.6,
relief -13.7 ± 3.6; `Measurements/ci-per-change.md`, 2026-10-18 and 2026-10-19), and PR #849 closed
unmerged, so this change has to bring the eval stack as well.

## Review focus

- **D2, the write gate.** RFP and LMP read the trend at depth 1-3, but the ancestor at ply - 2 sits
  at any depth >= the reader's. The gate that loses nothing is therefore every depth, which is the
  eager eval #848's triage measured at -1.9% nps. That cost is paid only while an option is on; the
  bench in Validation sizes it before any margin work.
- **D4, the RFP margin floor and lab value.** `margin * depth - relief`, floored at 0, at relief 50.
  #502 found the uniform margin flat from 75 to 150 and a loss at 200; 50 at an improving depth-1
  node is below anything measured.
- **D3, the exclusion-frame rule under the wide gate.** It no longer protects a value (D3 says why),
  only an `Evaluate()` call.

## Scope

**This change will:**

- port `ThreadData::ply_eval`, `EvalTrend` and `eval_trend()`, the exclusion-frame rule and their
  tests from `worktree-lmr-improving-848` at `6b24787` (also `refs/pull/849/head`), without the LMR
  options, the `extra` parameter of `lmr_reduction()` or the `record_lmr_reach()` change;
- add three off-by-default options, one per margin (D4-D6), each landing in its own commit when its
  lab run is next: RFP first, then LMP, then frontier futility;
- record each lab run, and the bench, in `Measurements/ci-per-change.md`.

**This change will not:**

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

`td.ply_eval[ply]` gets `node_eval()` when an option of D4-D6 is nonzero and the node is not in
check, at any depth; otherwise `kNoStaticEval`. The write sits where #848 put it: after `in_check`
is known, before RFP and the null move, so it precedes every child.

This is the only gate that loses nothing. #848's argument holds: depth never rises along a path (the
singular extension keeps a child at its parent's depth). So a reader at depth d has its ply - 2 and
ply - 4 ancestors at depth >= d, but not at depth <= 3: an unreduced depth-3 reader's grandparent is
at depth 5. The issue's "depth 1-3" is the old gate (`depth >= lmr_min_depth`, 3) extended down to
the readers' depths, which is depth >= 1, every `pvs()` frame.

A node TT-served its eval pays nothing. The cost is the frames that never evaluate today: PV nodes,
depth >= 4, and the zugzwang-unsafe frames RFP skips, mostly in endgames, where #848's triage found
almost all of the -1.9%.

Rejected:
- **A depth cap (`depth <= K`):** it turns most depth-2 and depth-3 readers to `Unknown`, so the
  margin would act mainly at depth 1, and the lab would measure the gate rather than the idea.
- **A material gate for RFP (`has_two_non_pawn_pieces`, side to move):** exact for RFP alone,
  barring promotions, since the side's non-pawn count only falls along a path and RFP needs the same
  floor. It would skip most of the endgame cost. It does not serve LMP or frontier futility, which
  read without that floor, and it is a second gate to keep right. It becomes worth building only if
  the bench below shows a cost the RFP lab result cannot carry.

### D3: An exclusion frame still writes nothing

The verification frame re-enters at its parent's ply with the same position. Under #848's narrow
gate its write would have been the sentinel and erased the parent's eval. Under D2 it would write
the same position's eval again: same value, but computed fresh, because an exclusion frame skips the
TT probe and so is never served. Skipping the write saves that `Evaluate()` per verification. The
#848 test (the slot keeps the node's eval after a verification) is kept as a guard of the value. It
no longer fails when the skip is removed, and the doc does not claim it does.

### D4: RFP: a smaller margin at an improving node

```
margin = reverse_futility_margin * depth - (trend == Improving ? relief : 0), floored at 0
```

`TUNING_FIELD(int, reverse_futility_improving_relief, 0, 0, 1000, true,
"ReverseFutilityImprovingRelief", true)`. The floor keeps the invariant that RFP never reports a
fail-high from an eval below beta, whatever the tuning. The lab runs 50: half of one ply's margin,
so an improving node at depth 1-3 cuts at 50 / 150 / 250 instead of 100 / 200 / 300.

Rejected:
- **Stockfish's `margin * (depth - improving)` (relief 100):** at depth 1 it cuts at eval >= beta,
  half the slack below anything #502 measured, where 200 was already a loss in the other direction.
  50 is the smaller first step; 100 is one value change if 50 is positive.
- **Also raising the margin at a non-improving node:** a second variable in one run.

### D5: LMP: an earlier cut-off at a non-improving node

The minimum legal index becomes `kLateMovePruningMinLegalIndex - cut` (12 - cut) at a
`NotImproving` node. `TUNING_FIELD(int, late_move_pruning_not_improving_cut, 0, 0, 8, true,
"LateMovePruningNotImprovingCut", true)`; the bound keeps the index at 4 or more, well clear of the
first legal move. The lab runs 4 (index 8).

### D6: Frontier futility: a smaller margin at a non-improving node

`margin = frontier_futility_margin - (trend == NotImproving ? cut : 0)`, the same value in the
move-loop test and in the `frontier_floor` after the loop, so the fail-low floor matches what was
skipped. `TUNING_FIELD(int, frontier_futility_not_improving_cut, 0, 0, 1000, true,
"FrontierFutilityNotImprovingCut", true)`, floored at 0 in use like D4. The lab runs 50 (margin 150;
#529 found 150-250 decision-equivalent on the uniform margin, so this one is expected to be small).

### D7: One option per lab run, in order, each kept only if it ships

Each run follows #848's rule against the merge base, through `candidate_uci_options`: **ship** when
Elo minus its error bar is above 0, by flipping that option's default in a follow-up commit with an
equivalence run against the flipped build; **otherwise** remove the option before the next one
lands, so no dead option merges: an off-by-default option ends default-on or deleted. A later run's
baseline is the merge base plus whatever already shipped. If none ship, the PR closes and the
results stay in the ledger, as #849 did.

## Assumptions I cannot verify from the code

- **A1: the wide-gate cost is near #848's -1.9% probe.** That probe evaluated every node not in
  check, which is D2. Verified first, before any margin, by the bench in Validation.
- **A2: the cost at the defaults is a branch.** With every option at 0 the frame writes the sentinel
  and evaluates nothing. #849 measured -0.35% nps with its option off at the narrow gate. Re-checked
  only if a margin ships, per the regression-check rule for a default-off option.

## Invariants

- With every option at 0: node counts and best moves identical to the merge base at `Threads=1`.
- RFP never returns beta from an eval below beta; frontier futility never skips with a negative
  margin.
- A frame never reads a `ply_eval` slot its own path did not write in this search (#848's invariant).
- `frontier_floor` uses the margin the skip test used.

## Validation

**Tier: engine search change.**

1. **Bench the gate first.** A local-only build that writes the stack under D2 with no reader (gate
   forced on, every margin unchanged) is node-identical to the merge base, so `Compare-Bench.ps1`
   applies. Ordered pair (`New-OrderedBuildPair.ps1`), `-Control -Rounds 60 -Affinity 4`, about
   35 min of quiet. The verdict and interval go in the ledger and the PR. No stop rule: the cost is
   carried by whichever margin the lab measures, which times it with the tree change.
2. **Equivalence:** `Compare-SearchEquivalence.ps1` on the PR build at the defaults, after each
   option commit.
3. **Unit tests:** #848's `eval_trend()` cases and exclusion-frame test, ported to a new
   `SearchEvalTrendTests.cpp` since no single guard owns the trend. Per option, in that guard's
   existing file (`SearchFutilityTests.cpp`, `SearchLateMovePruningTests.cpp`,
   `SearchFrontierFutilityTests.cpp`): the margin arithmetic (applied only on its trend, the 0
   floor) and a reaches-the-search node-count test. `SearchTuningTests.cpp`: mirror, default,
   bounds and UCI lines. Each new test except D3's is shown to fail with its code reverted.
4. **Screen** (insight, not a gate): `Compare-SearchProfile.ps1 -Depth 12` on profile builds, at the
   default against a local default-on copy. RFP's `rfp D1/D2/D3` counts show whether the relief
   reaches the depths D2 was widened for. Then `Compare-WallClock.ps1` on shipping builds.
5. **Elo:** one strength-lab run per option at the value its Decision names,
   `gh workflow run strength.yml --ref <branch> -f candidate_uci_options="<Option>=<value>"`, about
   3 h each. The owner approves each dispatch.

## Cost

- **Size:** 50-200 lines: the ported stack (~40 lines engine, ~100 test), plus ~10 engine lines and
  a test per option, in `ThreadData.h`, `AIPerplex.{h,cpp}`, `SearchTuning.def`, the tests and
  `Docs/EngineGuide.md`.
- **Blast radius:** engine tier. No gate, skill or script changes.
- **Review:** one code review plus `search-reviewer`, per option commit set.
- **Measurement:** bench ~35 min, screen and wall clock ~45 min per option, lab ~3 h per option
  (up to three runs).
- **Optional parts:** the screen and wall clock (step 4), each ~45 min per option; they explain a
  result, the lab decides it.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2's every-depth gate and why a cap loses readers | comment at the `ply_eval` write in `pvs()` |
| D3's exclusion-frame rule, now a cost saving | same comment |
| D1's rule | `eval_trend()` comment (ported) |
| D4-D6 option meaning, floor and range | `SearchTuning.def` comments, `Docs/EngineGuide.md` |
| D6's shared margin for the floor | comment at `frontier_floor` |
| Bench, screens and lab results | `Measurements/ci-per-change.md`, headline in `Docs/Changelog.md` |
