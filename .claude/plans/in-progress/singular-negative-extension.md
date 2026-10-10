# Singular negative extension — Design

**Issue:** #798 (slice 2 of #721)

## Goal

When the singular verification fails high, the hash move is not singular: an alternative also
reaches `singular_beta`. Multi-cut (#795, shipped on) returns when `singular_beta >= beta`. Below
that, in the band `singular_beta < beta <= tt_value`, the hash move's own TT entry still claims a
cutoff, but the node searches it at full depth (`depth - 1`). The node has a second move that comes
close to beta and a hash move whose shallower entry already says it cuts. A ply spent on that hash
move is the least informative ply in the node. Spike #792 counted 40.6% of verifications with
`tt_value >= beta` after a fail-high. Every multi-cut candidate is one of them, so about 26% of
verifications remain on today's baseline. Negative extension searches the hash move one or two
plies shallower there. Whether the saved work buys more Elo than the accuracy it costs, only the lab can
tell.

## Review focus

- **The fail-high at reduced depth is final (D4).** A reduced hash-move search that fails high cuts
  the node with no re-search, and the node then stores a LOWER bound at `depth`. That bound rests on
  a `depth - 2` child. This is the decision most likely to be argued, and its error is the risk that
  decides whether the change pays.
- **D3, signed depth arithmetic.** Today `singular_extension` is read as a Boolean. Assigning `-1`
  to it unchanged would *extend* the move. The rewrite must make that impossible and be observable
  in a test.
- **D2, PV exclusion and the mate-range `beta` guard.** PV costs nothing on the spike's counts
  (14 of 153,843), but departs from Stockfish 15's PV-inclusive rule.

## Scope

**This change will:**

- add a runtime int option `SingularNegativeExtension`, the reduction in plies (0 = off, the
  default; 1 or 2), and a pure eligibility helper;
- replace the Boolean `singular_extension` with a signed hash-move depth adjustment;
- count granted reductions on a new `info string` line (D6);
- run one CI lab measurement at 1 ply, which decides only whether the default becomes 1 (D7).

**This change will not:**

- change multi-cut, the verification's window, depth or return semantics, or the eligibility gate;
- add double extension (#794) or any reduction larger than two plies;
- reduce any move but the hash move, or touch LMR;
- retune any existing singular default.

## Decisions

### D1: Trigger

The hash move is reduced when **all** of these hold, tested after the multi-cut test, so multi-cut
has already returned if it was going to:

- `tuning_.singular_negative_extension_plies > 0`;
- `verify_value >= singular_beta`, the verification failed high;
- `tt_value_for_singular >= beta`;
- `!is_pv_node` (D2);
- `std::abs(beta) < GameValues::Mate_Threshold` (D2).

Helper, beside `singular_multicut_eligible()`:
`bool singular_negative_extension_eligible(int verify_value, int singular_beta, int tt_value, int beta, bool is_pv_node) const`.
`singular_beta` is a parameter only for the fail-high test, so the helper matches its sibling and is
unit-testable without a tree.

The helper does not test `singular_beta < beta`. With multi-cut on, placement after its return
guarantees it: both helpers carry the same mate-range `beta` guard, so any non-mate `beta` with
`singular_beta >= beta` has already returned. With multi-cut off, the band widens to every
fail-high with `tt_value >= beta`, which is the spike's 40.6% and Stockfish 15's ordering. That is
correct behaviour for the option combination, not an accident.

Rejected: Stockfish's later rules that also reduce when `cut_node`, or when `tt_value <= value`.
This engine has no cut-node flag, and the second rule has no spike count.

### D2: Non-PV only, non-mate beta

A PV node's first move defines the PV. Reducing it would trade PV accuracy for 14 of 153,843
spike candidates (about 0.009%), which is not a trade worth testing. Rejected: Stockfish 15's
PV-inclusive form.

**Mate-range `beta` guard**, the same as multi-cut's. Only a negative mate-range `beta` can reach
the trigger, because `tt_value` is non-mate by the eligibility gate. There, a reduced fail-high
would let the node store a LOWER bound at `depth` that claims "not mated within this distance" on
reduced-depth evidence (D4). The guard keeps mate-distance claims backed by a full-depth search, and
makes D1's band hold exactly when multi-cut is on. Rejected: no guard. That widens the trigger past
the documented band, for nodes too rare to matter to Elo.

### D3: Signed child depth

`int singular_extension` becomes `int hash_move_depth_adjust`, with a value in `[-2, +1]`. The
first-move site computes:

```cpp
const int child_depth = depth - 1 + (move == hash_move ? hash_move_depth_adjust : 0);
```

The rename forces every reader to be revisited, so no Boolean test (`!= 0`) survives on a value that
can now be negative. Fail-low sets `+1`. The negative-extension branch sets
`-tuning_.singular_negative_extension_plies`. The two are
exclusive, because fail-low and fail-high are complements.

No depth clamp. At the default `singular_min_depth` of 6 the shallowest child is depth 3.
`child_depth` reaches 0 or below only when `singular_min_depth <= 3`, which the option range allows
(minimum 1). `pvs()` sends `depth <= 0` straight to `quiescence()`, which is the
right search for that horizon. The child is a normal frame, not an exclusion frame, so the
verification's "never fall through to quiescence" clamp does not apply.

### D4: No re-search

A reduced hash-move result is final, either way:

- **Fail-low or in-window:** the node continues to its other moves as today. These now include the
  alternative that the verification said reaches `singular_beta`.
- **Fail-high:** the node cuts and stores a LOWER bound at `depth`, as any first-move cutoff does.

Rejected: re-search at `depth - 1` on fail-high, as LMR does. The trigger already says the hash move
cuts, by a shallower TT entry. A re-search would remove the saving in exactly the case the trigger
expects; only the reduced fail-lows would still save work. Stockfish 15 takes no re-search here.

This is a stronger speculation than LMR's. LMR re-searches any reduced result above alpha, so it
speculates only on fail-lows. Here an unconfirmed reduced fail-high becomes a LOWER bound at
`depth`. Multi-cut's parent accepts the same direction of speculation. The Review focus names this,
and the lab measures it.

### D5: Runtime option, not a compile gate

`TUNING_FIELD(int, singular_negative_extension_plies, 0, 0, 2, false, "SingularNegativeExtension", true)`,
placed after `SingularMultiCut` in `SearchTuning.def`. 0 is off. The field is read only after a
verification has failed high, so it is off the hot path. It is meaningful only while
`SingularExtensions` is on.

An int, not a bool, so a second size is a lab run, not a code change (D7). The range 1-2 is this
engine's experimental range. 2 is Stockfish 15's size for this branch (`sf_15` `search.cpp`
997-1018); Stockfish 16 uses 3 at non-PV nodes, not adopted here. The cap of 2 keeps the child at
depth 3 or more under the shipped `singular_min_depth`. Rejected: a bool fixed at one ply. A failed
1-ply run would then force deletion, or a code change to try Stockfish 15's size.

### D6: Telemetry

`SingularStats` gains `negative_extensions`: completed verifications that grant a negative
adjustment, the same semantics as `extensions` for +1. It is incremented where the adjustment is set,
after the verification's abort guard. It does not count completed reduced child searches: the move
can still fail legality or the child can abort. `add()` sums it, and `append_info` emits it as its
own payload, `singular negext <n>`, only when non-zero. An option-off run's output stays
byte-identical, and the `singular eligible …` wording does not change.

### D7: Ship rule

The lab run measures 1 ply, the conservative size, and decides only whether it is enabled on main.

- **Lower bound above 0:** the default becomes 1.
- **Otherwise:** the default stays 0.

2 plies is a separate issue (#803), filed whatever this run shows and measured against main as it
stands when picked up. It owns the default-off state, which is not an end state: it ends on by
default or deleted. 1 ply goes first because Stockfish's larger reductions were tuned on a far
deeper search with different pruning. A failed 1 ply does not show that 2 plies will fail, because
a reduction too timid to pay for the lost accuracy is a plausible failure mode.


## Assumptions I cannot verify from the code

- **The option reaches the engine in the lab.** It is set on the candidate only. Verified before the
  run by a short local UCI session with `setoption name SingularNegativeExtension value 1`,
  showing a non-zero `singular negext` line. Done: 6,946 of 26,768 verifications. The lab run pins `reference_ref` to the
  candidate SHA and keeps the resolved-options artifact for both sides.
- **The spike's 26% carries to game play.** That figure is the 40.6% minus 14.4% on one corpus at
  depth 12, Threads=1. The ordering change from multi-cut may also shift it. A UCI run over the
  bench positions reports the actual count. The decision rests on Elo, not on the rate.

## Invariants

- With the option at 0, search is node-identical to `origin/main` at Threads=1.
- The hash move is reduced only after a completed verification. An aborted one still returns
  `best_value` from the existing guard.
- A reduction never applies to a move other than the first legal move, and only when that move is
  the hash move. The existing `move_number == 0` and `move == hash_move` checks are kept.
- The reduction never happens at a PV node, after a fail-low, at a mate-range `beta`, or when
  `tt_value < beta`. When multi-cut fires, the node returns before any move is searched.
- A fail-low still extends by exactly +1. A negative adjustment never extends.

## Validation

Search tier, with search-reviewer review.

- **Unit tests** in `StratChessTests/SearchSingularTests.cpp`:
  - helper boundaries: `verify_value == singular_beta` reduces and `singular_beta - 1` does not;
    `tt_value == beta` reduces and `beta - 1` does not; PV; the option at 0; a negative mate-range
    `beta` does not reduce and `-Mate_Threshold + 1` does;
  - a search-level case that observes the **child depth**. It reuses the multi-cut test's setup
    (`kLowValue` LOWER entry at `kDepth - 1`) with `beta = kLowValue`: above `singular_beta`, so
    multi-cut does not fire, and at `tt_value`, the boundary that must reduce. Null move, RFP, LMP
    and frontier futility are off: at `kDepth = 4` the reduced child sits at LMP's depth, and an
    LMP-clipped fail-low stores nothing. It is called non-PV. The hash child's entry is asserted
    absent before the search and a MAIN entry after it, at depth `kDepth - 2` at 1, `kDepth - 3`
    at 2 and `kDepth - 1` at 0. `negative_extensions` is 1, 1 and 0. The child's own store is what
    discriminates the runs; the node's return value alone might coincide. The fixture gains one
    accessor that probes the TT entry of the position after a given move. Where the node fails
    high, the test also asserts the return and the node's own MAIN LOWER entry at `kDepth` naming
    the hash move, which pins D4;
  - the first-legal-move re-check (`move_number == 0 && move == hash_move`) has no dedicated test;
    it is preserved by inspection, since D3 changes only the adjustment it gates;
  - the existing fail-low extension test keeps passing unchanged, so `+1` still reaches the child.
- **Equivalence:** `Compare-SearchEquivalence.ps1 -Before <origin/main exe> -After <branch exe>
  -Positions Tests/profile-screen.fen -Depth 12` must report IDENTICAL, with the option at 0.
- **Bench:** bench a local build with the default set to 1 (not committed) against the branch
  build. Record node and nps deltas in the PR. `Run-Bench.ps1` has no `negext` column, so the
  count comes from a separate UCI run at 1.
- **Elo:** CI strength lab, one binary (the branch SHA on both sides),
  `candidate_uci_options: SingularNegativeExtension=1`, multi-cut on both sides (its default),
  Threads=1, 10+0.1, **one 19,980-game run**. **Default 1 if and only if the interval's lower bound
  is above 0; otherwise default 0 (D7).** That rule is fixed before
  the run, with no confirmation or pooling afterwards. Record the run in
  `Measurements/ci-per-change.md`. #721 closes once negative extension ships or is deleted.

## Cost

- **Size:** 50–200 lines across `SearchTuning.def`, `AIPerplex.{h,cpp}`, `SearchTelemetry.h`, the
  test fixture and `SearchSingularTests.cpp`. On a pass, a follow-up commit sets the default to 1.
- **Blast radius:** search tier. The UCI option table gains one entry.
- **Review:** one search-reviewer pass plus the code review (170–270k tokens, 3–5 min).
- **Lab:** one 19,980-game run at Threads=1, about 3–3.5 h. The size-2 issue costs one more run of
  the same size and no code.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 trigger, its dependence on multi-cut's placement; D2 mate guard | comment on `singular_negative_extension_eligible()` |
| D3 signed adjustment, no clamp; D4 no re-search | comment at the adjustment site in `pvs()` |
| D6 separate telemetry line | `SingularStats` member comment |
| Lab result and keep-off decision | `Measurements/ci-per-change.md`, `Docs/Changelog.md`, the PR body |
| D7 size-2 follow-up | #803 |
| Option semantics | `SearchTuning.def` comment |

**Outcome:** lab -1.46 +/- 3.54 at 1 ply (run 38079265803, interval [-5.00, +2.08]); the default
stays 0.

**Decision changed during implementation:** D7. The owner split the 2-ply measurement from this
run: #803 is filed whatever the 1-ply result, and measured against main at pickup, so this run
decides only enable or stay off. Deletion moved to #803.
