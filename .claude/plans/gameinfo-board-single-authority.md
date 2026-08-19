# Board as the single authority for position metadata — Design

**Issue:** #348 (Stage 1)

## Goal

`GameInfo` is maintained in three independent instances by two update paths that nothing forces to
agree. Move generation takes its `epSquare` and `castlingRights` from a `GameInfo` supplied by the
caller, while the position itself — and the Zobrist hash keyed on it — lives on the `Board`. A stale
or mismatched `GameInfo` therefore produces illegal moves against a hash that says otherwise, and no
test stands in the way. On top of that, the search maintains `ThreadData::info_seq`, a per-node
shadow copy of state the board already tracks with an O(1) undo stack.

This change makes the `Board` the only object that answers "what is the en-passant square, what are
the castling rights, what is the halfmove clock", deletes the shadow, and replaces the mutable
`GameInfo&` out-parameter of `GetMove()` with a returned `SearchResult`.

## Scope

**This change will:**

- Add narrow `const` accessors to `Board`: `ep_square()`, `castling_rights()`, `halfmove_clock()`.
- Drop the `const GameInfo&` parameter from `MoveGenerator::ComputeLegalMoves`, `ComputeCaptures`,
  `GeneratePawnCaptures` and `AddCastleMoves`; they read the board they are already given.
- Delete `ThreadData::info_seq` and its helpers (`get_last_info`, `store_info_at_ply`,
  `add_move_to_seq`, `add_null_move_to_seq`, and `info_seq`'s share of `update_game_state`).
  `check_draws` reads `board.halfmove_clock()`; `update_game_state` writes only `board.SetGameState`.
- Move `SearchResult` out of `AIPerplex.h` into its own header and add a `GameStates game_state`
  field.
- Change `IPlayer::GetMove(GameInfo&, const SearchLimits&) -> Move` to
  `GetMove(const SearchLimits&) -> SearchResult`, across `PlayerBase`'s convenience overload,
  `AIPerplex`, `PlayerHuman`, `AIBasic`, `AIAgent` and `ABIterative`.
- Drop the `const GameInfo&` parameter from `PlayerHuman::IsAnyLegalMoves`.
- Reduce `Game::gameInfo_` to a `GameStates game_state_`, delete `Game::SetGameParams`, and move the
  fifty-move adjudication to `Game::Run` after its own `DoMove`.
- Retire `IPlayer::EGameStateChanged` and `Game::OnGameStateChanged`.
- Re-scope `PlayerAiBase::m_infoSeq` to what the legacy agents actually use it for — the `lastMove`
  history behind `GetParentMove()` — and say so in a comment.

**This change will not:**

- Split `GameInfo` into a reversible-position record and an outcome type, or merge it with the three
  parallel `MAX_PLY` undo arrays. That is **#348 Stage 2**, which stays open after this lands.
- Shrink `GameInfo` from 20 to 16 bytes, or give `GameStates`/`eSquare` explicit underlying types —
  **#292**. Stage 1 removes the per-node `info_seq` copy but leaves `gameInfoHistory_` alone, which
  re-scopes #292 rather than closing it.
- Fix the quiet-promotion halfmove-clock bug — **#351**. Both update paths share it identically,
  which is precisely why it does not threaten the equivalence claim below; fixing it here would.
- Change `Board::GetGameInfo()`'s by-value return, or migrate the ~40 existing
  `GetGameInfo().<field>` test reads to the new accessors — **#355**.
- Touch `GameState.h`: neither the defaulted constructor/destructor nor the `fullMoveCount{0}` vs
  `Reset()`'s `1` inconsistency — **#356**.
- Add the halfmove clock to the transposition-table identity — **#347**.
- Remove the `dynamic_cast`s in `UCIHandler` and `Game.cpp`, or otherwise decouple `AIPerplex` from
  `PlayerAiBase` — **#256**. `GetMove` returning a `SearchResult` narrows the gap; it does not close
  it, because `SetIterationObserver` still needs the downcast.
- Rewrite or retire the legacy agents, or cap `PlayerAiBase::Quiescent` — **#307**.

The will-not list is what stops this becoming the #256 rewrite. Every entry has a home issue; none is
being dropped on the floor.

## Decisions

### D1: Move generation reads the `Board`, and takes no `GameInfo`

The four entry points lose the parameter entirely rather than keeping it as an optional override.

Rejected: keeping the parameter and asserting it agrees with the board. That preserves the ability to
pass an inconsistent object — the defect — and pays an assert per node for the privilege.

This is close to free. Of the 29 call sites across the four entry points, 21 already pass
`board.GetGameInfo()`; the other 8 are AIPerplex's `info_seq` and the legacy agents' `m_infoSeq`,
both of which this change removes from the picture anyway. `Perft.cpp:171` even carries the comment
*"MoveGenerator needs latest
GameInfo, which only board has — so ignore passed info"*. `GetAttackBoard` already read
`board.GetGameInfo().epSquare` directly, so the two halves of castle-move generation currently
consult different objects for related state.

### D2: Narrow `const` accessors, not a `const&` `GetGameInfo()`

`ep_square()`, `castling_rights()` and `halfmove_clock()` are what the hot path needs; handing out a
reference to the whole struct would put `gameState` and `lastMove` back on the surface that this
change exists to take them off.

Rejected: `const GameInfo& GetGameInfo()`. It fixes #355's write-through-a-temporary trap for every
field in one line, which is genuinely attractive — but it also hands out a reference whose lifetime is
tied to a `Board` that `DoMove`/`UndoMove` mutate under it, which is the shape of the bug #308 reports
one level up. Left to #355 to decide on its own terms.

### D3: `info_seq` is deleted; the legacy `m_infoSeq` stays, re-scoped

For everything AIPerplex reads, `info_seq[ply]` is redundant with `td.board`. Three legs, all
enumerated rather than assumed:

- Move generation reads only `epSquare` and `castlingRights`.
- `pvs()` and `quiescence()` never read `gameState` or `lastMove`; `GetParentMove` and `lastMove` have
  zero occurrences in `AIPerplex.cpp`/`.h`.
- `halfmoveClock` is computed identically by both paths, because `board.GetPiece(m.to())` after
  `DoMove` equals `GetEffectiveMovPiece(m)` for every move type — promotions included, which is where
  #351 lives — and the null-move path snapshots the board rather than recomputing.

The castling logic differs in structure between the two paths but not in outcome: `Board::DoMove`
gates the from-square strip on the moving piece being a rook, `UpdateCastlingState` does not. If the
right is still held, the rook is by definition still on that square, so no other piece can move from
it. The gate is unreachable-different.

`PlayerAiBase::m_infoSeq` is kept because the legacy agents feed `GetParentMove(ply)` — that is,
`GameInfo::lastMove` — to `MoveSorter::SortMoves`. Removing it would change legacy move ordering,
which nothing here measures.

### D4: The output semantic moves from a mutable parameter to a returned `SearchResult`

`Docs/ArchitectureReview-2026-08.md:97` requires the output semantic to survive, not the carrier, and
warns specifically against keeping the `GameInfo&` merely to preserve behaviour.

Rejected: keeping `GetMove(GameInfo&, ...)` and simply sourcing its contents from the board. Smaller
diff, but it leaves the search writing into a caller-owned struct whose fields nobody reads.

The out-parameter turns out to be almost entirely dead. `UCIHandler` copies a `GameInfo` into its
search lambda and discards the mutated result, taking everything it reports from `GetLastResult()`.
`Game::gameInfo_` has exactly one field ever read — `gameState`, in `IsStillPlaying()` and
`PrintStateMessage()`; `epSquare`, `castlingRights`, `halfmoveClock`, `fullMoveCount` and `lastMove`
are written by `SetGameParams` and by `GetMove`, and read nowhere. That makes `SetGameParams` dead and
collapses `gameInfo_` to a `GameStates`. One test — `FiftyMoveRuleTests.cpp:137` — asserts on the
mutated object; see D5.

`SearchResult` moves to its own header because `IPlayer` cannot include `AIPerplex.h`. `PlayerHuman`
and the legacy agents fill `best_move` and `game_state` and leave the search counters at their
defaults. `GetLastResult()` is kept as-is: the test suite uses it in seven places, and churning that
buys nothing here.

### D5: The fifty-move draw is adjudicated by `Game`, not by the search

Today `AIPerplex::GetMove` calls `info.UpdateBoardInfo(bestMove, ...)` on the caller's struct as its
last act, and `UpdateHalfmoveClock` sets `DRAW_50_MOVES` as a side effect — after the state event has
already fired, so the draw reaches `Game` only through the out-parameter. `Game.cpp:262` carries a
comment explaining that the move must therefore be committed before the game-over test, because the
draw is reported for a position the board has not reached.

`SearchResult::game_state` carries only what the search adjudicates at its own root: `WHITE_WON`,
`BLACK_WON`, `DRAW_PAT`, `STILL_PLAYING`. `Game::Run` checks
`board_.halfmove_clock() >= HALFMOVE_CLOCK_LIMIT` after its own `DoMove`, where the clock is a fact
about a position that actually exists. The apologetic comment goes away with the thing it apologised
for.

Rejected: having `AIPerplex` compute the post-move state so game-mode behaviour stays bit-identical.
It preserves the current output exactly, but keeps adjudication inside the search — against the field
ownership this issue exists to establish — and it would have to synthesise a position the board never
visits.

`FiftyMoveRuleTests.cpp:137` currently asserts `info.halfmoveClock == 100` and
`info.gameState == DRAW_50_MOVES` on the returned struct. It is rewritten to play the returned move
and assert on the board — which is what it was always trying to express.

### D6: `EGameStateChanged` is retired

`Game` is its only subscriber. Once `SearchResult::game_state` exists, keeping the event means the
outcome reaches `Game` through two channels that nothing forces to agree — a smaller copy of the
defect this issue is about. `PlayerHuman` returns `HUMAN_EXITED` in `game_state` instead of firing it;
`Game::HasHumanExited` already keys off the null move and is unaffected.

Rejected: keeping the event and having `Game` ignore the returned state. That is the status quo with
an extra field, and it leaves `IPlayer` carrying an event nobody needs.

### D7: A committed Debug-only invariant check, deleted in the same PR

D3's whole case is that `info_seq[ply]` agrees with `td.board` at every node. The first commit on the
branch adds a Debug-only assert to that effect — `epSquare`, `castlingRights` and `halfmoveClock` at
each node, plus root `gameState` — and the removal commit deletes it along with `info_seq`.

Rejected: an uncommitted local probe. Same evidence, but it survives only as prose in a PR body,
outside git. Rejected: relying on `Compare-SearchEquivalence` alone — it covers the UCI path and
cannot distinguish "redundant" from "coincidentally equal on the positions we happened to search".

The assert must run against the **Debug** build; Release hides exactly this class of thing.

## Assumptions I cannot verify from the code

Most of what this design rests on was enumerated rather than assumed — every `GameInfo` parameter,
call site, field read and `GetMove` caller outside `Archived/` was counted. Three items are left.

- **Game mode has no automated coverage, so D5 and D6 are verified by reading, not by tests.**
  `Game::Run` drives a console loop; the fifty-move and `HUMAN_EXITED` paths are reachable only
  interactively or through a very long self-play game. Would be settled by an AI-vs-AI run via the
  `self-play-validate` skill for the ordinary path, plus a new `Game`-level test for the fifty-move
  transition. The self-play run will be done; the `Game`-level test is proposed, not assumed.
- **`Game::gameInfo_.gameState` and `board_`'s `gameState` are assumed never to diverge at the root.**
  Under UCI the search's `GameInfo` is seeded from `board_.GetGameInfo()`, so they agree by
  construction; in game mode they are fed by two different writers. The D7 assert covers root
  `gameState` for exactly this reason. Verified once the assert runs clean.
- **The bench direction is unmeasured.** Removing a 20-byte copy and a redundant `UpdateBoardInfo` per
  node should be a small nps gain, but "should" is not a measurement. `Run-Bench.ps1` before and
  after; the number gets reported, not predicted.

## Invariants

- Move generation's `epSquare` and `castlingRights` come from the same object that produced the
  position and its Zobrist hash. No second object can disagree, because no second object exists.
- `Compare-SearchEquivalence.ps1 -BaselineRef origin/main` is **identical** — every per-iteration
  `info` line and the `bestmove`, at fixed depth, `Threads=1`. This is the behaviour-preserving claim;
  a single differing line falsifies D3.
- `nodes_searched` and `qnodes_searched` are unchanged, and still sum to the reported `nodes`.
- Legacy agent move ordering is unchanged: `m_infoSeq` still supplies `GetParentMove()`.
- Exactly one channel reports the game outcome to `Game`.
- An aborted frame still mutates nothing. Deleting `add_move_to_seq` removes a write from the move
  loop; the `IsAborted()` guard after each recursive call is unaffected.
- Perft counts are unchanged across the 142,953-position corpus.

## Validation

Engine tier, so `Validate-PrePR.ps1` runs the full local gate. Beyond it:

- **`Compare-SearchEquivalence.ps1 -After <exe> -BaselineRef origin/main`** — identical `info` lines
  and `bestmove`. This is the gate, not a formality: it is what makes D3 a finding rather than an
  opinion.
- **`Run-PerftCheck.ps1`** — the movegen signatures change, and `Perft` is one of the callers. A clean
  sweep is the script's classification, not zero failures.
- **`Run-Bench.ps1`** before and after, same compiler (clang-cl), matched thread counts. Reported as
  measured.
- **Debug-build test run with the D7 assert in place**, before the removal commit.
- **`self-play-validate`** — one AI-vs-AI game, because D5 and D6 change the game-mode path and no
  automated test drives it.
- **`search-reviewer` dispatch** — the diff touches `AIPerplex.cpp` and `ThreadData.h`.

**No Elo match.** The search is behaviour-identical at `Threads=1`, which the equivalence check proves
directly; an Elo match cannot resolve a zero-Elo change and would only add noise. If the bench shows a
nps delta above ~5%, that is worth reporting and the user can decide whether an SPRT is worth buying —
it is not a precondition for merging.

Closes #308 outright: the dangling-reference hazard in `quiescence()` disappears with the vector it
pointed into.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| `Board` is the sole authority for `epSquare`/`castlingRights`/`halfmoveClock`; move generation takes no `GameInfo` | `CLAUDE.md` → Key Source Facts, and a comment on the new `Board` accessors |
| `GetMove` returns `SearchResult`; the outcome is a return value, not a mutated parameter | `CLAUDE.md` → Key Source Facts (replaces the current `GetMove(info, limits)` note), `Docs/Engine-Readme.md:77-78` |
| `m_infoSeq` survives only as the legacy `lastMove` history for `MoveSorter` | comment on `PlayerAiBase::m_infoSeq` |
| Why the two castling paths provably agreed (the unreachable rook gate) | PR body — the code it described is gone, so there is nothing left to comment |
| The fifty-move draw is the game controller's adjudication, not the search's | comment in `Game::Run` where the check now lives |
| Measured bench before/after | `Docs/Changelog.md` and the PR body |
| #292 re-scoped: the per-node copy is gone, `gameInfoHistory_` remains | comment on #292 |

**Approved decisions that changed during implementation:** none yet — fill in before opening the PR.
