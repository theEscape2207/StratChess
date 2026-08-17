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

**This change will**, as three landings:

*PR 0 — a deterministic abort trigger*

- Add an opt-in node limit (`go nodes N`) checked inside the existing poll, and rename
  `ShouldStopSearch()` to `TimeLimitReached()` now that the clock is no longer the only stop reason
  (D8).

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
- Change the clock trigger — `TimeManager`'s limits, the budget formula, or the 1024-node poll
  cadence. PR 0 adds a second stop *reason* inside the existing poll; it does not alter when the poll
  runs or how the clock behaves.
- Rename `should_stop_early()`, which is a mate-distance/PV-length early exit and unrelated to
  stopping on a limit, despite now reading similarly to its neighbours.
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
named helper — `bool pv_replays_legally(const Board& root, std::span<const Move> line)` — so the
invariant is unit testable without a search. It needs no `ThreadData` and no `GameInfo` argument:
`Board` is `= default` copyable (`Board.h:24-27`, with precedent at `UCIHandler.cpp:376`) and
`DoMove` maintains `gameInfo_` itself, so a disposable local copy plus `board.GetGameInfo()` at each
step is sufficient — the pattern `Perft::perft_recursive` (`Tests/Perft.cpp:165-194`) already uses.

**Each step needs both halves of legality, and they are not interchangeable.**
`MoveGenerator::ComputeLegalMoves` is pseudo-legal — it does not test check (`MoveGenerator.h:15`) —
while `DoMove` returns false only when the move leaves its own king in check (`Board.cpp:458-464`)
and otherwise executes whatever from/to/flags triple it is handed, including a geometrically
impossible one. So: test membership in the generated list, *then* `DoMove`. Membership must compare
`flags()` explicitly, because `Move` equality ignores them (`Move.h:64-68`) and would match a PV
move against a different promotion piece.

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

### D8: A node limit, not the clock, is what makes the abort path testable

The abort path cannot be regression-tested through the clock. A zero budget is passed through
verbatim (`SearchLimits.cpp:41-42`; `TimeBudget` documents that a drained clock yields zero and is
never floored) and `should_stop_search()` latches on its first call because `el >= 0ms` always holds —
but that first call is either the poll at node 1024 or, more usually, `:334` right after depth 1
completes, since depth 1 is far too small to reach the poll. The abort therefore lands before any
iteration deep enough to build or emit a spliced line. Deterministic, and useless. Any non-zero
budget is wall-clock dependent and so not deterministic at all.

Chosen: `std::optional<int64_t> nodes` on `SearchLimits`, resolved alongside the others, checked
inside the existing poll block and latching through `TimeManager::stop()` so the whole existing
collapse path is reused unchanged. At `Threads=1` node counting is deterministic, so the abort point
is reproducible bit-for-bit. `go nodes` is also a UCI standard the engine lacks, so this is a feature
rather than test scaffolding.

Only thread 0 polls, so the budget bounds *thread 0's* node count and a `Threads=N` search lands near
N times the budget. The determinism this exists for is therefore a `Threads=1` property, and a
node-limited match is machine-independent only at `Threads=1`.

Rejected: a test-only `abort_after_nodes_` seam — same determinism, smaller diff, but it puts
test-only machinery in the production search and gives a user nothing. Rejected: a probabilistic soak
test over short movetimes — it would go red quickly on the field rate of 0.24 warnings per game, but a
regression test that is only *probably* red is not a regression test.

Because only thread 0 polls, the limit is thread-0's own node count and is therefore approximate
under Lazy SMP, exactly as the clock check already is. That belongs in the `go nodes` documentation.

**Rename.** With two stop reasons, `ShouldStopSearch()` no longer describes what it tests. It becomes
`StopRequested()`, paired with `NodeLimitReached()`, while `IsAborted()` stays the cheap latched read.
One declaration and thirteen live call sites, seven of them in `AIPerplex.cpp` and the rest in the
legacy agents. `Archived/` keeps the old name: it is excluded from every build and is frozen reference
code.

A time-based name was the first choice and is wrong: `should_stop_search()` returns true whenever the
flag is *already* latched, so it answers for a node limit and for UCI `stop` as well as for the clock —
`cmd_stop` has latched it since it existed. Four call sites already rely on that generality. If PR 1
needs to know *which* limit stopped a search, that wants a `StopReason` beside the flag, not a name
that implies one.

## Assumptions I cannot verify from the code

Three earlier assumptions have since been checked and are recorded under Invariants (the game-state
skip), D8 (the zero budget) and D5 (the legality replay). What remains:

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
- The root game state survives the unwind. Returning early skips `adjustScoreForGameState`, hence
  `td.update_game_state`, whose two consumers are `:246` (propagated to `m_Board`) and `:293-296`
  (returned, and fires `EGameStateChanged`). The skip is inert: the function is a no-op for every
  `ply > 0` (`ThreadData.h:214`), `init_search` seeds `info_seq[0]` with the true state, any completed
  iteration has already written `STILL_PLAYING`, and the only other value it writes — mate or
  stalemate — requires `!moveFound`, which an aborted frame cannot be.

## Validation

Engine tier, all three landings.

**PR 0**

- Unit tests: the limit is honoured to the poll's 1024-node granularity at `Threads=1`; an unset
  limit changes nothing; `resolve_limits` precedence against `movetime`, `clock` and `depth`.
- Fixed-depth equivalence with no limit set: identical node counts and best moves at `Threads=1`.
- `Run-Bench.ps1` before/after — the new comparison sits inside the existing 1024-node poll, so the
  expectation is no measurable change; the run exists to confirm that.
- No Elo match. The limit is opt-in and unset in every match and in `game_settings.json`, so with the
  equivalence result above there is no strength question to answer.

**PR 1**

- Fixed-depth equivalence: identical node counts and best moves at `Threads=1`, before and after.
- `Run-Bench.ps1` before/after — the change adds one branch per searched child.
- Debug-build tests (`build.ps1 all -Config Debug`), so the new assertion is live.
- Deterministic abort test, on PR 0's node limit: search at `Threads=1` with a limit chosen deep
  enough that an interrupted iteration is accepted, and assert the emitted PV replays legally and no
  transposition entry was written from an incomplete child. Falsify it by reverting the guard and
  confirming it fails.
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
| `go nodes` semantics, including that it is thread-0's count under Lazy SMP | doc comment on the new `SearchLimits` field, and `Docs/Changelog.md` |
| Why the clock cannot trigger a deterministic abort deep in the tree | `Docs/TestDesign.md`, next to the abort-path tests |

This file stays until PR 2 lands: while PR 2 is unstarted it is the spec for it. Delete it in PR 2
once the table above is discharged.
