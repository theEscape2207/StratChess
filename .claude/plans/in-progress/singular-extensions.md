# Singular Extensions — Design

**Issue:** #95

## Goal

At a tactically lopsided node the search spends the same nominal depth on a move whose alternatives
are all materially worse as it does on an ordinary first move, so forced continuations run into the
horizon one ply too early. A singular extension buys that ply back, but only where it has been
*earned*: the transposition table already believes one move is a strong lower bound, and a cheap
reduced-depth search confirms every alternative fails below a margin.

Whether this is worth its cost in this engine is unknown and cannot be assumed from other engines'
published gains — the trade depends on this search's ordering, this evaluation and this time
management. This change therefore lands the mechanism **compiled out of the shipping engine** (D9),
so that build is provably node-identical to today's search and pays nothing for carrying the code,
and leaves enabling it to a follow-up backed by a measured Elo result.

## Scope

**This change will:**

- Add a per-ply exclusion state to `AIPerplex::pvs()` and the guards that make an exclusion search
  safe (no PV row clear, no TT probe, no TT store, no null move, no terminal adjudication).
- Add an absolute ply backstop to `pvs()`. It has none today (D6), and an extension removes the
  depth-decreases-monotonically argument that stands in for one.
- Add a conservative singular trigger at the hash move, gated behind `SearchTuning` knobs.
- Add regression coverage for the exclusion semantics, the eligibility boundaries and the backstop.
- Add three per-thread telemetry counters so the trigger rate and cost can be measured.

**This change will not:**

- Ship the feature. It is compiled out of the shipping engine entirely (D9), so that build is
  node-identical to `main` and pays nothing for carrying it.
- Add double extensions, negative extensions or multi-cut pruning. Those are the tuned refinements
  Stockfish layers *on top* of a working singular check, and each needs its own measurement.
- Build a runtime configuration route for `SearchTuning` (D5). The two configurations this PR needs
  are covered by two builds; a route is a prerequisite for the tuning follow-up, not for this merge.
- Remove the gates. That is the follow-up's job: delete both defines and the `option()` if the
  feature wins its match, delete the feature if it loses. Neither outcome leaves a permanent flag.
- Fix #305 (D7). The change makes deeper plies reachable in principle but does not bring ply 100
  within reach.
- Produce an Elo verdict. Bench cost and trigger rate are reported; the measurement budget beyond
  that is the project owner's call.

## Decisions

### D1: Carry the exclusion state on `ThreadData`, not as a `pvs()` parameter

`td.excluded_move[ply]`, mirroring the existing `bool last_move_was_null[MAX_PLY]` (`ThreadData.h`),
which solves the identical problem — a per-ply flag that suppresses null-move pruning at one node.

Rejected: an eighth parameter on `pvs()`. It is more explicit, but it churns all seven recursive
call sites to pass `Move::EmptyMove()` and widens a signature that is already at the limit of
readability. The precedent in `ThreadData` is what makes the implicit version legible rather than
surprising: a reader who understands `last_move_was_null` understands this immediately.

Rejected: a `SearchFrame` aggregate. One field does not justify a new type.

**Lifetime.** The array is value-initialised to empty moves with the rest of `ThreadData`, and the
slot is set and restored by an RAII guard around the verification call — not by a bare
set/call/clear sequence. The verification search can return through the abort path, and a scope
guard makes restoration independent of how the call returns. A leaked non-empty slot would silently
disable the TT and null move for every later node at that ply.

### D2: The verification search re-enters `pvs()` at the *same* ply

This is the standard formulation and the only one that keeps the verification measuring the same
position. Its hazard is that `pvs()` opens with `td.pv_table.clear_ply(ply)`, which would wipe the
row the parent frame is midway through building.

Two ways out. Guarding the clear (chosen) removes the hazard outright and costs one branch on a path
that is already doing a table write. Relying on the trigger firing only at `move_number == 0`
(rejected as the *primary* defence) is true today — `MoveSorter::ScoreMoves` orders the hash move
first, so no earlier move can have called `pv_table.update(ply, ...)` — but it makes a move-ordering
property load-bearing for a memory-safety-adjacent invariant three functions away. The
`move_number == 0` gate is still there, as a conservativeness gate rather than as protection.

### D3: The trigger fires only at the hash move, at `move_number == 0`

Eligibility is computed once per node before the move loop and re-checked as `move_number == 0 &&
move == hash_move` inside it. This is narrower than Stockfish, which tests the TT move wherever it
lands. It is chosen because the whole point of a default-off first version is to keep the eligible
set small and the cost bounded while the trigger rate is being measured; widening it is a tuning
question for the follow-up.

It also has the effect that with the flag off, the only work added to the hot loop is a `bool` test
that short-circuits — the flag is the first term of the eligibility conjunction.

### D4: An excluded-only-legal-move node fails low; it is not checkmate

If the excluded move is the position's only legal move, the loop finds nothing and `moveFound` stays
false. The existing code path adjudicates that as mate or stalemate and stores it as `EXACT`. Under
exclusion that is a lie about the real position, and storing it would poison the key for every
subsequent probe. The exclusion branch returns `original_alpha` — a fail-low, i.e. "no alternative
reached the margin", which is exactly the answer a verification search wants — and writes nothing.
`original_alpha` rather than `alpha`: the two are equal on this path, since alpha only moves inside
the `moveFound` branch, but the equality is three nesting levels away from the return.

### D9: The feature is compiled out of the shipping engine, not merely disabled

A runtime flag alone still costs the shipping build: measured **−1.33% nps** for code that never
executes. Since the end state is unconditional-on or deleted, nothing should pay for it
indefinitely.

`STRAT_SINGULAR_EXTENSIONS` (CMake `option`, OFF) drives a `constexpr bool`, and every use is
`if constexpr` or the leading term of a conjunction. **Not `#ifdef`**: the discarded branch of an
`if constexpr` in a non-template context is still parsed and type-checked, so the disabled code
cannot rot — which is the standing objection to preprocessor branches in a hot path.

**Two defines, because the two targets want opposite answers.** `STRAT_SINGULAR_EXTENSIONS`
compiles the code in; `STRAT_SINGULAR_DEFAULT_ON` starts it enabled.

| Target | compiled in | default on | why |
|---|---|---|---|
| shipping engine | no | no | pays nothing |
| experimental engine (`-DSTRAT_SINGULAR_EXTENSIONS=ON`) | yes | yes | UCI cannot set the runtime flag, so compiled-in-but-off would measure nothing |
| test binary | yes (always) | **no** | every other search test must keep exercising the SHIPPED configuration; the singular tests enable it for themselves |

That third row is the one worth pausing on. The test binary compiles the feature in, so defaulting
it on there would have quietly turned every tactical, quiescence and TT-contract test into a test of
a configuration that does not ship.

The runtime flag survives inside a compiled-in build so tests can toggle it and the follow-up can
sweep parameters without a rebuild per candidate.

**What this costs in coverage, stated plainly:** the shipped binary no longer contains the feature,
so the tests only ever exercise the compiled-in build. The claim "the shipped build is unaffected"
now rests on the equivalence gate rather than on unit tests. That is a stronger guarantee than a
runtime flag reading false, but it is a real shift in what covers what.

### D5: Two builds, not a runtime configuration route

The knobs are `SearchTuning` fields: `singular_extensions_enabled` (false), `singular_min_depth`
(8), `singular_tt_depth_margin` (3), `singular_margin_factor` (2).

**`SearchTuning` is not reachable from a UCI search.** `UciHandler::init_ai()` builds
`AIPerplexConfig` from hardcoded values and never consults `game_settings.json` or `PlayerFactory`;
`search_tuning` reaches only the `game`-mode path, and `cmd_setoption` recognises just `Threads` and
`Hash`. Since `Run-Bench.ps1`, `Compare-SearchEquivalence.ps1` and every match harness drive the
engine over UCI, adding C++ fields alone configures nothing they can see.

This PR needs exactly two configurations — flag off and flag on — so it uses **two builds**, with
the default flipped in the second. Rejected for this PR: adding setoption cases (scope-creeps a
deliberately minimal parser) and wiring `game_settings.json` into `init_ai()` (would make every
existing knob live over UCI for the first time — a behavioural change for all users, buried inside a
singular-extensions PR).

A runtime route *is* a prerequisite for the tuning follow-up, which sweeps rather than compares two
points. That follow-up should pick the mechanism knowing what the sweep needs. Recorded on #95.

### D6: `pvs()` gets an absolute ply backstop

`pvs()` has no ply bound today. It is bounded only by depth decreasing on every recursive call,
which terminates the recursion long before `ply` can index past `MAX_PLY`. An extension searches a
child at the parent's depth, so that argument no longer holds: a line in which every node extends
never reduces depth.

Draw detection bounds such a line in practice, but that is exactly the reasoning `quiescence()`
already rejects for itself — *"the recursion must terminate on its own rather than on an argument
about what positions can arise, and ply indexes fixed-size per-thread arrays elsewhere in the
search"* (`AIPerplex.cpp` ~L893). `pvs()` indexes `td.killers[ply]`, `td.last_move_was_null[ply]`
and the PV table, and **writes `td.last_move_was_null[ply + 1]`**, so its bound must be
`ply >= MAX_PLY - 1` to keep that write in range.

The backstop returns a static evaluation, matching `quiescence()`'s. **It is the first statement in
the function**, above the PV clear:

```
// (1) Absolute backstop. First, because it is what bounds every ply-indexed access
//     below — including the excluded_move[ply] read that the PV guard in (2) needs.
//     An exclusion frame can never reach here: it runs at its parent's ply, and that
//     parent returned from this same test before it could launch a verification. So
//     the clear is unconditional and cannot wipe a row another frame is building.
if (ply >= MAX_PLY - 1) {
    td.pv_table.clear_ply(ply);
    return evaluator_.Evaluate(td.board);
}

// (2) PV clear — same position relative to the abort exits as today, so L494-502's
//     rationale is untouched; now skipped for exclusion frames per D2.
if (td.excluded_move[ply].IsEmpty())
    td.pv_table.clear_ply(ply);

// (3) abort / poll_search_limits / check_draws / depth <= 0 — all unchanged.
```

Ordering here is load-bearing in two directions. `td.pv_table.clear_ply(ply)` must stay **above** the
abort exits: the comment at `AIPerplex.cpp:494-502` explains that a frame returning from an abort
would otherwise leave a stale row 0 populated, letting an aborted aspiration retry look like a
completed iteration at the root. But the D2 guard in front of it reads `td.excluded_move[ply]`, and
that read is unbounded — which is why the backstop goes above it rather than after the abort checks
as the previous revision of this document said. `PVTable::clear_ply` bounds-checks internally
(`PVTable.h:19`), so it is not the access that needs protecting; the new array is.

Cost is one predictable compare per `pvs()` node, on a path that already does a table write.

This is a latent-bug fix, not a feature: it is correct on `main` too. It ships here because this is
the change that makes it live.

### D7: #305's mate-score ply limit is acknowledged, not fixed

TT mate-score normalisation silently breaks beyond ply 100 (#305), and extensions make deeper plies
reachable in principle. It stays out of scope because the reach is not there: extensions grant one
ply each, are gated at `depth >= 8`, and do not nest within a frame, so ply 100 needs roughly fifty
extensions on a single line. The backstop in D6 caps `ply` at `MAX_PLY - 1`, which is well above
#305's cliff — D6 bounds memory safety, not #305. If the follow-up widens eligibility or adds double
extensions, #305 must be re-examined before it does.

### D8: The verification is hoisted above the move loop

`pvs()` learns that a move is legal only from `DoMove()` returning true, and assigns `move_number`
inside that branch (`AIPerplex.cpp:613-614`) — at which point the board holds the child position.
"At the first legal move" is therefore not a place where a search of the *parent* position can be
issued.

The verification is hoisted to just after `ScoreMoves()`, where the board is unambiguously the
parent, and its result is applied as an `extension` variable consumed at `move_number == 0`.
Eligibility gains one term — the hash move must be the first *sorted* move — and the in-loop
`move == hash_move` check supplies the first-*legal* half. Details and the rejected undo/remake
alternative are in the Verification algorithm section below.

## Verification algorithm

**The verification runs before the move loop, not inside it** (D8). `pvs()` establishes legality by
`DoMove()` returning true and assigns `move_number` inside that branch (`AIPerplex.cpp:613-614`), so
by the time a "first legal move" is identified the board already holds the *child* position. A
verification search placed there would search the wrong position.

```
// (a) After ScoreMoves(), before the move loop. Board holds the parent position.
//     `tt_value`/`tt_depth` are from the MAIN entry probed at the top of the node.
eligible =  tuning.singular_extensions_enabled          // first term: short-circuits when off
         && ply > 0 && !in_check
         && td.excluded_move[ply].IsEmpty()             // not already an exclusion frame
         && depth >= tuning.singular_min_depth
         && tt_entry.has_value() && tt_entry.phase == MAIN
         && !tt_entry.best_move.IsEmpty()
         && (tt_entry.bound == LOWER || tt_entry.bound == EXACT)
         && abs(tt_value) < GameValues::Mate_Threshold
         && tt_depth >= depth - tuning.singular_tt_depth_margin
         && n > 0 && moveList[scored_idx[0].second] == hash_move;   // hash move sorted first

int extension = 0;
if (eligible) {
    singular_beta = tt_value - tuning.singular_margin_factor * depth;
    verify_depth  = max(1, (depth - 1) / 2);            // clamped, not asserted -- see below
    {
        ExcludedMoveGuard guard(td, ply, hash_move);    // RAII, restores the previous slot value
        value = pvs(td, verify_depth, singular_beta - 1, singular_beta, ply, false, tt);
    }
    if (control_.IsAborted())
        return best_value;                              // still the sentinel; nothing searched yet
    if (value < singular_beta)
        extension = 1;
}

// (b) Inside the move loop, at the first legal move. No ply bound needed here: the backstop at
//     the top of pvs() (D6) already guarantees ply <= MAX_PLY - 2 for any frame that gets here.
if (move_number == 0) {
    const int child_depth = (extension && move == hash_move) ? depth : depth - 1;
    value = -pvs(td, child_depth, -beta, -alpha, ply + 1, is_pv_node, tt);
}
```

Two properties make the hoist exact rather than approximate. The last eligibility term checks that
the hash move is the **first sorted** move, which `MoveSorter::ScoreMoves` guarantees whenever a
hash move exists; and the in-loop `move_number == 0 && move == hash_move` re-check is what confirms
it was also the first *legal* one. If the hash move turns out to be illegal, the verification search
is wasted but harmless — it excluded a move that was not in the list — and no extension is applied.
That case costs one search on a TT move that failed legality, which is rare enough not to warrant
avoiding.

The abort return is `best_value`, which is still the `-Search_Init` sentinel at this point since no
move has been searched. The board is untouched — verification runs before any `DoMove()` — so no
unwinding is needed beyond the return itself. `poll_search_limits()` can only return true by
latching the abort flag, so there is no path where the verification returns a fabricated
`GameValues::Draw` and the `IsAborted()` check fails to fire; that matters, because a stale
`verify_value == 0` against a typical `singular_beta` would have granted a spurious extension.

Rejected: verifying inside the loop via `DoMove` → `UndoMove` → verify → `DoMove` again, as the
review suggested. It works, but it needs its own abort handling between the undo and the remake, and
it needs the remake to be assumed infallible — a determinism contract on `DoMove` that does not
currently exist and that nothing else in the search depends on. Hoisting removes the question.

The window is a null window `[singular_beta - 1, singular_beta]`, so the verification is the cheap
one-bit question "does any alternative reach `singular_beta`?" and the comparison that answers it is
`value < singular_beta` — a strict fail-low. `singular_beta` is not clamped: `tt_value` is already
below `Mate_Threshold` by the eligibility gate, and the margin only moves it further from mate.

Guards applied inside a frame whose `td.excluded_move[ply]` is non-empty:

| Site | Behaviour under exclusion |
|---|---|
| `pv_table.clear_ply(ply)` at the top | skipped — must not wipe the parent's row it is re-entering. Unreachable at the D6 backstop's own clear, which stays unconditional |
| TT probe | skipped — no probe, no cutoff |
| `should_try_null_move()` | returns false — a pass is not one of the alternatives being disproved |
| move loop | skips the excluded move |
| the `!moveFound` branch | returns a fail-low `original_alpha`, **not** mate/stalemate, and stores nothing |
| the final `tt.store()` | skipped |

## Assumptions I cannot verify from the code

- **That a singular extension is worth its node cost in this engine.** Explicitly not assumed — it
  is why the flag ships off. Settled only by an Elo match against the immediate pre-change
  reference, which this change does not run.
- ~~**That the margin and reduction defaults are in a sane range.**~~ **Settled, and it split.** The
  *margin* is fine: the extension fires on 7.9% of verifications, nowhere near 0% or 100%. The
  *verification depth and eligibility* are not: they produce 2,688 verifications costing +43.5% wall
  clock at fixed depth. Sane trigger selectivity, unaffordable trigger frequency.
- **That the verification search's effect on `td.killers[ply]` is acceptable.** It will store
  killers into the parent's own ply slots from its own cutoffs. This is deliberate in the standard
  formulation — those moves are refutations in the same position — but it is **not** confined to
  move ordering: `isKiller` disables LMR (`AIPerplex.cpp:625,630`), so a killer written by the
  verification search can change the *depth* at which the parent later searches that move. The
  effect is second-order and cannot make a result incorrect, and with the flag off it never happens,
  but it means the enabled path is not "same tree plus one ply". Not otherwise verified; the
  tactical suite with the flag on is what would expose it going wrong.

## Invariants

- **Shipping build ⇒ node-identical.** `Compare-SearchEquivalence.ps1` must report identical node
  counts and best moves at `Threads=1`. This is the property that makes an unmeasured merge safe,
  and since D9 compiles the feature out it is now the *only* thing covering the shipped
  configuration — the tests all run against a build that has the feature compiled in.
- **`ply <= MAX_PLY - 2` at every ply-indexed access in `pvs()`**, so the `last_move_was_null[ply + 1]`
  write stays in range (D6). Holds regardless of how many extensions a line has been granted.
- **The PV row clear still precedes the abort exits for every non-exclusion frame.** `AIPerplex.cpp:494-502`
  is a contract about root aspiration retries, not an implementation detail, and the backstop and D2
  guard are both inserted around it rather than through it.
- **The verification search is issued while the board holds the parent position** (D8).
- **`verify_depth >= 1`.** A verification search must not fall through to `quiescence()`, which has
  no exclusion state and would both search the excluded move and use the normal TT. With
  `singular_min_depth >= 3` the formula gives at least 1; this is asserted rather than left as a
  consequence of a tunable's default.
- **No exclusion search ever probes or stores the MAIN TT under its position's key.** A partial move
  set must never be cached as if all legal moves were available.
- **A frame does not re-enter verification at its own ply.** What enforces this is the **skipped TT
  probe**, not the `!is_exclusion_frame` term in the eligibility conjunction: with no probe an
  exclusion frame has no hash move and no usable entry, so the gate cannot pass however the rest of
  it is written. Removing that term alone changes no behaviour — established by falsification, which
  left the suite green until a test was written against the probe skip itself. The term is kept as
  defence in depth and labelled as such in the source.

  The guarantee is per-frame only: nodes *below* an exclusion frame carry an empty slot and may
  trigger their own verifications. That is intended — they are ordinary nodes in a real subtree —
  and it is why the cost is bounded by the eligibility gate rather than by a nesting rule.
- **Null-move pruning is off inside an exclusion search.** A pass is not one of the alternatives
  being proved inferior, so a null-move cutoff would answer a different question.
- **`td.excluded_move[ply]` is empty on every path out of the verification call**, including abort.
- **The abort contract is unchanged.** An incomplete frame writes no TT entry, PV row, killer or
  history. The verification search runs before `DoMove`, so the board is intact at the existing
  guard and no new unwind path is introduced.

## Validation

Engine tier.

| Risk | Evidence that closes it |
|---|---|
| The mechanism changes today's search tree | `Compare-SearchEquivalence.ps1 -After <exe>`: identical node counts and best moves at `Threads=1`, flag off |
| The feature costs speed even when not in use | Repeated `Run-Bench.ps1` passes, shipping build vs. the fork point, compared on **nps**. Equivalence proves the tree is the same; only nps proves the same tree is not reached more slowly under a clock. This is the check that caught the −3.44% and then the −1.33%, neither of which equivalence could see |
| The experimental build is broken and nobody notices | Build it (`-DSTRAT_SINGULAR_EXTENSIONS=ON`) and confirm over UCI that it emits telemetry and searches a different tree. Compiled-in-but-disabled would look identical to a working build from the outside |
| Exclusion semantics are wrong | Unit tests per guard, each falsified by patching the guard out and confirming the suite goes red, with an unmutated control run that must stay green — a harness whose rebuild is broken reports every mutation red for the wrong reason |
| Unbounded ply / OOB per-thread array access | A test driving repeated extensions toward the boundary; Debug-build run (Release passes OOB reads silently); Linux Debug + sanitizers in CI |
| Enabled path crashes, hangs, or loses tactics | Tactical suite at `Threads=1` and `Threads=4` on the flag-on build, including the killer/LMR interaction in the assumptions above |
| Enabled path's cost is unacceptable | Repeated `Run-Bench.ps1` on the flag-on build, reported as per-position wall clock plus MAIN/QS node movement — not aggregate nps, since the tree changes when the flag is on |
| Trigger is degenerate | Telemetry counters: eligible nodes, verification searches, extensions granted |
| Cost is attributed to the wrong cause | `singular_verification_nodes`, counted across each verification call. Aggregate node counts and trigger counts cannot separate verification from the extended subtrees it authorises -- dividing growth by verification count yields an identity, and the first version of this document drew the wrong conclusion from exactly that |

### Results

| Check | Result |
|---|---|
| Equivalence vs fork point `9708c65`, flag off | **IDENTICAL**, 90 lines, 6 positions, depth 12, `Threads=1` |
| nps, shipping build vs fork point (warm, n=6, interleaved, idle machine) | **−0.40%** (2,719,282 vs 2,730,262), spreads 0.17%/0.40%, no overlap |
| Full + extended suites, flag on | pass (599 cases) |
| Tactical suite, flag on | 36/36 |
| Tactical depth-stability, flag on, `Threads=1` and `4` | 3 runs each, 0 flips |
| Debug build (asserts live), flag off and singular tag | pass (593 cases; 16 singular) |
| Trigger rate, flag on, depth 12 | 0.18 verifications per 1000 nodes; extension on **7.9%** of them (2.6–21.5% by position) |
| Enabled-path cost at fixed depth | **+43.5% wall clock** (pre-merge), **+23.8% nodes** |
| Where that cost goes | verification **20.9%**, extended subtrees + knock-on **79.1%** (measured via `singular_verification_nodes`, not inferred) |

**The enabled path is too expensive at these defaults, and the cost is dominated by the extensions,
not by verifying them.** `singular_verification_nodes` measures the node edges spent inside
verification searches directly:

| | nodes | share of growth |
|---|---|---|
| total growth, feature on vs off | 2,924,149 | — |
| inside verification searches | 610,266 | **20.9%** |
| extended subtrees + TT/killer knock-on | 2,313,883 | **79.1%** |

So the follow-up's lever is the **extension rate** — the margin — at least as much as the number and
depth of verifications. Cutting verifications entirely would recover about a fifth of the cost.

The effect is not uniformly additive: `startpos` searched ~797k *fewer* nodes with the feature on,
the extensions having improved its ordering. Aggregate growth hides that, which is another reason
the per-position column matters.

**This corrects an earlier claim in this document, and the correction is the point.** The first
version asserted the cost was *entirely* verification, reasoning "2,688 verifications × ~1,400 nodes
≈ the whole growth". That per-verification figure was itself obtained by dividing the growth by the
verification count, so the arithmetic was an identity that would hold for any split — it could not
distinguish verification from extended subtrees, and it happened to be wrong by a factor of four.
The counter exists because a reviewer asked for the supporting measurement and there wasn't one.

**The −0.40% residual is the ply backstop, and it stays.** With the feature compiled out nothing
else executes: the `if constexpr` leaves no code, every predicate folds, and the extra `ThreadData`
bytes are never read. What remains is one predictable compare per node for the `MAX_PLY` bound.
That is a stated benefit outweighing a measured slowdown, per the project rule — it is an absolute
recursion bound matching the one `quiescence()` already carries, justified independently of this
feature. Gating it on the experimental constant would reach ~0% but would make the bound conditional
on a flag that is meant to disappear.

Cost trajectory across the three shapes: **−3.44%** (as first written) → **−1.33%** (enable flag
leading every hot-path test) → **−0.40%** (feature compiled out).

**nps measurement notes.** Three things had to be right before any of these numbers meant anything:

- *Warm-up.* The first run of each binary is reliably slow, and including it made one pass report
  "inside run-to-run noise" when its own warm runs separated cleanly. Discarded.
- *Contention.* Two runs were rejected outright — the unchanged baseline binary measured 3.9% slower
  than on an idle machine, with spread ten times larger, and they disagreed with each other
  (−1.75%, then +2.31%). The harness now treats the baseline's absolute nps as a thermometer and
  refuses to report when it drifts more than 2% from a quiet-machine reference. A harness that
  cannot detect its own failure reports a number regardless.
- *Resolution.* The measurement settled a code-shape question the eye could not: extracting the
  verification into a helper cost 0.42%, so it stays an inline block.

**No Elo match is run for this PR.** The shipped configuration is node-identical to `main`, so the
only thing a match could detect is the per-node cost of branches that are never taken — and repeated
bench nps measures that directly, at a fraction of the wall time and with a tighter error bar. A
fixed-time match is the right instrument for the *enabled* path, and the follow-up that flips the
flag cannot merge without one.

Telemetry is read two ways: unit tests assert the counters directly through the existing
`STRAT_ENABLE_TEST_ACCESS` friend, and the flag-on build emits one `info string` summary line at the
end of a search, which is what makes the counters visible to the UCI-driven bench harness. The line
is emitted only when the flag is enabled, so the shipped configuration is byte-identical on stdout.

## Changed during implementation, after the search-reviewer round

Recorded here because the diff review and the design review look at different artifacts, and
nothing else reconciles them.

- **`verify_depth` is clamped, not asserted.** The doc listed `verify_depth >= 1` as an invariant of
  the code; it was an invariant of `singular_min_depth`'s *default* plus a Debug-only assert, in a
  feature whose stated follow-up is a parameter sweep. Now `std::max(1, (depth - 1) / 2)`.
- **The PV *write* gained the `!is_exclusion_frame` guard** that D2 gave the *clear*, plus an assert
  that an exclusion frame is never a PV node. D2's own argument — that a caller-side property must
  not carry a safety invariant — applied to the write too, and was not applied.
- **`ExcludedMoveGuard` restores the previous slot value**, not Empty. Identical while nesting is
  unreachable; structurally safe if it ever is not.
- **The `!moveFound` exclusion return is `original_alpha`**, not `alpha`. Same value; no longer
  depends on the alpha update three nesting levels away.

Three of these are unfalsifiable by construction — removing the guard leaves the suite green,
because something else already makes the state unreachable. They are listed as such in
`SearchSingularTests.cpp` rather than covered by tests that could only pass. Two tests written for
this round were **deleted for exactly that reason**; the third survived and pins the PV clear, which
had no test before.

The doc's own pseudocode was stale in three places (a `ply + 1 < MAX_PLY` term the backstop makes
redundant, an abort-return spelling the code improved on, and guard-table line numbers) — corrected
above rather than left to contradict the code.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the PV-row clear is skipped under exclusion (D2) | source comment at the guard in `pvs()` |
| Why an excluded-only-legal-move fails low rather than adjudicating (D4) | source comment at the `!moveFound` branch |
| Why null move is off under exclusion | source comment in `should_try_null_move()` |
| Why `pvs()` needs its own backstop once depth can stay flat (D6) | source comment at the backstop |
| Why the backstop precedes the pre-abort PV clear, and why that clear stays above the abort exits (D6) | source comment at the entry sequence |
| Why the verification cannot sit at the first legal move (D8) | source comment where the verification call sits |
| Why the feature is compiled out, and why two defines (D9) | source comment on the constant in `AIPerplex.h`, and the `option()` block in `CMakeLists.txt` |
| That the test target compiles it in but leaves it off (D9) | source comment at `target_compile_definitions(StratChessTests ...)` |
| That killers gate LMR, so verification writes can change later depths | source comment where the verification call sits |
| `SearchTuning` is unreachable from a UCI search (D5) | `Docs/EngineContracts.md` — a cross-cutting fact that outlives this change |
| That the flag ships off and why | `Docs/Changelog.md`, and the PR body |
| The backstop as a latent-bug fix independent of the feature | `Docs/Changelog.md` |
| Trigger rate and bench cost figures | PR body and issue #95 — point-in-time, so not source comments |
| Remaining work: config route, tune, measure, flip the flag; re-examine #305 if eligibility widens | issue #95, updated on merge |
