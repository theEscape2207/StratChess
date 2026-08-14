# Issue #237 — search instability: execution status

Living status document for the #237 work sequence. The issue body is the authoritative
specification; this file records **where the work stands, what was decided beyond the issue, and
what the next session should pick up**. Delete it when #237 closes.

Last updated: 2026-08-14.

---

## Standing decisions

These were settled during re-review of the issue against the code at `d5f3520` and are not to be
relitigated without new evidence.

1. **Stages ship as separate PRs, in order, and merge before the next one starts.** Each stage
   changes TT contents, node counts and PVs. Bundling destroys causal attribution for `WAC-287`,
   which is the only thing this issue is actually trying to explain.
2. **Instrumentation is stage 0 and is search-semantics-preserving.** Its correctness gate is
   fixed-depth identity (same best move, score and node count at `Threads=1`), not runtime
   equivalence — per-depth output costs time in clocked searches.
3. **No monotonicity test.** Best move legitimately changes with depth. The instrument exposes the
   transition; a human decides whether a given transition is valid.
4. **`Threads=1` for every diagnostic run.** Lazy SMP makes TT contents nondeterministic, which is
   the opposite of what these measurements need.
5. **Test-data changes are a separate, non-blocking track** (stage 4). They must not be folded into
   a search PR.

## Findings added on top of the issue body

Verified against the checkout; the issue does not mention these.

- **`send()` is unsynchronised.** `UCIHandler.h:71` declares `static void send(std::string_view)`
  writing straight to stdout. Today the search thread emits exactly two lines, at the very end of
  the search, so interleaving with the command loop is a narrow window. Per-iteration `info` widens
  it to the whole search: a `go infinite` search emitting info while the main loop answers
  `isready` can tear a line, and a torn line is a protocol violation that a match runner resolves by
  forfeiting (the failure mode of #245). **Stage 0 must serialise `send()`.**
- **There are two accept branches, not one.** `iterative_deepening()` accepts at
  `AIPerplex.cpp:355` (`ACCEPT_AND_CONTINUE`) and at `:379` (`ACCEPT_AND_STOP`). Only the first
  calls `log_completed_iteration()`. Emitting from that call site alone would drop the final
  iteration of every clocked search — exactly the iteration that has to agree with `bestmove`.
- **Cumulative node counts diverge under Lazy SMP.** `iterative_deepening()` sees only
  `td.nodes_searched` (main thread); the final `info`/`bestmove` line reports the cross-thread total
  assembled at `AIPerplex.cpp:256-259`. At `Threads=1` these are equal. Stage 0 reports the
  main-thread figure per iteration and leaves the final line alone; this is documented, not fixed.
- **The absolute-ply bound stage 2 asks for is genuinely absent, and the exposure is smaller than it
  looks.** `pvs()` indexes `td.killers[ply]` and `td.last_move_was_null[ply+1]` unguarded, and both
  are `[MAX_PLY]` (256, `defines.h:101`). `quiescence()` touches neither, and `PVTable` guards every
  ply access (`PVTable.h`). So removing the `MAX_QSEARCH_DEPTH` cap for in-check nodes risks stack
  exhaustion on a perpetual-check line rather than an out-of-bounds write. Bound it anyway.
- **Defect B's magnitude is confirmed arithmetic, not an estimate.** `Search_Init = 50000`
  (`defines.h:108`); `static_cast<int16_t>(-50000)` is well-defined modulo 2^16 since C++20 and
  yields **+15536**. `normalize_for_storage()` leaves it alone (below `Mate_Threshold = 29900`), so
  it is stored verbatim as an UPPER bound. As a cutoff it is nearly inert — a probe must have
  `alpha >= 15536` to trigger it — so the damage is the poisoned hash move and the never-cached
  mate score, not spurious cutoffs. Fix it because it is wrong, and do not expect it to move
  `WAC-287` on its own.

## Open questions for the user

- **Stage 4 semantics.** Does a tactical test assert "the puzzle key move" or "any objectively
  equivalent decisive continuation"? The issue explicitly defers this. Nothing in stage 4 can be
  implemented until it is answered, and #235's admission criteria probably want the same answer.
- **Measurement budget for stage 2.** Legal in-check qsearch adds nodes by design. Deciding it needs
  `Run-Bench.ps1` plus an SPRT, not a fixed batch. A `-Sprt NonRegression` run is unattended hours;
  the call is the user's.

---

## Stage board

| # | Stage | Kind | Status | Branch / PR |
|---|---|---|---|---|
| 0 | Per-iteration UCI `info` + `send()` serialisation | semantics-preserving | **in progress** | `worktree-237-uci-per-iteration-info` |
| 0b | `WAC-287` baseline sweep, depths 4-12 | evidence | blocked on 0 | — |
| 1 | Terminal-node TT storage (defect B) | search change | not started | — |
| 2 | Legal qsearch while in check (defect A) | search change | not started | — |
| 3 | Qsearch TT depth → remaining budget (defect C) | search change | not started | — |
| 4 | Tactical test-data semantics (`WAC-043`, `WAC-065`) | test data | blocked on user decision | — |

### Stage 0 — instrumentation

Pinned design (beyond what the issue specifies):

- Snapshot type is a value type carrying `depth`, `score`, cumulative main-thread `nodes`, and a
  **copied** PV (`pv_table.get_line(0)` truncated to `get_length(0)`), taken before the next
  iteration mutates the table.
- The observer lives on `AIPerplex`, not on `PlayerAiBase`. `cmd_go` already reaches the concrete
  type by `dynamic_cast` (`UCIHandler.cpp:375`); reuse that rather than widening the base class for
  one consumer. Register before spawning `search_thread_`, clear after the search.
- Emit from a single helper called in **both** accept branches, after `state` is updated. Never on
  `REJECT_AND_STOP`.
- Mate/cp formatting is extracted from the existing final-line code (`UCIHandler.cpp:379-388`) and
  shared, so the last `info` cannot drift from `bestmove`.
- `send()` gains a mutex (or equivalent) covering the whole line write.
- Helper threads stay silent for free: `helper_loop()` has its own loop and never enters
  `iterative_deepening()`.

Equivalence gate: build a baseline binary from `origin/main` **before** touching anything
(`run-tests` does not rebuild the exe), then compare `bestmove`, final score and node count at fixed
depth, `Threads=1`, fixed Hash, across a small FEN corpus.

### Stage 0b — baseline

Once stage 0 merges, record for `rn3k1r/pp2bBpp/2p2n2/q5N1/3P4/1P6/P1P3PP/R1BQ1RK1 w - - 0 1` at
depths 4-12, `Threads=1`, fixed Hash, fresh process per depth: per-iteration score, full PV,
cumulative nodes, selected move. This table is the comparison basis for stages 1-3 and belongs in
this file, not in a chat transcript.

### Stages 1-3

Specified in the issue body. Each needs: the pre-change sweep, a focused regression test for the
exact invariant, the fix alone, the identical post-change sweep, the full suite, a `search-reviewer`
dispatch, and — for 2 and 3 — bench plus SPRT.

Stage 1 is the smallest and the least likely to change anything observable. Stage 2 is the one with
a plausible mechanism for `WAC-287`'s depth-8 score reversal, and the one that costs nodes. Stage 3
is a rename-and-invert with a narrow blast radius.

---

## Resuming in a later session

1. Read this file and the #237 issue body.
2. `Get-Worktrees.ps1` for drift and PR state.
3. Pick up the first stage on the board that is not merged; the stages are strictly ordered except
   stage 4.
4. Re-check assumptions against `origin/main` before executing — this document ages.
