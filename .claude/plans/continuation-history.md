# Continuation history for quiet ordering — Design

**Issue:** #664 (child A3 of epic #636; also covers A2)

## Goal

After the hash move and the killers, quiet moves are ordered by one butterfly table,
`ThreadData::history[side][from][to]`. That table carries no context: a move that answers the
opponent's last move well scores the same as the same move in any other position. In #636's
baseline, 33% of all nodes are spent before the cutting move at late-cut nodes, and 71% of late
cuts come from a killer or a history-ordered quiet. A1 (#651) made the butterfly table rank moves
by how often they cut when tried, and gained +13.2 Elo. The next standard step is to condition
quiet scores on the moves that led to the node.

## Review focus

- **nps cost against node savings.** Each quiet scored reads two entries from a 1.5 KB row (D1, D2),
  and A1 alone cost 2.5% nps. Whether the change pays is decided on wall clock, not on nodes.
- **D6, ageing once per search.** The butterfly table decays every iteration and this table does
  not, so their sum shifts toward continuation entries as the search deepens.
- **D3, the key writes.** They have to be correct across null moves, pruning skips and singular
  re-entry at the same ply.

## Scope

**This change will:**

- add a continuation-history table indexed `[prev piece][prev to][piece][to]`;
- read it at a 1-ply offset (the opponent's last move) and at a 2-ply offset (our own previous
  move), and add both entries to the butterfly score of every quiet in `pvs()`;
- update it at the same sites, with the same bonus and malus, as the butterfly table;
- add one tuning field, `continuation_history_plies` (0, 1 or 2), so the two offsets can be screened
  separately and 0 can be checked for equivalence with the base.

**This change will not:**

- change quiescence ordering. Its `ScoreMoves` call passes no continuation rows;
- use history in LMR, LMP or futility decisions. That is B2;
- add capture history (A4) or a separate countermove table (A2);
- tune the relative weights of the three tables, or the bonus formula.

## Decisions

### D1: One table, indexed by piece and destination on both sides

`int16_t cont[12 * 64][12 * 64]`, row `prev_piece * 64 + prev_to`, column `piece * 64 + to`. The
1-ply and 2-ply offsets share the table, as in Stockfish. Rejected: one table per offset. It doubles
the memory, and the two contexts are the same kind of fact ("after X, Y tends to cut"). Rejected:
from-square indexing. The piece carries more information than the from-square does, and the
destination is what a reply is aimed at.

`piece` is `Board::GetEffectiveMovPiece(move)`, read before `DoMove`. The value is 0..11, and for a
promotion it is the promoted piece. It is only ever a quiet on the column side, where that value is
the piece on `from`.

### D2: `int16_t` entries, heap-allocated, per thread

At 768 × 768 × 2 bytes the table is 1.18 MB. `ThreadData` holds it through
`std::unique_ptr<ContinuationHistory>`, where
`using ContinuationHistory = std::array<std::array<int16_t, 768>, 768>`, and
`std::make_unique` zero-initialises it. Rejected: an inline member. `AIPerplex` holds `td_` by value,
and tests construct `AIPerplex` on the stack (`SearchServiceTests.cpp:133` and others). 1.18 MB
inline would exceed Windows' 1 MB default stack. Rejected: `int32_t`. `HISTORY_MAX` (16,384) fits
in `int16_t`, and half the size halves the cache footprint of the lookups that decide nps.

Each Lazy SMP helper owns its table, like `history`. Nothing is shared, so thread safety is not
affected.

### D3: A per-ply continuation key, written before each child search

`uint16_t cont_key[MAX_PLY + 1]` in `ThreadData`: `cont_key[p]` is `piece * 64 + to` for the move
that led to ply `p`, or `kNoContinuation` (`0xFFFF`) when there is none. `pvs()` writes
`cont_key[ply + 1]` after `DoMove` accepts a move and after the frontier and LMP skip checks, so a
skipped move writes nothing that a child reads. The null move writes `kNoContinuation` to
`cont_key[ply + 1]`. The root's `cont_key[0]` is `kNoContinuation`: the game's last move is known,
but its piece is not recorded, and one node per search does not justify the plumbing.

At a node at `ply`, the 1-ply row is `cont_key[ply]` and the 2-ply row is `cont_key[ply - 1]`
(absent at `ply == 0`). A singular verification re-enters at the same ply and reads the same keys,
which is correct, because it searches the same position.

Rejected: a row pointer per ply. It is 8 bytes instead of 2, and still needs a sentinel.

### D4: The score is the plain sum

A quiet that is neither the hash move nor a killer scores
`history[side][from][to] + cont1[col] + cont2[col]`. An absent row contributes 0. The sum is bounded
by 3 × 16,384 = 49,152, far below the lowest tier above quiets (losing captures, 700,000), so the
tier structure is unchanged.
`ScoreMoves` takes the two rows as `const int16_t*` (`nullptr` when absent), so `Sort.h` does not
learn about `ThreadData`. Rejected: weighting the tables (for example 2× on the 1-ply entry). It is a
tuning question, and it is out of scope.

### D5: The updates mirror the butterfly table

At a beta cutoff by a quiet, the cutting move gets `+history_bonus(depth)` and every earlier
searched quiet gets `-history_bonus(depth)`, in every present row, through the same gravity update.
`apply_history` becomes a template over the entry type, with its arithmetic in `int32_t`, so
`|entry| <= HISTORY_MAX` holds for `int16_t` for the same reason it holds today. The update sites stay
`update_history` and `penalize_searched_quiets`, which gain the two row indices. Each move's column
piece is read from `td.board`, which at both sites holds the node's own position again, because the
cutting move has already been undone.

### D6: Aged once per search, not per iteration

The butterfly table is halved before every iteration (`age_history()`, 8,192 entries). Halving 1.18
MB before every iteration would cost a memory pass that dominates the shallow iterations. The
continuation table is halved once per search instead, per thread: beside `td.clear_killers()` in
`iterative_deepening()`, and beside `htd.clear_killers()` for helpers. It is cleared in
`reset_for_new_game()`. Rejected: never ageing. Stockfish does not age, but here the butterfly table
decays within a search while the continuation table would not, and the sum would drift toward
stale continuation entries from earlier moves of the game.

### D7: The shipped default is chosen by the gate

`continuation_history_plies` is `int`, range 0..2, JSON-bound, not exposed over UCI. At 0,
`ScoreMoves` gets no rows and nothing updates the table. Only the `cont_key` writes remain, and they
do not affect the search. The default is 2, unless the 1-ply build passes the wall-clock gate and the
2-ply build does not, in which case it is 1. The lab runs only on the chosen default. If neither
passes, the change is parked and not merged. The field stays after merge as the kill switch, like
`lmr_enabled`. It is not a compile gate to be removed later.

## Assumptions I cannot verify from the code

- **An inline table would overflow the test stack.** Inferred from the 1 MB Windows default and the
  stack-constructed `AIPerplex` in tests. It was not run. D2 makes it moot.
- **Equal weights are a sensible starting point.** Taken from common engine practice, not measured
  here. Tuning is out of scope. The screen shows whether the unweighted sum helps at all.
- **The nps cost is tolerable.** Each quiet scored adds two reads from a 1.5 KB row. A1 alone cost
  2.5% nps. Not known until `Run-Bench.ps1`. The wall-clock gate is what decides.
- **Per-search ageing is better than none.** Not screened separately. Revisit only if the screen is
  flat.

## Invariants

- `|entry| <= HISTORY_MAX` for every continuation entry, after any sequence of updates.
- With `continuation_history_plies = 0`, the search is node-identical to the merge base at
  `Threads=1`.
- No continuation write survives an aborted child: the updates stay below the existing
  `IsAborted()` guard.
- A row is read only through a key that is not `kNoContinuation`, and every column index is below
  768.
- Tier order in `ScoreMoves` is unchanged: hash > good captures > killers > losing captures > quiets.

## Validation

Engine tier: a search behaviour change.

- **Unit tests** (`StratChessTests`): the gravity bound holds on `int16_t` entries at `±HISTORY_MAX`;
  `ScoreMoves` ranks a quiet with a large continuation entry above one with a larger butterfly entry
  alone; a null move leaves no 1-ply row for its child; at `plies = 0` a search leaves the table
  all-zero.
- **Asserts and static checks:** `static_assert(3 * HISTORY_MAX < 700'000)`, which keeps the quiet
  sum below every other tier. Debug asserts that a row is read only through a key that is not
  `kNoContinuation`, and that every index is below 768. The Linux Debug and sanitizer CI leg runs
  them over the whole suite.
- **Abort invariant:** the continuation updates are added inside the existing `beta <= alpha` block,
  which is below the `IsAborted()` return. `search-reviewer` checks that no new write lands above it.
- **Equivalence:** a build with the default temporarily set to 0, run through
  `Compare-SearchEquivalence.ps1` against the merge base, gives identical nodes and best moves.
- **Screen:** `Compare-SearchProfile.ps1` on `Tests/profile-screen.fen`, depths 12 and 16, 8 seeds,
  against the merge base, for the 1-ply and the 2-ply builds. Pre-registered expectation: late-cut
  work falls by more than the #653 noise floor, and the killer share of late cuts falls.
- **Speed:** `Run-Bench.ps1` nps, clang-cl Release.
- **Gate (#636):** interleaved fixed-depth wall clock, `Threads=1`, against the merge base: median
  <= -3% and faster in >= 8 of 9 rounds, plus the 200-position set.
- **Strength:** a CI strength-lab run of the chosen default against the merge base, on the owner's
  decision.
- **Review:** dispatch `search-reviewer` on the diff.

## Cost

- **Size:** 50–200 lines. Files: `ThreadData.h`, `Sort.h`, `Sort.cpp`, `AIPerplex.cpp`,
  `SearchTuning.def`, tests, `Docs/Changelog.md`, `Measurements/ci-per-change.md`.
- **Blast radius:** Engine tier. No gate, skill or script changes.
- **Review:** one code review, `search-reviewer`, and the cross-agent round.
- **Measurement:** two screen builds, the wall-clock gate, and one lab run (~20k games).

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2 heap allocation and the stack reason | comment on the `ThreadData` member |
| D3 key semantics and the null-move sentinel | comment on `cont_key` |
| D4 sum bounded below the killer tier | comment in `ScoreMoves` |
| D6 per-search ageing and why not per iteration | comment on the ageing function |
| D7 field meaning and the equivalence at 0 | comment in `SearchTuning.def` |
| Screen, gate and lab results | `Docs/Changelog.md`, `Measurements/ci-per-change.md`, PR body |
