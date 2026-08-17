# Abort unwind and PV integrity — Design

**Issue:** #299, #310

## Goal

When the clock fires mid-search, `pvs()` and `quiescence()` return `GameValues::Draw` — a legal
score — and every frame on the unwinding stack goes on consuming it. Each frame then writes state
derived from children that were never searched: transposition entries at full nominal depth, killer
and history updates from a cutoff that did not happen, and PV rows spliced from a sibling's subtree.
The writes outlive the search that made them.

#299 and #310 observe this from opposite ends. #299 traced the contaminated score and the TT writes.
#310 is the same defect seen in the reported PV: fastchess rejects moves in our `info … pv` lines at
about 0.24 per game (4,780 warnings over a 19,980-game batch).

**Why the PV splices.** The two abort fast exits in `pvs()` sit *above* `td.pv_table.clear_ply(ply)`
(`AIPerplex.cpp:439`, `:448`, `:452`), so an aborted node leaves the row at its ply holding the line
an earlier sibling's subtree wrote there. At a PV node the first move is searched full-window
(`:547`) and populates that row; when a later sibling aborts and returns `0`, and `0 > alpha`
(both #310 samples are `score cp 0`), `:613` grafts the earlier sibling's line onto this move. From
ply 3 on, the line describes a different position — hence the reported `c5d3 … c5e4` with nothing
having moved back to c5. The interrupted-but-accepted iteration then emits it verbatim (`:396`).

## Scope

**This change will**, as two landings:

*PR 1 — abort correctness*

- Make an aborted frame unwind without mutating anything: after each recursive call returns, restore
  the board and return immediately, before any store, PV update, or killer/history write.
- Keep the interrupted root iteration's partial result, which becomes trustworthy as a consequence.
- Add a Debug assertion that every emitted PV replays legally from the search root, behind a named
  helper that is unit tested directly.
- Add a deterministic regression test for the abort path (see Validation).

*PR 2 — PV-move ordering*

- Revive the dead previous-iteration PV-move ordering hint (see D6, D7).

**This change will not:**

- Introduce a dedicated abort sentinel score (D2).
- Touch the abort *trigger* — `TimeManager`, the budget formula, or the 1024-node poll interval.
- Guard the three stores that depend on no child result (D1, exempt list).
- Validate or truncate PVs in Release builds (D5).
- Remove or relax `assess_iteration_quality`'s CASE 4/5. They also guard root-move instability,
  which is a separate concern from score contamination.
- Address #237's `WAC-287` reproduction, which runs at fixed depth where no abort fires.

## Decisions

### D1: One invariant — an aborted frame mutates nothing — rather than a guard per store

#299's suggested direction was to check `IsAborted()` after the move loop in both functions and
return without storing. Auditing every abort-exposed mutation shows that shape is incomplete:

| Site | Derived from a child result? |
|---|---|
| `:613` `pv_table.update` | yes — the #310 splice |
| `:622-623` `store_killer`, `update_history` | yes — a garbage `beta <= alpha` pollutes ordering for the rest of the search |
| `:658` main `tt.store` | yes |
| `:513` null-move store | yes |
| `:836` quiescence beta-cutoff store | yes — and it returns from *inside* the loop, so a check placed after the loop never runs |
| `:885` quiescence final store | yes |
| `:637` terminal, `:784` stand-pat, `:853` quiescence mate | **no** — derived from `InCheck()`, `ply` and `Eval` alone |

Rejected: a guard at each exposed site. It needs six of them, two of which #299 had not identified,
and every store added to these functions later must remember to be guarded — the failure mode #304's
terminal store already demonstrated.

Chosen: check `IsAborted()` once per recursive call, immediately after the board is restored, and
return `best_value` there. Control then never reaches any of the exposed sites, and the three exempt
sites are unaffected because they are all reached before or without a child search. The exemption
argument for `:637` is #299's; the same reasoning covers `:784` and `:853`, and each deserves a
source comment because the reason it is safe is not local to it.

### D2: No abort sentinel score

#299 proposed giving abort its own signal so it cannot be confused with the genuine
`GameValues::Draw` return at `:443`. With D1 that confusion is unreachable: an aborted frame's
return value is consumed only by a parent that is itself returning immediately. Adding a sentinel
would mean a magnitude that survives negation at every frame and a check at every consumer, for a
value nothing reads.

**This supersedes an approved direction in #299 and must be stated in the PR body.**

### D3: The root keeps its partial iteration

#299's third bullet was to have `iterative_deepening()` keep the previous depth's score outright for
an interrupted iteration. Under D1 that is no longer necessary, and it discards real information:
the root's `best_value` at abort is the best score over the root moves that *did* complete, which is
a valid lower bound, and row 0 of the PV table holds that move's coherent line. Reporting the
partial iteration is standard and is strictly more information than the previous depth's score.

The ±20 cp blind spot in CASE 4 that #299 documents closes as a consequence rather than as a patch:
there is no contaminated `0` left to accept. If no root move completed, `best_value` is still the
`-Search_Init` sentinel and row 0 is empty, so `metrics.current_move` is empty, CASE 5 sees a changed
root move and the iteration is rejected — the existing machinery already handles it.

### D4: PV integrity comes from D1, not from moving `clear_ply`

Rejected: hoisting `clear_ply(ply)` above the two abort returns. It also stops the splice, but it
leaves a length-1 PV naming a move whose subtree was never searched, and it empties row 0 on an
abort-at-root, so `log_search_complete` loses the last coherent line it prints today.

### D5: Assert in Debug at the emission choke point; do not sanitise in Release

`emit_iteration_info` is the single place the reported PV leaves the engine. Assert there, through a
named helper — `pv_replays_legally(root_board, line)` — so the invariant is unit testable without a
search.

Rejected: trimming the emitted line to its longest legally-replayable prefix in Release. It would
guarantee fastchess never warns again while permanently hiding any splice path we have not found,
which is the opposite of what the assertion is for.

### D6: The PV-move revival is a second landing

PR 1's cheapest and strongest correctness proof is that fixed-depth output is bit-identical: no
clock, no abort, so node counts and best moves must be unchanged. Reviving the ordering hint changes
node counts at fixed depth by design and would destroy that proof, and a single SPRT could not
attribute its result to either half. Separate landings keep both measurements interpretable.

### D7: Revive the hint from a snapshot of the accepted PV, not from the live table

`Move pv_move;` (`:469`) is only ever assigned at `:496`, and `clear_ply(ply)` at `:452` runs first,
so `get_pv_move(ply)` always returns an empty move and the 2,000,000 tier in
`MoveSorter::ScoreMoves` (`Sort.cpp:108`) — which outranks `hash_move`'s 1,900,000 — never fires
anywhere. Ordering is `hash_move`-first in practice today.

Rejected: reading the row before `clear_ply`. Row `ply` then holds the line from whatever subtree
last visited that ply in this iteration, a different position; the resulting hint is usually not even
a legal move here.

Chosen: snapshot the accepted iteration's root line, and apply `snapshot[ply]` only while the current
path is still a prefix of that line, so the hint is always legal in the position it is offered for.

## Assumptions I cannot verify from the code

- **Skipping `adjustScoreForGameState` on the unwind leaves no stale per-thread game state.** The
  abort return bypasses `td.update_game_state(ply, …)`, and a stale `STILL_PLAYING`/`DRAW_PAT` entry
  is what `GameState::UpdateFiftyMovesState`'s assertion reads. Would be verified by reading
  `ThreadData::update_game_state` against `init_search`'s reset, then a Debug-build timed self-play
  game with assertions live. Not done.
- **A 0 ms allocation survives `resolve_limits`/`compute_budget`.** The deterministic test in
  Validation depends on it: with a zero budget the first poll at node 1024 latches the abort, and at
  `Threads=1` node counting is deterministic, so the abort point is reproducible bit-for-bit. If the
  budget is floored, the test needs a different seam. Not verified.
- **A legality replay is available outside a search.** `pv_replays_legally` needs a board copy and
  the `GameInfo` threading that `DoMove` expects. Whether that is constructible without a full
  `ThreadData` is unchecked.
- **The fastchess warnings are this defect and not an additional one.** Inferred from the duplicate
  from-square signature and `score cp 0` at depth, not replayed. Would be settled by mining a
  position out of `logs/elo/*.log` and reproducing the illegal line with the zero-budget abort before
  the fix.

## Invariants

- At fixed depth, PR 1 changes nothing: identical node counts and best moves at `Threads=1`.
- No transposition entry is ever stored from a value that depends on a child search that did not
  complete. The three exempt stores depend on `InCheck()`, `ply` and `Eval` only.
- Killers and history are never updated from an aborted child's cutoff.
- Every emitted `info … pv` line replays legally from the search root.
- An aborted frame restores the board before returning, on both the move and null-move paths.

## Validation

Engine tier, both landings.

**PR 1**

- Fixed-depth equivalence: identical node counts and best moves at `Threads=1`, before and after.
- `Run-Bench.ps1` before/after — the change adds one branch per searched child.
- Debug-build tests (`build.ps1 all -Config Debug`), so the new assertion is live.
- Deterministic abort test: search a position with a zero time budget at `Threads=1`, so the abort
  latches at node 1024, and assert the emitted PV replays legally. Falsify it by reverting the guard
  and confirming it fails.
- `-Sprt NonRegression` under time control. TT content changes there, so a fixed-depth equivalence
  result does not cover it. Cost to be reported before starting; the decision to spend it is the
  user's.

**PR 2**

- `Run-Bench.ps1` before/after, plus the per-position column.
- `-Sprt Gain`. Node counts change by design, so there is no equivalence check to lean on.

No perft run: move generation is untouched. Linux Debug + sanitizers come from CI.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the three exempt stores are safe under abort | source comment at each of `:637`, `:784`, `:853` |
| The unwind invariant itself (an aborted frame mutates nothing) | source comment at the guard in `pvs()`, and `CLAUDE.md` → Key Source Facts |
| That the reported PV must replay from the root | the `pv_replays_legally` assertion and its unit test |
| D2 superseding #299's abort-signal direction | PR 1 body, and a closing comment on #299 |
| Bench and SPRT results | `Docs/Changelog.md`, `Docs/EloLog.md`, PR bodies |
| The PV-move hint was dead, and why the naive revival is wrong | source comment where the snapshot is applied |

This file stays until PR 2 lands: while PR 2 is unstarted it is the spec for it. Delete it in PR 2
once the table above is discharged.
