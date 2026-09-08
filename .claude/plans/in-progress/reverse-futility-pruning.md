# Reverse futility pruning — Design

**Issue:** #87 (Stage 1). Stage 0 is #498, measured in PR #499.

## Goal

`pvs()` has TT cutoffs, null-move pruning and LMR, but no futility pruning of any kind. A shallow
non-PV node whose static evaluation already stands far enough above beta will, in the overwhelming
majority of cases, fail high anyway; searching it costs a whole subtree to learn what one evaluation
call could have told us. Stage 0 established that the evaluation such a guard needs costs **~4.1%
nps**, that **2,956,678** frames per bench run are eligible (84% of them at depth 1), and that only
**3.0%** of those are already resolved by null move — so the headroom is real and largely
unclaimed. This change adds reverse futility pruning as one isolated experiment against that
surface.

## Scope

**This change will:**

- Add a node-level reverse futility cutoff to `pvs()`, at the point where #498's probe sits — after
  the TT probe, after `in_check`, before the null-move attempt.
- Gate it exactly as singular extensions are gated: a compile-time constant
  (`kReverseFutilityCompiled`, from `-DSTRAT_REVERSE_FUTILITY`) leading every hot-path test, plus
  a runtime `SearchTuning` flag for the tests and for the node-identity proof.
- Add falsification-checked unit tests for every safety gate.

**This change will not:**

- Touch frontier or extended futility (Stage 2), the `TTEntry` layout, or the BT2630/ECM-GCP corpus.
- Tune margins or the depth band beyond the single candidate defined here. A margin sweep is a
  separate, separately screened change and only earns the right to exist if this candidate survives.
- Ship the feature on by default. The shipping build compiles it out until a strength result says
  otherwise.
- Start an SPRT. That is a paid measurement and the project owner's call, and it is reached only
  after the wall-clock gate below passes.

## Decisions

### D1: Compile-time gate as well as a runtime flag

#95 measured **1.33% nps** for a runtime-only gate on code that never executed, and #498 measured
4.1% for the evaluation itself. A runtime flag alone would therefore tax the shipping build for a
feature it does not use. Rejected: runtime flag only (cheaper diff, but a measurable permanent tax);
`#ifdef` (the disabled branch stops being type-checked and rots). The compile-time constant leads
every conjunction so the whole test folds away at level 0.

**Changed during implementation:** the CMake gate is three levels rather than an on/off option.
Level 1 — compiled in, runtime flag off — exists because the node-identity claim is about a build
that *has* the branch and does not take it, and a two-state option cannot produce that binary.

### D2: Return `beta`, fail-hard, and store nothing

The cutoff's evidence is a static evaluation, not a search. Returning `static_eval - margin` would
hand the parent a score no search ever produced, and storing it would let a speculative bound answer
a later, deeper probe. So: return `beta`, write no TT entry. Rejected: fail-soft return plus a
`LOWER` store at this depth — it is what several engines do, but it makes the cutoff's error
persistent instead of local, and Stage 1 is trying to measure one variable.

The absence of a store also removes any interaction with singular extensions through the TT: a
depth ≤ 3 futility bound would sit below singular's depth ≥ 5 trust floor even if it were written.

**The fail-hard return is what makes the rest of the guarantees hold**, which the search review
traced and which anyone attempting fail-soft must read first. Every caller that can reach the guard
passes a null window, so `beta` arrives at the parent as exactly its `alpha`: no improvement, so no
killer, history or PV write; and a null-move child returns `beta - 1`, one below the cutoff that
would otherwise reach `tt.store(... LOWER, CUT_NODE)`. "No TT store" is therefore a property of the
return value, not only of the guard.

### D3: Candidate parameters — margin `100 * depth`, `depth <= 3`

The margin is one pawn per remaining ply, on the engine's centipawn scale (`g_iPieceValues[0] ==
100`). The depth band is where the eligible frames actually are: Stage 0 found depths 1-3 hold
**95.4%** of them, so a wider band buys almost no extra surface while widening the risk of pruning a
node whose evaluation is stale by more than the margin. Rejected: a deeper band with a
larger-per-ply margin (more surface, but the two variables move together and a failure becomes
unattributable).

### D4: Zugzwang guard copied from null move, not omitted

Reverse futility asserts "this position is already good enough"; in a zugzwang-bound endgame the
side to move must destroy that assessment. `should_try_null_move()` already refuses below two
non-pawn pieces (#66: KQ vs KR). The futility guard reuses the same test rather than assuming the
static evaluation is trustworthy there. Rejected: no material guard (this is the failure mode
zugzwang endgames are famous for, and #66 is a measured precedent in this engine).

### D5: The exclusion frame stays excluded

A singular verification search's fail-low is what grants the extension. If futility can produce that
fail-low, the extension is granted because futility said so, not because the search did. `#498`'s
probe already excludes exclusion frames and every number quoted above is outside them, so keeping
the exclusion also keeps the measurement applicable. The 8.0% extension rate measured on #95's tree
is the tripwire for anyone who later wants to revisit this.

### D6: The cutoff runs before move generation, so a stalemate node can be cut

The guard sits above `ComputeLegalMoves()`, so a node with no legal move returns `beta` instead of a
terminal score. Accepted, on three grounds: checkmate is unreachable (the in-check guard); the
stalemated side must also hold two non-pawn pieces and evaluate above beta plus the margin, which is
a composed-position class rather than a game one; and the node is searched properly once iterative
deepening passes the depth band. Rejected: generating moves before the cutoff, which is the entire
cost the cutoff exists to avoid.

**The error propagates, and it is not confined to this visit.** The cut node itself stores nothing,
but its caller can: every caller reaching the guard passes a null window, so `beta` arrives at the
parent as exactly its `alpha`, and a parent no sibling improves stores an `UPPER` bound at that
`alpha`. Where the true value is the draw score 0 and the parent's `alpha` sits below it, that bound
is false and can hide a drawing move from a later probe. This is what an incorrect fail-high from
*any* pruning heuristic does in this search — null move included — rather than something stalemate
introduces; what stalemate contributes is a case where the gap between the returned score and the
truth is not bounded by the margin. Accepted as a heuristic risk and written down rather than
mitigated, because mitigation means generating moves at the node.

**Both knobs in D3 enlarge this hole** — a wider band or a lower material floor makes it more
reachable. `SearchFutilityTests.cpp` pins the behaviour with a composed stalemate (verified against
python-chess) so that whoever moves them has to read about it.

## Assumptions I cannot verify from the code

- **Stage 0's eligibility counts describe this guard's surface.** The probe's `futility_probe_reverse`
  predicate is `!is_pv_node && !in_check && !is_exclusion_frame && |beta| < Mate_Threshold`. This
  design adds two further conditions (depth band, zugzwang material), so the real surface is
  *smaller* than 2,956,678 — bounded below by the 95.4% depth figure and by an unmeasured material
  fraction. Not verified, and it does not need to be: it only makes the estimate conservative.
- **Part of the 4.1% evaluation cost returns as recovered work.** A depth-1 cutoff avoids a
  quiescence search that would have called `Evaluate()` anyway. Unmeasured, and deliberately so —
  the wall-clock gate below measures the net directly, which is the number that matters.

## Invariants

- At `STRAT_REVERSE_FUTILITY=0` the engine is behaviourally the baseline: no
  evaluation call, no branch on the hot path.
- With the feature compiled in and `reverse_futility_enabled = false`, the search is
  **node-identical** to the baseline at `Threads=1` (`Compare-SearchEquivalence.ps1`).
- No futility cutoff is ever taken at a PV node, in check, in an exclusion frame, against a
  mate-range beta, above the depth band, or below the zugzwang material floor.
- No TT entry is written by the cutoff.

## Validation

Tier: search change, so the full ladder — but ordered so the cheap falsification runs first.

1. **Wall clock at fixed depth 12, interleaved against the exact merge-base binary, `Threads=1`.**
   Futility is a tree-size change, so this is its cheap local falsification: **down, or the candidate
   is parked**. The instrument is wall clock and not nps, because the change alters the node count on
   purpose and an nps comparison would be comparing two different searches.
2. Node identity with the runtime flag off (`Compare-SearchEquivalence.ps1`).
3. Full test suite, the 36-position tactical suite including stability mode, and `Run-Bench.ps1`
   with main and qsearch node movement reported separately.
4. Falsification-checked unit tests per safety gate: PV node, in check, exclusion frame, mate-range
   beta, depth above the band, material below the floor, and the tuning flag itself. Each is proved
   by removing the guard and watching the test fail.
5. Search-specialist review (`search-reviewer`).
6. **Only then**, and only on the project owner's decision, an isolated SPRT against the merge-base
   binary. Node reduction is not acceptance.

If step 1 fails, the candidate is removed and the measurement recorded on #87; no match is run to
look for a strength result the instrument already denied.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the compile-time constant leads the conjunction (D1) | source comment at the gate, mirroring the singular one |
| Fail-hard return with no TT store, and why (D2) | source comment at the cutoff |
| Zugzwang floor shared with null move (D4) | source comment referencing the shared reasoning |
| Exclusion frame must stay excluded, and the 8.0% tripwire (D5) | source comment at the gate |
| Candidate parameters and the 95.4% depth figure behind them (D3) | comment on the `SearchTuning` knobs |
| Wall-clock-at-fixed-depth result, either sign | `Docs/Changelog.md`, the PR body, and a comment on #87 |

## Result

Step 1 passed decisively: **wall clock -38.5%** at fixed depth 12, `Threads=1` (paired per-round
median over 9 interleaved rounds, range -41.5 to -37.2%), main nodes 13,004,919 → 9,229,827 and
quiescence nodes 4,691,063 → 2,620,683. Node identity at level 1 is IDENTICAL against `origin/main`
over 90 compared lines. All 626 fast-tier tests pass in the shipped configuration; with the feature
forced on, the only failure is the test that asserts the shipped configuration does not prune, and
the 36-position tactical suite still passes. Every one of the six guards was falsified by removing
it and watching the suite fail.

The search review returned LGTM with no correctness defect. Its non-blocking observations, parked
rather than actioned here: `has_two_non_pawn_pieces()` now runs twice on a node that is eligible for
both guards, worth a bench pass and a hoist in Stage 2; the zugzwang floor is arguably too strong for
reverse futility, since it hands the opponent no free move, and a softer floor is its own experiment;
and the tactical suite is worth one run above the depth band before any decision to ship.

What remains is the SPRT, which is the project owner's budget call. Nothing measured here is a
strength result: a smaller tree at fixed depth is the *precondition* this candidate had to meet, not
evidence that the moves it now makes are better.
