# Multi-PV root ranking — Design

**Issue:** #94 (slice 1 of 2, per the [2026-11-20 re-triage](https://github.com/theEscape2207/StratChess/issues/94#issuecomment-7052168048)); consumer #985

## Goal

#985 (`UCI_LimitStrength`/`UCI_Elo`) weakens the engine by picking among the best few root moves,
with a penalty proportional to each move's gap to the best. Today the search knows one exact root
score. Every other root move ends as a null-window bound that says only "no better than the best".
So there is no gap to penalise. This change makes one search return the top N root moves, each with
an exact score and its own line, all from the same accepted depth. `N = 1`, the default, must stay
search-identical to today.

## Review focus

- **D2, the exclusion test inside `pvs()`.** It is the only change on the per-node path, at
  `N = 1` too. Is its cost really one compare per node, and is its folding into the singular
  exclusion test sound (D2 says the two can never meet in one frame)?
- **D3, the root TT guard.** Is suppressing the root store for ranks 2..N enough to keep every TT
  entry a full-move-set result, at `Threads > 1` too?
- **D5/D6, sorting and interrupted depths.** Rank 1 after sorting drives the iteration policy, and
  `N > 1` keeps `ACCEPTED_INTERRUPTED` only before the first complete set. Is anything in `IterationPolicy` or time management
  unsound under that?

## Scope

**This change will:**

- add `AIPerplex::SetMultiPV(unsigned)`, a configuration setter like `SetThreads`;
- at `N > 1`, run one full root search per rank at each depth, excluding the moves already ranked;
- report ranks 2..N through `SearchResult` and `IterationInfo`, beside the unchanged rank 1;
- add tests, a hidden fixed-depth node-multiplier test for `N = 4`, and the contract in
  `Docs/EngineContracts.md`.

**This change will not:**

- add a UCI `MultiPV` option or `multipv` info fields. That is slice 2, for #91 and #68;
- add `UCI_LimitStrength`, `UCI_Elo` or any pick policy (#985);
- add `searchmoves`, or run multi-PV on the Lazy SMP helpers;
- measure fixed-time depth loss. Without a UCI option the timing tools can't drive `N > 1`, so it
  moves to slice 2;
- predict a set's cost before starting it. A set costs about N depths, and with no interrupted
  acceptance (D6) a set the hard limit cuts is wasted. A rule such as "don't start a set past a
  fraction of the soft limit" is for slice 2 or #985 to measure.

## Decisions

### D1: Result shape: rank 1 stays where it is; ranks 2..N are added beside it

`SearchResult` and `IterationInfo` gain `std::vector<RootLine> alternatives`, empty at `N = 1`.
`RootLine` is `{Move move; int score; std::vector<Move> pv;}`, and `pv` starts with `move`.
`best_move`, `best_score` and `pv` keep meaning rank 1.

Rejected: a `lines` vector holding every rank, rank 1 included. It either duplicates `best_move`,
`best_score` and `pv` (two copies to keep in step) or moves them, which touches every consumer and
test of `SearchResult` for no gain. The triage's separate "result shape" slice existed only because
of that move, so it disappears.

`seldepth` stays one value for the whole set: the deepest ply over all ranks, which is what
`td.seldepth` gives when it is reset per depth, as today, and not per rank. Nothing reads a
per-rank one yet. Slice 2 may add it if the UCI lines want it.

### D2: Root exclusion is a list in `ThreadData`, tested inside `pvs()`'s move loop

`ThreadData` gains `root_excluded` (a fixed array of `MoveList::MAX_MOVES` moves) and
`root_excluded_count`. They sit at the end of the struct, beside the other cold members, so hot
offsets don't move. The multi-PV loop appends each rank's move before searching the next rank. An RAII guard,
the pattern of `ExcludedMoveGuard`, empties the list when a depth's ranks end, on every exit.
`helper_loop` asserts the count is 0 on entry.

In `pvs()`:

```cpp
const bool root_exclusion_frame = ply == 0 && td.root_excluded_count != 0;
const bool any_exclusion = is_exclusion_frame || root_exclusion_frame;
...
if (any_exclusion && is_excluded(td, ply, move, is_exclusion_frame))   // replaces the singular test
    continue;
```

`is_excluded()` tests `excluded_move[ply]` in a singular frame, else scans `root_excluded`.

The two kinds can't meet in one frame. A singular verification runs at its parent's ply, and
singular eligibility requires `ply > 0`, so no singular frame is ever at ply 0. A root exclusion
frame is only at ply 0. `is_exclusion_frame` keeps all its other meanings: no TT probe, no PV
write, non-PV assert, no null move. A root exclusion frame has none of them. It is an ordinary PV
root that may not play certain moves.

Cost at `N = 1`: one `ply == 0` compare per node, a load of `root_excluded_count` at the root
alone, and one test of the resulting bool for D3's ordering hint. The per-move test is still one branch on one bool, as today. Whether that is measurable is
a `Run-Bench` question (Validation).

Rejected:

- **Reusing `excluded_move[]`.** It has one slot per ply, is live only with singular extensions
  on, and carries the non-PV meanings above. (The 2026-11-01 triage named this trap.)
- **A separate root search function** that loops over root moves and calls `pvs()` at ply 1. It
  costs nothing at `N = 1`, but it duplicates the root's move loop: ordering, staged generation,
  the PV write and the aspiration fail-high handling. The copy would drift. (Not that rank 1 would
  search a different tree: from depth 2 it does anyway, since ranks 2..N feed the killers, history
  and the root's TT entry.)
- **`pvs<bool Root>` as a template.** It removes the compare, but it doubles the hottest function's
  code. #555 showed code layout alone swings nps by 3.9%, so it isn't free either.
- **Filtering the root move list at generation.** At depth 1 and 2 the root can use staged
  generation, which appends quiets later. So there are two insertion points to filter, in exchange
  for one compare.

### D3: A root exclusion frame stores nothing at the root key

Rank k > 1 searches a smaller move set. Its root result, and its best move, are not the position's.
So the main TT store at the end of `pvs()` and the terminal store are both guarded on
`!any_exclusion`, not `!is_exclusion_frame`. With `N` clamped to the legal move count (D4), a root
exclusion frame always has a legal move left, so its terminal branch is unreachable. That is
asserted.

Nothing else needs a guard. Every frame below the root searches its own position's full move set,
so its entry is true whatever the root excluded. The root probe stays: the root is a PV node, so it
never cuts off. It supplies a hash move, which the loop may skip as excluded, and a static eval.
Killer and history writes at the root follow from real moves searched in the real position, so they
stay too.

Helpers run plain `N = 1` searches (D7), so their root stores are full-move-set results.

**Root ordering hint for ranks 2..N.** The root TT entry names rank 1's move, which every later
rank excludes. So without help, rank k's first full-window move is chosen by capture, killer and
history scores alone, and a wrong first move costs a PV re-search. So in a root exclusion frame,
`hash_move` is the previous accepted set's rank-k move, `td.root_rank_hint`, when that move is not
excluded. Otherwise the probed move stands. It is set inside the `root_exclusion_frame` branch, so
`N = 1` never reaches it. Stockfish orders root moves by their previous scores to the same end.

### D4: `N` is configuration, clamped twice

`SetMultiPV(n)` clamps to `[1, MoveList::MAX_MOVES]` and has the same idle-only precondition as
`SetThreads`. `StartNewGame()` keeps it. At search start the effective count is
`min(N, legal root moves)`. A root with no legal move takes today's path whatever `N` is. A root
with one legal move runs as `N = 1`.

Rejected: a field in `SearchTuning`. `SetTuning` clears the TT on any change, and nothing about
`N` invalidates stored scores. Nor is `N` a strength parameter the tuning catalogue should sweep.

### D5: One iteration at `N > 1` is all ranks; the sorted set's rank 1 drives the policy

At each depth, with `N > 1`:

1. clear `root_excluded`;
2. for k = 1..N: search the root, through the same full-window/aspiration branch as today, with
   the rank-k score of the previous accepted set as the aspiration seed. Copy row 0 into a
   `RootLine` and append its move to `root_excluded`;
3. if every rank completed (D6), stable-sort the set by score, descending. Write the new rank 1's
   line back into PV row 0 (a new `PVTable::set_line(0, …)`). Build the `IterationSample` from it,
   with nodes summed over the ranks, and assess as today.

Writing rank 1 back into row 0 means every reader of row 0 works unchanged: `accept_root_pv`,
`emit_iteration_info`, the logs and the replay asserts. `emit_iteration_info` and
`accept_root_pv` also copy ranks 2..N into the outgoing `alternatives`.

**Why sort.** Rank k's score is exact for the best move outside ranks 1..k-1. But search
instability can still put a later rank above an earlier one: a move searched with a null window in
rank 1 gets its own full PV search in rank k. The set has to be sorted best to worst. Otherwise
#985's gap `best − score` and #91's `rank1 − rank2` can come out negative. Sorting can make
`bestmove` differ from the unrestricted search's move. That is the trade Stockfish makes too
(Assumptions). At `N = 1` there is nothing to sort.

**Aspiration seeds.** Rank k is seeded from the previous accepted set's k-th score: the
sorted set's k-th best, which is the closest prior estimate for "the best move outside k-1 better
ones". Depth 1 is full window for every rank, as today, and so is a rank whose seed is a mate score
(`|seed| >= Mate_Threshold`): a window around a mate score keeps failing until the retry cap
opens it anyway. Rank 1 never meets that, because the mate stop ends the search first.
`search_with_aspiration()` is reused unchanged.

Rejected: keeping the unrestricted rank 1 fixed and sorting only ranks 2..N. It keeps `bestmove`
equal to the `N = 1` choice, but it leaves the set unsorted whenever rank 2 beats rank 1. That is
exactly the case #985 and #91 can't handle.

### D6: A depth counts only when every rank completed; no `ACCEPTED_INTERRUPTED` at `N > 1`

A rank is complete when its search returned without `IsAborted()`. That reads the latched abort, so
a stop that lands after an out-of-window `pvs()` counts too. At `N > 1` the sample's `interrupted`
is `!all_ranks_complete`, not today's `StopRequested()` read at sample time. Otherwise a complete
set whose hard limit latched just after its last rank would be assessed as interrupted. An
incomplete set goes to `assess_iteration` with a null move, which is today's `INCOMPLETE`
rejection, so the previous accepted set stands. The final result is always the last complete set.

One exception: when no set has completed yet but rank 1 of the first depth has, that rank-1 line is
assessed alone, as an interrupted depth with empty `alternatives`, exactly as at `N = 1`.
Otherwise a tiny clock or node limit would trade a searched move for the emergency path's first
legal move. So at `N > 1`, `ACCEPTED_INTERRUPTED` is reachable only while `depth_completed == 0`.
That is asserted.

Rejected: keeping `ACCEPTED_INTERRUPTED` for rank 1 alone. Its partial rank 1 would come from depth
d while ranks 2..N came from d-1, and sorting across depths compares unlike scores. This mode is for
analysis and weakened play. The few Elo the interrupted acceptance buys belong to full-strength
`N = 1` play, which keeps it.

The rest of the policy reads rank 1 only, as the triage asked: `move_run`, the soft-limit
scaling, the mate stop and `search_was_stable`. The soft limit is checked once per completed set,
not per rank, so a set is never cut short by the soft limit, only by the hard one.

### D7: Lazy SMP helpers stay `N = 1`

Helpers keep warming the shared TT with plain searches, and thread 0 is still the only publisher,
as the triage asked. A helper never reads `root_excluded`: its count is 0, set at construction and
never changed. Node accounting is unchanged: every thread's nodes are summed once in `Search()`.

Rejected: helpers searching ranks in parallel. It needs a parallel-root design, and #985 runs at
`Threads = 1` anyway.

## Assumptions I cannot verify from the code

- **#985 picks from one accepted depth's set, read through the observer or the final result.**
  #985's pick depth comes from the level, and its design isn't written yet. Not verified. If it
  needs something else, such as scores at several depths at once, D1's shape still carries it
  through the per-depth observer.
- **Stockfish sorts its MultiPV set and lets the top move change.** Taken from the #94 body and
  #985's reference-design summary of Stockfish's `search.cpp`, not re-read. D5's case rests on
  #985's gap arithmetic, not on Stockfish, so the decision stands either way.

## Invariants

- `N = 1` is search-identical to `origin/main`: same depths, best moves, scores, PVs and node
  counts at `Threads = 1`.
- At `N > 1`, a result's `best_move` plus its `alternatives` are distinct legal root moves. The
  scores are non-increasing. Each `pv` starts with its move and replays legally. All come from one
  completed depth, `depth_completed`.
- `alternatives.size() == min(N, legal moves) - 1` whenever a depth completed. It is empty for an
  emergency move, a terminal root, and at `N = 1`.
- No root exclusion frame writes the TT at the root key.
- `root_excluded_count` is 0 whenever no multi-PV loop is running, on every exit, including a stop.

## Validation

**Engine tier.** It changes `pvs()` and the iterative-deepening loop.

- **Equivalence:** `Compare-SearchEquivalence.ps1 -After <worktree build>` at `N = 1`: identical
  node counts, best moves and scores.
- **nps:** a `Run-Bench` before/after pass at `N = 1`, read with #555 in mind (3.9% from code layout alone). The
  target is no measurable change. A slowdown beyond the noise sends D2 back for the rejected
  alternatives.
- **Tests** (new `SearchMultiPVTests.cpp`, `[multipv]`):
  - the invariant above on several positions at `N = 2, 4`;
  - `N` above the legal count clamps (a position with 3 legal moves at `N = 5`);
  - terminal roots (mate, stalemate) return no move and no alternatives at `N = 4`;
  - mate ordering: a mate-in-1 among the moves ranks first, and mated lines rank last;
  - the TT guard: a position with one clearly best move (a free queen), `N = 4`, `Threads = 1`:
    the root TT entry's move is the best move at the final depth. Without D3's guard, rank 4
    writes last and the test fails. To be falsified by reverting the guard;
  - stop during rank N: a node limit that ends the search mid-set returns the previous depth's
    complete set, and `root_excluded_count == 0` afterwards;
  - a node limit just above depth 1's rank 1 returns that rank-1 move, not the emergency move;
  - every `[multipv]` invariant test runs with `SingularExtensions` on and off, since the folded
    exclusion test has a branch for each;
  - `Threads = 2` at `N = 3`: the invariant holds, and so does the TT guard. Node aggregation in
    `Search()` is not touched, so the existing `[smp]` accounting tests cover it;
  - the observer sees `alternatives` sized as above on every emitted depth.
- **Characterisation, reported with no threshold:** a hidden `[.multipv-cost]` test runs the bench
  positions at fixed depth with `N = 1` and `N = 4`. It prints the node multiplier, and the share
  of depths where sorting changed rank 1, which sizes D5's trade. That is deterministic, so it needs
  no quiet window. Printing the multiplier with D3's ordering hint off as well is optional.
- **Debug/sanitizers:** the Linux Debug CI run covers the replay and terminal asserts.

**No Elo match.** `N = 1` is proved search-identical, and `N > 1` is an opt-in mode with no
strength target.

## Cost

- **Size:** over 200 lines, of which about half is tests. Files: `AIPerplex.h/.cpp`,
  `ThreadData.h`, `PVTable.h`, `SearchResult.h`, a new test file, `Docs/EngineContracts.md`,
  `Docs/Changelog.md`.
- **Blast radius:** Engine tier, through `pvs()`'s move loop and store guard and the ID loop.
- **Review:** one code review (170-270k tokens), plus `search-reviewer` on the diff.
- **Optional parts:** the `[.multipv-cost]` characterisation test (about 30 lines). It covers no
  risk and only sizes `N = 4` for #985's time use. Drop it, and slice 2 measures through UCI.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2: root exclusion vs singular exclusion, why they can't meet | source comment at the exclusion test in `pvs()` |
| D3: no root store under root exclusion | source comment at the store guard; `Docs/EngineContracts.md` → Search internals |
| D5/D6: one iteration = all ranks, sorted, no interrupted acceptance | `Docs/EngineContracts.md` → The search service; comment on the multi-PV loop |
| D1: `alternatives` meaning | comment on `SearchResult::alternatives` |
| `N = 4` node multiplier | PR body and `Docs/Changelog.md` headline |
