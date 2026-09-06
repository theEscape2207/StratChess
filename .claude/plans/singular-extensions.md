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
management. This change therefore lands the mechanism **disabled by default**, so it is provably
node-identical to today's search, and leaves the flip-to-enabled to a follow-up backed by a measured
Elo result.

## Scope

**This change will:**

- Add a per-ply exclusion state to `AIPerplex::pvs()` and the six guards that make an exclusion
  search safe (no PV row clear, no TT probe, no TT store, no null move, no terminal adjudication).
- Add a conservative singular trigger at the hash move, gated behind `SearchTuning` knobs.
- Add regression coverage for the exclusion semantics and the eligibility boundaries.
- Add three per-thread telemetry counters so the trigger rate and cost can be measured.

**This change will not:**

- Enable the feature. `singular_extensions_enabled` defaults to `false`; with it off the search is
  node-identical to `main`.
- Add double extensions, negative extensions or multi-cut pruning. Those are the tuned refinements
  Stockfish layers *on top* of a working singular check, and each needs its own measurement.
- Tune the knobs. The defaults below are starting points chosen to be conservative, not tuned
  values.
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
subsequent probe. The exclusion branch returns `alpha` — a fail-low, i.e. "no alternative reached
the margin", which is exactly the answer a verification search wants — and writes nothing.

### D5: Knobs live on `SearchTuning` while experimenting

`singular_extensions_enabled` (false), `singular_min_depth` (8), `singular_tt_depth_margin` (3),
`singular_margin_factor` (2). Putting them on `SearchTuning` is what makes the follow-up measurable
without a rebuild per candidate. Only the knobs a measurement justifies survive into the enabled
version; the rest collapse to constants.

## Assumptions I cannot verify from the code

- **That a singular extension is worth its node cost in this engine.** Explicitly not assumed — it
  is why the flag ships off. Settled only by an Elo match against the immediate pre-change
  reference, which this change does not run.
- **That the margin and reduction defaults are in a sane range.** Taken from the shape of the
  standard formulation, not from measurement here. The bench and trigger-rate pass will say whether
  they produce a plausible trigger rate (a rate near 0% or near 100% means the defaults are wrong,
  independent of Elo).
- **That re-entering `pvs()` at the same ply is safe for `td.killers[ply]`.** The verification search
  will store killers at the parent's ply from its own cutoffs. This is deliberate in the standard
  formulation — those moves are refutations in the same position — but it is a real mutation of the
  parent's ordering state, not a no-op. It cannot affect correctness (killers only order moves), and
  with the flag off it never happens. Not otherwise verified.

## Invariants

- **Flag off ⇒ node-identical.** `Compare-SearchEquivalence.ps1` must report identical node counts
  and best moves at `Threads=1`. This is the property that makes an unmeasured merge safe.
- **No exclusion search ever probes or stores the MAIN TT under its position's key.** A partial move
  set must never be cached as if all legal moves were available.
- **No nested singular verification.** An exclusion search does not itself trigger one.
- **Null-move pruning is off inside an exclusion search.** A pass is not one of the alternatives
  being proved inferior, so a null-move cutoff would answer a different question.
- **The abort contract is unchanged.** An incomplete frame writes no TT entry, PV row, killer or
  history. The verification search runs before `DoMove`, so the board is intact at the existing
  guard and no new unwind path is introduced.
- **The extension respects `MAX_PLY`.** `ply + 1 < MAX_PLY` gates the extra ply.

## Validation

Engine tier.

| Risk | Evidence that closes it |
|---|---|
| The mechanism changes today's search | `Compare-SearchEquivalence.ps1 -After <exe>`: identical node counts and best moves at `Threads=1`, flag off |
| Exclusion semantics are wrong | Unit tests per guard, each falsified against the unfixed code before being trusted |
| Out-of-bounds or uninitialised per-ply state | Debug-build test run (Release passes OOB reads silently); Linux Debug + sanitizers in CI |
| Enabled path crashes or hangs | Tactical suite at `Threads=1` and `Threads=4` with the flag forced on |
| Cost is unacceptable | Repeated `Run-Bench.ps1` passes, flag off *and* on, reported as per-position wall clock plus MAIN/QS node movement — not aggregate nps, since the tree changes when the flag is on |
| Trigger is degenerate | Telemetry counters: eligible nodes, verification searches, extensions granted |

**No Elo match is run for this PR**, and it is not needed for it: the shipped configuration is
node-identical to `main`, so there is nothing for a match to measure. The follow-up that flips the
flag cannot merge without one.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the PV-row clear is skipped under exclusion (D2) | source comment at the guard in `pvs()` |
| Why an excluded-only-legal-move fails low rather than adjudicating (D4) | source comment at the `!moveFound` branch |
| Why null move is off under exclusion | source comment in `should_try_null_move()` |
| That the flag ships off and why | `Docs/Changelog.md`, and the PR body |
| Trigger rate and bench cost figures | PR body and issue #95 — point-in-time, so not source comments |
| Remaining work: tune, measure, flip the flag | issue #95, updated on merge |
