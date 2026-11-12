# Reverse futility on a TT-refined value — Design

**Issue:** #545 (re-screen tracked by #961, slice B) · **Baseline:** `origin/main` `7f16db4f`

## Goal

Reverse futility cuts on one static evaluation, even when this node's TT probe has just returned a
searched MAIN lower bound that sits above it but was too shallow to cut. That bound is better evidence
that the node stands above beta. #545 implemented this in September (`090415e5`) and parked it on a
wall-clock gate: about −1% nodes against about 0.75% nps for the flag check. Neither number is an Elo
measurement, and #636's method has since replaced wall-clock gates with the lab. The search has also
moved since: depth-3 LMP, depth-2 LMP UPPER stores, depth-2 frontier futility, quiet SEE pruning and
IIR. This change re-applies the patch by hand on current `main` behind a default-off option, so one
lab SPRT on one SHA can answer the only open question: does it gain Elo on today's tree?

## Review focus

- **D2, trusting an entry of any stored depth.** Almost every refined cut rests on an entry shallower
  than the node. The thinnest evidence is a depth-1 searched fail-high, or a depth-1/2 PV EXACT entry,
  at a depth-3 node. That is the feature, and the risk of it.
- **D3, where the TT value is read.** The parked patch tested the flag on every non-cutting MAIN hit,
  which is where the 0.75% nps went. This design reads it only inside the eligible reverse-futility
  block. It intends no added work on the probe path; the flag-off bench decides.

## Scope

**This change will:**

- add `TUNING_FLAG(reverse_futility_tt_refine_enabled, false, …, "ReverseFutilityTtRefine", true)`;
- at an eligible reverse-futility node with the flag on, compare `max(node_eval(), tt_value)` with
  `beta + margin * depth`, where `tt_value` is this node's own probe's MAIN entry, bound `LOWER` or
  `EXACT`, and non-mate;
- add falsified tests and catalogue tests, carried over from `090415e5` and adapted.

**This change will not:**

- change any default. With the flag at its default, search is node-identical to `main`;
- change `static_eval` or `node_eval()`, frontier futility or its floor, null move, quiescence, TT
  layout or storage, singular eligibility, or the reverse-futility margin, depth band, fail-hard
  return or no-store rule;
- use `UPPER` values to skip a prune. That is the move-quality mirror, which belongs to #502;
- add a rule-50 guard (D4) or a new telemetry counter;
- promote the default. That is a follow-up PR, and only on a lab H1.

## Decisions

### D1: refine reverse futility only, upward only

A local `rfp_eval = max(node_eval(), tt_value)` is used for the comparison alone. `static_eval` stays
the evaluator's number, so frontier futility's skip test and fail-low floor read exactly what they do
today. Since slice A, that floor also runs at depth 2, as `static_eval + 300`.

- **Rejected: refining `static_eval` itself.** The floor bounds a reported fail-low. A raised value
  could exceed the alpha that licensed the skip (#545 triage). Stockfish keeps the same split.

### D2: trust an entry of any stored depth

There is no depth condition on the entry. A non-mate `LOWER`/`EXACT` entry with `depth >= node depth`
and a value at or above beta has already taken the TT cutoff, so a depth-qualified rule changes
nothing (#548: every flip came from a shallower entry). With the default band (RFP depth ≤ 3, MAIN
entries depth ≥ 1), the entry's label is at most two plies below the node's.

There is one exception. IIR runs between the probe and reverse futility, and fires only without a hash
move, so it can turn a depth-4 node into a depth-3 one. A depth-3 entry with an empty move (a
null-move LOWER, or a terminal EXACT) that missed the depth-4 cutoff may then refine at the post-IIR
node's own label. That is the only way a null-move entry refines by default: null move needs depth ≥ 3
and stores at the node's depth, so in the band it is never shallower otherwise.

The thinnest evidence is therefore a shallow entry that carries a move: a depth-1 searched fail-high
at a depth-3 node (the hash move's quiescence value cleared `beta + 300`), or a depth-1/2 PV EXACT
entry. Without the flag, that node would search the same hash move at depth 2 first, so each cut
saves little. Two labels is a statement about labels, not about searched subtree depth. The cut is
fail-hard and stores nothing.

- **Rejected: a `depth − k` field.** With the default band it would only drop depth-1 entries at
  depth-3 nodes. No evidence says those are the bad ones, and it would add a second axis to a
  one-question lab run.

### D3: read the entry inside the reverse-futility block, not in the probe

The probe's `std::optional<TTEntry>` is hoisted from the `if (!is_exclusion_frame)` block to a local
`tt_entry`, declared beside `hash_move` and left empty for exclusion frames. The probe path is
unchanged: no flag test, no extra copy. Inside the existing `reverse_futility_eligible(...)` block:

```cpp
int rfp_eval = node_eval();
if (tuning_.reverse_futility_tt_refine_enabled && tt_refines_reverse_futility(tt_entry))
	rfp_eval = std::max(rfp_eval, static_cast<int>(tt_entry->value));
```

`tt_refines_reverse_futility` is a static helper: entry present, phase `MAIN`, bound not `UPPER`,
`|value| < Mate_Threshold`. The flag is tested first, so flag-off is intended to add one branch per
eligible node and nothing per probe. That is a code-generation hypothesis: the hoisted optional stays
live across `Evaluate()` and IIR, and the compiler may lay out the probe path differently. The
flag-off bench decides. A copy, not a pointer into the slot, is required: the lock-free TT can
replace the slot under another thread.

- **Rejected: the parked patch's capture in the probe.** It tested the flag on every non-cutting MAIN
  hit and stored a sentinel on every node. The issue's close note asked for exactly this reordering.
- **Rejected: two extra locals (value, bound) set in the probe.** That is a store on every MAIN hit to
  serve a block most nodes never reach.

### D4: no rule-50 guard; defer to #347

This is unchanged from the owner's 2026-09-17 decision on the original design: no halfmove-clock
condition. The refinement reads the same entries the TT cutoff already trusts without a guard, and
#549 counted zero cutoffs near the boundary.

The shared probe snapshot does not make a future guard cover this read automatically. #347 must apply
its score-context predicate to both consumers, the TT cutoff and `tt_refines_reverse_futility`, while
keeping the hash move usable. That obligation is harvested into `Docs/EngineContracts.md`.

## Assumptions I cannot verify from the code

- **Some headroom survives today's pruning mix.** On September's tree it was about −1% nodes, and LMP
  has since grown. Not verified, and deliberately not gated: the informational node probe in
  Validation records it, and the lab decides.
- **Fewer nodes at fixed depth converts to Elo.** Not inferable, because the extra cuts rest on the
  least-trusted entries. Settled only by the owner-approved lab SPRT.
- **#549's zero rule-50 exposure is representative.** Its corpus never reached clock 80. Not verified
  further. The residual risk is stated in D4.

## Invariants

- Flag off: search is node-identical to `7f16db4f` at `Threads=1`: same nodes, PVs and best moves.
- `static_eval` never takes a TT value. Frontier futility's skip test and floor are unchanged with the
  flag on.
- Only a MAIN entry from this node's own probe refines. Exclusion frames (no probe), PV nodes and
  in-check nodes (ineligible) never do.
- A mate-range value, an `UPPER` bound, a QUIESCENCE entry, or a value at or below static evaluation
  never changes the decision.
- Reverse futility still returns exactly `beta` and stores nothing.

## Validation

Engine tier, search-behaviour change behind a default-off option.

1. **Tests** in `SearchFutilityTests.cpp`, carried over from `090415e5`. Each is falsified by its own
   source mutation: refinement removed, flag ignored, `UPPER` admitted, mate admitted, phase check
   dropped, `max` dropped, and the TT value written into `static_eval`.
   - A shallower `LOWER`/`EXACT` entry clearing the margin cuts at node depths 2 and 3. It returns
     `== beta`, searches nothing, and leaves the planted entry unchanged. The same entry does not cut
     with the flag off, or one centipawn short.
   - No cut from `UPPER`, mate-range or QUIESCENCE entries.
   - A TT value at or below static evaluation does not stop a cut that static evaluation earns.
   - Frontier futility's floor tripwire: same score and stored entry with the flag on and off, at
     depth 1 and at depth 2 (margin 300 since slice A). The planted value sits between `static_eval +
     margin` and alpha, so a leak into `static_eval` would move the floor. Each case `REQUIRE`s a
     frontier skip.
   - The IIR exception: a depth-4 node with a depth-3 LOWER entry, empty move, value ≥ `beta + 300`,
     cuts with the flag on and is searched with it off. The same entry carrying a best move does not
     cut, because IIR does not fire and depth 4 is outside the band.
   - Catalogue default, UCI parse and option-line cases in `SearchTuningTests.cpp`, and an
     option-apply case in `UCITests.cpp` following slice A's, since that is the path the lab uses.

   The depth-2 and depth-3 cases are beyond IIR's reach (`iir_min_depth` is 4).

   **Flag-on route.** Unit tests set the flag through the fixture (`set_reverse_futility_tt_refine`).
   `Run-Bench.ps1` and the tactical suite cannot set an engine option, so steps 4 and 5's flag-on runs
   use a temporary build with the default flipped to `true`. It is labelled as such, never committed,
   and the default-off build is rebuilt before steps 2, 3 and the lab.
2. **Equivalence:** `Compare-SearchEquivalence.ps1` against `7f16db4f` must report `IDENTICAL`.
3. **Flag-off cost:** a paired `Run-Bench.ps1` against `7f16db4f`, with interleaved rounds per skill
   `measure-strength`. Flag-off nps must be within noise of main.
4. **Informational, not a gate:** deterministic depth-12 bench node counts with the flag on, for
   comparison with September's −1%. Recorded in the PR body; it decides nothing.
5. Fast suite plus `[tactical]` with the flag on and off, and `search-reviewer` on the diff.
6. **Elo, after the owner approves:** a lab Full run with `sprt=true`, SPRT [0, 3],
   `candidate_uci_options=ReverseFutilityTtRefine=true`, and `reference_ref` at the merge base.
   **Rule, stated before dispatch:** H1 → flip the default on in a follow-up PR. H0, or inconclusive
   at the cap → delete the code. This is #961's rule and deliberately overrides the lab's cap
   fallback (`Elo − err > 0` passes): a cap-completed run deletes the code even if the harness reports
   a pass. The harness result and the keep/delete decision are recorded separately in `Measurements/`.

## Cost

- **Size:** 50–200 lines, mostly tests, across `AIPerplex.cpp`, `SearchTuning.def`,
  `SearchFutilityTests.cpp`, `SearchTuningTests.cpp`, `SearchTestFixture.h` and `UCITests.cpp`.
- **Blast radius:** Engine tier, default off. `Docs/EngineContracts.md` gets one line.
- **Review:** one code review, plus `search-reviewer` on this document and on the diff.
- **Lab:** 1–4 h of SPRT, at the owner's go.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 (`rfp_eval` vs `static_eval`), D2's any-depth reason, D3's placement | source comment at the reverse-futility guard |
| Flag semantics | `SearchTuning.def` comment beside the field |
| D4: no rule-50 guard, residual risk, and #347's obligation to guard both consumers | `Docs/EngineContracts.md` → Search internals |
| Node probe, lab verdict | `Measurements/`, `Docs/Changelog.md`, PR body |
