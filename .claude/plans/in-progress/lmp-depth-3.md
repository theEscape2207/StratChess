# Late move pruning at parent depth 3 — Design

**Issue:** #895

## Goal

Late move pruning (LMP) fires only at parent depth 2. At depth 3, late quiet moves are a large share
of the tree and rarely matter: on `18d29644`, the moves LMP's guards would skip from index 8 hold an
upper bound of 15.6-16.2% of nodes and beat alpha 0.106-0.111% of the time, against 0.146% for
depth 2 at its shipped index (#895 triage). Whether that is worth Elo is unknown, and node screens
have not predicted it here (#545, #634, #664). This change makes depth-3 LMP measurable behind a
default-off gate, so one lab run can decide whether it ships or is deleted.

## Review focus

- **D3, the UPPER store at alpha.** It keeps depth-3 frames' entries, but depth-1 frames and quiescence
  reuse a bound whose skips they would not repeat. Is that reuse safe enough to run?
- **D2, the single arm at index 8.** Half the wrong skips sit in indices 8-11, for about 17% of the
  savings, and a depth-3 cut has survived two searches where depth 2's survived one.
- **D4, no guard beyond depth 2's.** Quiet SEE pruning carries a mated term and a two-non-pawn gate.
  LMP has neither; the split in D4 is the case for leaving them out.

## Scope

**This change will:**

- Make depth 3 eligible for LMP behind a default-off flag, with its own move index.
- Store UPPER at alpha on a depth-3 fail-low that skipped a late move.
- Run one lab row, on against off, and flip the default or delete the gate under the rule below.

**This change will not:**

- Touch depth 2: its index, return and missing store stay as they are. #940 tests that store.
- Add depth 1, parked by #894: its cut rate is 2.6-3x depth 2's, and it overlaps frontier futility.
- Generalise to a per-depth table or a `LateMovePruningMaxDepth`.
- Change the guards at the skip site, or add a telemetry counter per depth.

## Decisions

### D1: A separate flag, index and depth constant

The later slices rely on these names:

| Item | Name | Value |
|---|---|---|
| Flag field, UCI check | `late_move_pruning_depth3_enabled`, `LateMovePruningDepth3` | default `false` |
| Index field, UCI spin | `late_move_pruning_depth3_min_index`, `LateMovePruningDepth3MinIndex` | default 8, range 1-64 |
| Constant, `AIPerplex.h` | `kLateMovePruningDepth3` | 3 |

`late_move_pruning_eligible()` keeps its signature and accepts `depth == kLateMovePruningDepth3` only
when the flag is on and `late_move_pruning_enabled` is too, so `LateMovePruning=false` still turns
off all LMP. `pvs()` picks the index once per node for `lmp_candidate`. `kLateMovePruningDepth` keeps
its value; its "exactly two" comment and the `SearchTuning.def` comment are rewritten.

Rejected: a per-depth array plus a max-depth option. One run needs one extra depth, and the end state
is a default-on bool or deletion, so the general form would be debt either way.

### D2: One arm, at index 8

Index 8's cut rate (0.106-0.111%) is below depth 2's at its shipped index, and it saves about 3
points more than 12 (15.6-16.2% against 13.0-13.4%). Depth 2's own move from 12 to 8 gained
+3.98 +/- 2.95 (#869), and going on to 6 did not improve on 8 (-1.76 +/- 2.95, #888).

The aggregate hides the marginal band, so the owner should weigh this table (split in Validation's
probe, shipped tree):

| Set | Beat-alpha, 8-11 | Depth 2's, 8-11 | Share of depth-3 cuts in 8-11 | Share of savings in 8-11 |
|---|---|---|---|---|
| 200 pos, d12 | 0.365% | 0.506% | 48% | 16.5% |
| 50 pos, d14 | 0.378% | not run | 50% | 17.5% |

So 8 stays below depth 2's rate in the band that decides it too, but this understates a depth-3 miss:
at index 8 and above the scout runs at child depth 1 and re-searches at depth 2, so a depth-3 cut has
survived two searches. Index 8 is the higher-risk first arm, chosen because it is the one with lab
support at depth 2.

Rejected: one arm at 12, the conservative choice, which keeps about 83% of the savings. It stays the
next step on a miss (Validation). Rejected: a two-arm screen and confirm, as #96 did, because it
costs about 4 h more lab and a confirm run. **The owner chose one arm at 8 at approval (2026-11-03).**

### D3: A depth-3 skip stores UPPER at alpha

The `quiet_see_skipped` floor in `pvs()` also takes `lmp_skipped && depth == kLateMovePruningDepth3`,
and the store-nothing return narrows to `depth == kLateMovePruningDepth`. A depth-3 fail-low that
skipped a late move then falls through and stores UPPER at alpha with the best searched move, then
returns alpha. A searched cutoff after a skip stores LOWER, as at depth 2.

Depth 3 is different because 89.6-94.1% of its completed non-PV fail-lows store UPPER at alpha today,
through quiet SEE pruning, which gained +8.84 +/- 3.02 (#399). At index 8, depth-2's return would
remove the store from 98.3-98.9% of depth-3 fail-lows, and with it the best move that stops a later
visit from applying IIR at `IirMinDepth=4` and above.

Rejected: no store, as depth 2 does. The run would measure the pruning and the lost stores as one
result, so a loss could not say which of them caused it.

This does not isolate the pruning completely, and the lab row is read that way:

- **A depth-2 probe adds no exposure.** At the default indices it would skip those same late moves,
  ordering aside, with weaker evidence and no store.
- **Depth 1 and quiescence do.** A depth-1 frame or a quiescence probe reuses the bound and has no
  matching skip, so #399's monotonic argument does not cover them. A same-depth revisit with new
  killers or order is in the same class. #399's review accepted this class.
- **A wrong skip can reach higher.** In a null-move child at parent depth 7 it writes a depth-7 LOWER,
  the same class as SEE but higher than depth 2's store can reach.
- **It usually corrects itself.** The next iteration normally visits at depth 4, where the entry
  cannot cut, and its stored move blocks IIR, so that visit gets a full frame with no LMP. Alpha also
  answers fewer probes than a fail-soft value would, since it only serves alpha' >= alpha.
- **Some frames gain a selective bound they lack today.** Without two non-pawn pieces SEE never fires,
  so those frames store a fully searched fail-soft UPPER now. Those with a late move are 3.8-7.7% of
  depth-3 fail-lows. Elsewhere the bound's basis weakens from moves judged losing to moves judged by
  list position.

**Order with #940:** the two lab runs stay serial. If #940's default-off code lands first, this change
reconciles with its gate without turning its behaviour on. If #940 ships, the depth-2 return is gone and
the floor's condition drops to `lmp_skipped`. Its result does not transfer to depth 3, in either
direction.

### D4: Same guards as depth 2, nothing added

Non-PV, null window, out of check, no exclusion frame, alpha and beta outside the mate range; the move
is not a capture, promotion, killer or hash move, gives no check and draws nothing. Depth is read
after IIR's decrement, so a no-hash depth-4 frame searched at 3 is eligible. The probes counted these
frames the same way, and such a frame has the weakest ordering, so it is the likeliest place for a
wrong skip.

Quiet SEE pruning carries two more guards, and depth-3 LMP takes moves it would have left alone under
them. The rule, proposed by this design's domain review before the split ran: add a guard if its class beats alpha
at 3x the rest or more. Neither qualifies. Beat-alpha rate at depth 3, index 8 and above:

| Class | 200 pos, d12 | 50 pos, d14 |
|---|---|---|
| Two or more non-pawn pieces, not mated | 0.107% | 0.114% |
| One non-pawn piece | 0.066% | 0.035% |
| Pawn-only | 0 of 13,426 | 3 of 60,763 |
| Every searched move mated | 1 of 15 | 0 of 1 |

Rejected: `zugzwang_safe`. Those frames cut less often than the rest, not more, and hold 1.5-3.5% of
the savings. Rejected: `best_value > -Mate_Threshold`. Too rare to measure (16 moves in 36M), so the
term would have no measurable effect on the run; these frames also return alpha rather than their
mated fail-soft value, the same as depth 2's.

### D5: The order against LMR and SEE stays as it is

The skip's `continue` precedes `apply_lmr` and `record_lmr_reach`, so a pruned move is neither reduced
nor counted as reduced. No code changes for this; a test pins it. LMP also takes precedence over SEE
through `!lmp_candidate`, so at index 8 it takes over 89% of depth-3 SEE skips (71-72% at 12). These
move from `quietsee skips` to `lmp skips` and are not new savings; the savings figures above already
exclude them. `lmp skips` stays one counter.

## Assumptions I cannot verify from the code

- **The savings and fail-low shares hold once the tree changes.** Both come from a counting build on
  the unpruned tree, so savings are an upper bound and the shares assume those frames still fail low.
  They were not re-run for this document; the baseline is the probe's commit, `18d29644`. Only the
  lab settles it, and the row does.
- **The option reaches the lab engines.** Settled as #888 did: lab PGN scores reproduced by a local
  `go depth` under each arm's options. Differing node counts alone are weaker evidence.

## Invariants

- With the flag off, the search is identical to the merge base.
- Depth 2's eligibility, index, return and missing store do not depend on the depth-3 options, and
  depth 2's index does not move depth 3's threshold.
- `LateMovePruning=false` disables both depths.
- A depth-3 skip takes no PV, killer or history write, runs no child and precedes the abort guard, the
  same contract as depth 2's.
- The first legal move is always searched: the index range starts at 1.
- A depth-3 fail-low with a skip returns alpha and stores UPPER at alpha, never below it.

## Validation

**Engine tier, implementation PR, default off:**

- **Equivalence:** `Compare-SearchEquivalence.ps1` against the merge base with the flag off: identical
  nodes and best moves.
- **Speed:** one `Compare-Bench.ps1` with the flag off. The on-against-off lab run uses one binary, so
  it cannot charge the new branch. Node equivalence says nothing about nps (#555). A slowdown beyond
  the series' spread follows `measure-strength/reference/regression-check.md`, including its relink for
  placement and its deferral of escalation until the lab has decided. If a material cost remains once
  the gate is staying, remove it or measure the enabled change against its merge base before claiming
  a net gain. An explanation alone does not waive it; deleting the gate after a miss removes it.
- **Tests, each falsified by mutation** (`[search][lmp]`, `SearchTuningTests.cpp`):
  - Eligibility: depth 3 only with the flag on, depth 4 never, depth 2 either way, and none with
    `LateMovePruning=false`.
  - Each index moves only its own depth's threshold.
  - Depth-3 fail-low with a skip: returns alpha and replaces a shallower planted entry with UPPER at
    alpha at depth 3, carrying a searched best move. Depth 2's store-nothing test stays unchanged.
  - Depth-3 searched cutoff after a skip stores LOWER.
  - A skipped move is not reduced: at a depth-3 node that fails low in both arms, flag on against off,
    `lmr.capped` (written on every depth-3 reach) falls by exactly `lmp_skips()`, and that count matches
    an independent tally of the parent's candidates. Two settings isolate the parent:
    `QuietSeePruning=false`, because a move LMP takes over from SEE reaches no LMR in the off arm
    either; and `LateMovePruningMinIndex=64`, because `lmp_skips()` also counts skips in depth-2
    children, so the off arm must show 0. Test builds always compile the search-profile counters, so a
    skip moved below `record_lmr_reach` fails this.
  - Option parse, range (0 and 65 rejected) and the UCI catalogue lines.
- **Option applied:** a local `go depth 12` with the flag on shows higher `lmp skips` than off, and
  different nodes.

**Lab run, once the owner approves:** option-only on `main` after the default-off PR merges,
`LateMovePruningDepth3=true` against the same commit with it off, 10+0.1, `Threads=1`, 18 shards x 740 pairs (26,640 games, about
4 h), not overlapping #940's run.

**Ship rule, declared before dispatch:** the default flips on only if Elo minus its error bar is above
0. Either way the row goes in `Measurements/ci-per-change.md`. On a miss the owner chooses between a run
at index 12 and deleting the gate, as #399 kept arm B in reserve. At a bar of about +/-3, a true +3 passes
only about half the time (reviewer's estimate, not simulated), so a deletion records that index 12 and
the no-store variant were untested, and a miss cannot say whether the pruning or the bound caused it.

**Probe behind D2 and D4:** a throwaway counting patch on `18d29644`, `Tests/profile-screen.fen`,
`Threads=1`, `ucinewgame` per position: 200 positions at d12 and 50 at d14 on the shipped tree, and for
the depth-2 reference 200 at d12 with `LateMovePruningMinIndex=64`. Its depth-3 totals match the #895
triage's. Patch, driver and raw counts: `StratChessSupport\DesignRecords\895 - lmp-depth-3.probe\`.

## Cost

- **Size:** 50-200 lines, about 11 files (`AIPerplex.cpp/.h`, `SearchTuning.def`, the LMP and tuning
  tests with their fixture, `Docs/EngineContracts.md`, `Architecture.md`, `EngineGuide.md`,
  `TestDesign.md`, Changelog).
- **Blast radius:** Engine tier and the option catalogue tests. Label `uci-facing`.
- **Review:** full code review plus `search-reviewer` on the diff.
- **Lab:** about 4 h. A local bench pass of about 5 min needs a quiet window.
- **Optional, a second screen arm at 12:** about 4 h plus a confirm run. It covers picking the wrong
  index. Dropped by default in D2.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why depth 3 stores and depth 2 does not (D3) | comment at the floor in `pvs()` and at the LMP return, whose "no UPPER" reasoning D3 contradicts at depth 3; the `See::see_ge` bullet in `Docs/EngineContracts.md`, which today says LMP stores nothing |
| Why no mated or two-non-pawn guard (D4) | comment at `lmp_candidate` |
| The options and their index | `SearchTuning.def` comment; `Docs/Architecture.md` LMP row |
| Skip moves between counters (D5) | `lmp skips` row in `Docs/EngineGuide.md` |
| Test coverage | `Docs/TestDesign.md` LMP row |
| The lab result and the applied rule | `Measurements/ci-per-change.md`, headline in `Docs/Changelog.md` |
