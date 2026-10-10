# Singular multi-cut — Design

**Issue:** #795 (slice 1 of #721)

## Goal

When the singular verification fails high, the hash move is not singular: some alternative also
reaches `singular_beta`. If `singular_beta >= beta` as well, the node has two moves that reach the
cutoff: the hash move by its TT lower bound, the alternative by the verification. It still searches
every move at full depth. Spike #792 (results on #721) found this on 14.4% of verifications, all at
non-PV nodes and spread across the whole corpus. Multi-cut returns at that point instead. Whether
the saved work buys Elo, or the lost accuracy costs it, only the lab can tell.

This fires only where the TT entry could **not** already cut. A LOWER or EXACT entry with
`tt_value >= singular_beta >= beta` and `entry->depth >= depth` returns at the TT probe. So a
multi-cut is always a shallower entry (depth `depth - 3` to `depth - 1` under the shipped margin)
plus one reduced-depth alternative, both at or above `beta`.

## Review focus

- **D2, fail-hard `beta` rather than Stockfish's `singular_beta`.** This is the decision most likely
  to be argued. The rationale is the repo's convention for speculative cutoffs (RFP), and the
  evidence for it is reduced-depth.
- **D3, no TT store.** A cut node that stores nothing gets re-searched on transposition. The
  alternative risks a reduced-depth bound answering a full-depth probe.
- **Strength:** the risk that decides whether this pays is a wrong cut, where the alternative's
  reduced-depth fail-high does not hold at full depth. Only the lab can measure that.

## Scope

**This change will:**

- add a runtime option `SingularMultiCut` (default off) and a pure eligibility helper;
- return from a qualifying node right after the verification's abort guard;
- count completed cuts on a new `info string` line;
- run one CI lab measurement, then either flip the default to on or delete the feature.

**This change will not:**

- add negative extension, signed depth arithmetic or double extension (the next #721 slice and
  #794);
- change the verification's window, depth, return semantics or eligibility gate;
- retune any existing singular default.

## Decisions

### D1: Trigger

The cut fires when **all** of these hold, tested after the abort guard that follows the
verification:

- `tuning_.singular_multicut_enabled` is set;
- `verify_value >= singular_beta`, meaning the verification failed high (the complement of the
  existing strict fail-low test);
- `singular_beta >= beta`;
- `!is_pv_node` (D4);
- `std::abs(beta) < GameValues::Mate_Threshold` (D5).

It lives in a member helper beside the other guards,
`bool singular_multicut_eligible(int verify_value, int singular_beta, int beta, bool is_pv_node) const`,
so the boundaries are unit-testable without building a search tree. That matches
`reverse_futility_eligible()`.

Rejected: Stockfish's later variant, which also cuts when `tt_value >= beta` and reduces instead.
That is negative extension, the next slice.

### D2: Return `beta`, fail-hard

Rejected: `singular_beta`, Stockfish's historical fail-soft return. The evidence for the node is a
TT bound from a shallower search plus one reduced-depth null-window search. That is speculative in
the same way RFP's static evaluation is, and RFP returns `beta` for the reasons its comment gives
(`AIPerplex.cpp`, reverse futility block). Every caller here passes a null window, so `beta` lands
at the parent exactly as its alpha. The parent then makes no killer, history or PV write, and a
null-move child yields `beta - 1`, so it cannot fabricate a null-move cutoff. A fail-soft return
above `beta` would also let the parent store a tighter UPPER bound than any full-depth search
supports.

### D3: No TT store, no killer or history write

The node returns before its move loop, so it reaches no store site. None is added. Storing a LOWER
bound at `depth` would let a reduced-depth result answer later full-depth probes. The entry that
made the node eligible stays in the table unchanged.

### D4: Never at a PV node

A PV node that returns `beta` would leave an empty PV row, and its caller would read a bound as a
principal-variation score. The spike found one PV candidate in 378,835 verifications, so the
exclusion costs nothing.

### D5: Never with a mate-score `beta`

Same reasoning as RFP's guard: a cutoff fabricated in mate range claims a mate no search found.
`tt_value` is already non-mate by the eligibility gate.

### D6: Runtime option, not a compile gate

`TUNING_FIELD(bool, singular_multicut_enabled, false, false, true, true, "SingularMultiCut", true)`,
placed after the singular block in `SearchTuning.def`. The end state is on by default or deleted.
The bool is read only after a verification has failed high, so its cost is off the hot path. It is
meaningful only while `SingularExtensions` is on, because no verification runs otherwise.

### D7: Telemetry on its own line

`SingularStats` gains `multicuts`, incremented after the abort guard because a cut is a result.
`append_info` emits it as a separate payload, `singular multicut <n>`, and only when the count is
non-zero. The existing `singular eligible …` line is not reworded (the file header forbids it), and
an option-off run's output stays byte-identical.

## Assumptions I cannot verify from the code

- **The option reaches the engine in the lab.** It is a new UCI option set on the candidate only.
  The workflow checks option names against the advertised table before any shard starts. That this
  run actually cuts is verified by a short local UCI session with `setoption name SingularMultiCut
  value true` showing a non-zero `singular multicut` line. Not done yet.
- **The spike's 14.4% carries to game play.** It was measured on one corpus at depth 12 with
  Threads=1. The lab run settles it, because the decision rests on Elo, not on the trigger rate.

## Invariants

- With the option off, search is node-identical to `origin/main` at Threads=1.
- A cut happens only after the verification completed. An aborted verification still returns
  `best_value` from the existing guard, before the cut test.
- A cut writes nothing persistent: no TT entry, killer, history or PV row.
- No cut at a PV node, at a mate-score `beta`, after a verification fail-low, or when
  `singular_beta < beta`.

## Validation

Search tier, with search-reviewer review.

- **Unit tests** in `SearchSingularTests.cpp`:
  - helper boundaries, covering `verify_value == singular_beta` (cuts), `singular_beta - 1` (does
    not), `singular_beta == beta` (cuts), `beta - 1` (does not), PV, mate-range `beta`, and the
    option off;
  - a search-level case: a LOWER entry on the first sorted move, below the node's depth, with
    `tt_value` set so `singular_beta >= beta` and the alternatives clear it at a low null window,
    called as a non-PV node (`search_node`'s `is_pv_node` defaults to true).
    It checks `search_node` returns exactly `beta`, `multicuts == 1`, and no TT entry was written
    for the key beyond the seeded one. With the option off the same node returns otherwise, which
    makes the case falsifiable.
- **Equivalence:** `Compare-SearchEquivalence.ps1 -Before <origin/main exe> -After <branch exe>
  -Positions Tests/profile-screen.fen -Depth 12` must report IDENTICAL, with the option off.
- **Bench:** `Run-Bench.ps1` takes no UCI options, so bench a local build with the default flipped
  to on (not committed) against the branch build. Record the node and nps deltas in the PR. Fewer
  nodes is expected; a slower nps per node is not.
- **Elo:** CI strength lab, one binary (the branch SHA on both sides), `candidate_uci_options:
  SingularMultiCut=true`, Threads=1, 10+0.1, 8,880 games. Ship it on by default only if the interval
  excludes zero on the positive side. If the result is positive but its interval spans zero, run a
  19,980-game confirmation on held-out openings, as in #702. Otherwise delete the feature. Record
  every run in `Measurements/ci-per-change.md`.

## Cost

- **Size:** 50–200 lines across `SearchTuning.def`, `AIPerplex.{h,cpp}`, `SearchTelemetry.h`, the
  test fixture and `SearchSingularTests.cpp`. A follow-up commit flips the default or deletes the
  feature.
- **Blast radius:** search tier. The UCI option table gains one entry.
- **Review:** one search-reviewer pass plus the code review (170–270k tokens, 3–5 min).
- **Lab:** about 3–4.5 h for 8,880 games; a confirmation run, if needed, is about twice that.
  Funded by the owner on #721.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 trigger and why it fires only below the TT entry's cut depth | comment on `singular_multicut_eligible()` |
| D2 fail-hard return, D3 no store | comment at the cut site in `pvs()` |
| D7 separate telemetry line | `SingularStats` member comment |
| Lab result and ship/delete decision | `Measurements/ci-per-change.md`, `Docs/Changelog.md`, the PR body |
| Option semantics | `SearchTuning.def` comment |
