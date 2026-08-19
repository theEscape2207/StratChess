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
the castling rights, what is the halfmove clock, what was the last move", deletes both shadow
sequences, and replaces the mutable `GameInfo&` out-parameter of `GetMove()` with a returned
`SearchResult`.

## Scope

**This change will:**

- Add narrow `const` accessors to `Board`: `ep_square()`, `castling_rights()`, `halfmove_clock()`,
  `last_move()`.
- Make `Board::DoMove` write `gameInfo_.lastMove` — the field the board has always carried, saved and
  restored, but never set (D8).
- Drop the `const GameInfo&` parameter from `MoveGenerator::ComputeLegalMoves`, `ComputeCaptures`,
  `GeneratePawnCaptures` and `AddCastleMoves`; they read the board they are already given.
- Delete `ThreadData::info_seq` **and** `PlayerAiBase::m_infoSeq`, with their helpers
  (`get_last_info`/`GetLastBoardInfo`, `store_info_at_ply`/`StoreInfoAtPly`,
  `add_move_to_seq`/`AddMoveToSeq`, `add_null_move_to_seq`/`AddNullMoveToSeq`, `InitMoveVariables`'s
  seeding). `check_draws`/`checkDraws` read `board.halfmove_clock()`; `GetParentMove(ply)` reads
  `board.last_move()`; `update_game_state`/`UpdateGameState` write only `board.SetGameState`.
- Move `SearchResult` out of `AIPerplex.h` into its own header and add a `GameStates game_state`
  field.
- Change `IPlayer::GetMove(GameInfo&, const SearchLimits&) -> Move` to
  `GetMove(const SearchLimits&) -> SearchResult`, across `PlayerBase`'s convenience overload,
  `AIPerplex`, `PlayerHuman`, `AIBasic`, `AIAgent` and `ABIterative`. The returned value is the
  post-join, fully aggregated result (D10).
- Drop the `const GameInfo&` parameter from `PlayerHuman::IsAnyLegalMoves`.
- Reduce `Game::gameInfo_` to a `GameStates game_state_`; delete `Game::SetGameParams`,
  `Game::SetCustomGame` and the write-only `customGame_`, and simplify `Config::ReadFEN` to the
  `SetupFromFEN` call it now is.
- Restructure `Game::Run`'s report/commit order around a retained mover and result (D5), and add the
  fifty-move adjudication there under an explicit precedence rule (D9).
- Retire `IPlayer::EGameStateChanged` and `Game::OnGameStateChanged`.
- Add a test seam on `Game` and deterministic transition tests for the outcome paths the change
  re-routes (D11).

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
- Rewrite or retire the legacy agents, or cap `PlayerAiBase::Quiescent` — **#307**. Their move
  ordering must come out unchanged, which is an invariant here, not an improvement target.
- Change the precedence between a fifty-move draw and a checkmate delivered by the same move (D9).

The will-not list is what stops this becoming the #256 rewrite. Every entry has a home issue; none is
being dropped on the floor.

## Decisions

Ordering matters here: **D8 and the FEN cleanup are prerequisites for D4**, and D7's evidence probe
comes before D3's deletion. The dependency is stated in each decision rather than as a separate plan.

### D1: Move generation reads the `Board`, and takes no `GameInfo`

The four entry points lose the parameter entirely rather than keeping it as an optional override.

Rejected: keeping the parameter and asserting it agrees with the board. That preserves the ability to
pass an inconsistent object — the defect — and pays an assert per node for the privilege.

This is close to free. Of the 29 call sites across the four entry points, 21 already pass
`board.GetGameInfo()`; the other 8 are AIPerplex's `info_seq` and the legacy agents' `m_infoSeq`, both
of which this change removes. `Perft.cpp:171` even carries the comment *"MoveGenerator needs latest
GameInfo, which only board has — so ignore passed info"*. `GetAttackBoard` already read
`board.GetGameInfo().epSquare` directly, so the two halves of castle-move generation currently consult
different objects for related state.

### D2: Narrow `const` accessors, not a `const&` `GetGameInfo()`

`ep_square()`, `castling_rights()`, `halfmove_clock()` and `last_move()` are what the consumers need;
handing out a reference to the whole struct would put `gameState` back on the surface that this change
exists to take it off.

Rejected: `const GameInfo& GetGameInfo()`. It fixes #355's write-through-a-temporary trap for every
field in one line, which is genuinely attractive — but it also hands out a reference whose lifetime is
tied to a `Board` that `DoMove`/`UndoMove` mutate under it, which is the shape of the bug #308 reports
one level up. Left to #355 to decide on its own terms.

### D3: Both `GameInfo` sequences are deleted

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

`PlayerAiBase::m_infoSeq` goes too, but only **after** D8 supplies `lastMove` from the board. Its
three readers are `checkDraws` (halfmove clock), `ComputeLegalMoves` (D1) and `GetParentMove`
(`lastMove`, D8). Once all three read the board it holds no information, and deleting it is dead-code
removal rather than a behaviour change. Deleting it *before* D8 would silently change legacy move
ordering — see D8.

### D8: `Board::DoMove` writes `lastMove`; that is what replaces `m_infoSeq`

`GameInfo::lastMove` is the one field the board carries, saves into `gameInfoHistory_` and restores in
`UndoMove`, but never sets. The legacy agents depend on it: `Game::Run` passes `gameInfo_` into
`GetMove`, the previous player's `UpdateBoardInfo` wrote the last move into it, `InitMoveVariables`
copies it into `m_infoSeq[0]`, and `GetParentMove(ply)` feeds it to `MoveSorter::SortMoves`, which
uses it to front-load recaptures on the opponent's target square (`Sort.cpp:47-60`).

So `Game::gameInfo_` is **not** write-only, and collapsing it without a replacement would change
legacy move ordering. Setting the field in `DoMove` costs one 2-byte store per node and no extra undo
state, because the save/restore already covers it.

The replacement values are identical at every reachable ply:

| Ply | `m_infoSeq[ply].lastMove` today | `board.last_move()` after D8 |
|---|---|---|
| 0 | previous game move, via `Game::gameInfo_` | previous game move — `Game::Run` does `DoMove` on the same move the previous player returned |
| > 0 | the move stored by `AddMoveToSeq` | the move `DoMove` just applied |
| null-move ply | would differ | **unreachable**: `PlayerAiBase::AddNullMoveToSeq` has no callers; only AIPerplex uses null moves, and it never reads `lastMove` |

Rejected: an explicit previous-move setter on the player (`SetLastMove()` or similar). It reintroduces
exactly the pre-call ordering contract that `GetMove` is documented not to have — "every
`GetMove(info, limits)` call is self-contained" — and it leaves `lastMove` owned by two objects, which
is the defect this issue is about.

Note that the existing comments on `AddNullMoveToSeq`/`add_null_move_to_seq` claim the stored
`lastMove` is "whatever the parent ply's real move was". That is false today — the board's `lastMove`
is never written, so it is always null. D8 makes the comment true; the comment is corrected either
way.

### D4: The output semantic moves from a mutable parameter to a returned `SearchResult`

`Docs/ArchitectureReview-2026-08.md:97` requires the output semantic to survive, not the carrier, and
warns specifically against keeping the `GameInfo&` merely to preserve behaviour.

Rejected: keeping `GetMove(GameInfo&, ...)` and simply sourcing its contents from the board. Smaller
diff, but it leaves the search writing into a caller-owned struct.

**Prerequisite: D8 and the FEN cleanup.** Once `lastMove` comes from the board, the out-parameter's
remaining traffic is genuinely dead. `UCIHandler` copies a `GameInfo` into its search lambda and
discards the mutated result, taking everything it reports from `GetLastResult()`. Of `Game::gameInfo_`,
only `gameState` is read directly (`IsStillPlaying()`, `PrintStateMessage()`), which collapses it to a
`GameStates`. `SetGameParams` is reached from `Config::ReadFEN` → `SetCustomGame`, but it round-trips
values straight back out of the board (`GameInfo info = board.GetGameInfo();`), so every field it
copies is equal to the board's by construction — and the one field that could have carried independent
information, `lastMove`, is the commented-out line at `Game.cpp:226`. It and `customGame_` (written,
never read) go with it.

`SearchResult` moves to its own header because `IPlayer` cannot include `AIPerplex.h`. `PlayerHuman`
and the legacy agents fill `best_move` and `game_state` and leave the search counters at their
defaults. `GetLastResult()` is kept as-is: the test suite uses it in seven places, and churning that
buys nothing here.

### D10: `GetMove` returns the finalized, post-join result

`iterative_deepening` returns a `SearchResult` whose `nodes_searched` is the accepted-depth main-tree
count (`AIPerplex.cpp:416`). `GetMove` assigns it to `last_result_` (`:241`), then overwrites
`nodes_searched` and `qnodes_searched` with the sums across the main thread and every joined helper
(`:265-266`). `UCIHandler` reads `GetLastResult()` *after* `GetMove` returns, deliberately, and gets
the aggregated numbers.

So the returned value must be `last_result_` as it stands after the join, not the local `result` —
those differ in two ways, not one. `GetMove`'s return value and `GetLastResult()` must be
indistinguishable for the same call, and that has to hold at `Threads > 1`, where the difference is
observable.

### D5: The fifty-move draw is adjudicated by `Game`, with an explicit report/commit order

Today `AIPerplex::GetMove` calls `info.UpdateBoardInfo(bestMove, ...)` on the caller's struct as its
last act, and `UpdateHalfmoveClock` sets `DRAW_50_MOVES` as a side effect — after the state event has
already fired, so the draw reaches `Game` only through the out-parameter. `Game.cpp:262` carries a
comment explaining that the move must therefore be committed before the game-over test, because the
draw is reported for a position the board has not reached.

`SearchResult::game_state` carries only what the search adjudicates at its own root: `WHITE_WON`,
`BLACK_WON`, `DRAW_PAT`, `STILL_PLAYING`. `Game::Run` checks the clock after its own `DoMove`, where
it is a fact about a position that actually exists.

That forces the loop's order, because two things currently depend on running *before* `DoMove`:
`PrintStateMessage()` reports `GetCurrentPlayer().GetBestScore()`, and `GetCurrentPlayer()` keys off
`board_.GetCurrentColor()`, which `DoMove` flips. The loop therefore:

1. captures the mover (`IPlayer&`) and the returned `SearchResult` before committing anything;
2. commits the move (`DoMove`, `ResetSearchDepth`, `AddGameMove`);
3. adjudicates — the returned `game_state`, then the fifty-move check under D9;
4. reports, using the **retained** mover and result, not `GetCurrentPlayer()`;
5. tests for termination.

Simply moving the existing `PrintStateMessage()` call later would report the opponent's score.

Rejected: having `AIPerplex` compute the post-move state so game-mode behaviour stays bit-identical.
It preserves the current output exactly, but keeps adjudication inside the search — against the field
ownership this issue exists to establish — and it would have to synthesise a position the board never
visits.

### D9: Fifty-move guard — precedence and preconditions

The check applies only when **all** of the following hold: a non-null move was returned, `DoMove`
committed it successfully, and the returned `game_state` is `STILL_PLAYING`. Otherwise a position
loaded from a FEN with a high halfmove clock could overwrite a mate, a stalemate or `HUMAN_EXITED`.

**Behaviour when the threshold-crossing move also delivers checkmate is deliberately preserved, not
changed.** Today a root mate returns a null move and takes the early return at `AIPerplex.cpp:275-277`,
never reaching the `UpdateBoardInfo` line, so `DRAW_50_MOVES` cannot overwrite a mate found at the
root. A move that both crosses 100 and mates is only recognised as mate on the *next* `GetMove`, which
never happens because the loop has already terminated on the draw — so the draw wins today, and it
wins after this change. This is arguably wrong under FIDE (mate ends the game; the fifty-move rule is
a claim, not automatic). Preserving it keeps this change behaviour-preserving; changing it is a
separate decision with its own justification.

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

### D11: `Game`-level tests are required, not optional

`Game::Run` is a console loop with no automated coverage, and D4-D6 re-route every outcome path
through it. Rewriting `FiftyMoveRuleTests` to play the returned move only proves that `Board::DoMove`
advances the clock — it says nothing about whether `Game` adjudicates, terminates and reports. Nor can
self-play substitute: it cannot deterministically reach 99→100, and it can never reach `HUMAN_EXITED`.

This change therefore adds a test seam on `Game` — enough to drive the loop with scripted player
results and observe the resulting state and termination, without a console — plus deterministic tests
for: the 99→100 transition, root mate, root stalemate, `HUMAN_EXITED`, and a custom-FEN start whose
halfmove clock is already high (the D9 precondition).

The seam is the cost of this decision and it is real: `Game` currently owns its `Board` and constructs
its players. Keeping the seam minimal — injecting results, not restructuring ownership — is what keeps
this from becoming #256.

## Assumptions I cannot verify from the code

Most of what this design rests on was enumerated rather than assumed — every `GameInfo` parameter,
call site, field read and `GetMove` caller outside `Archived/` was counted. What is left:

- **`Game::gameInfo_.gameState` and `board_`'s `gameState` are assumed never to diverge at the root.**
  Under UCI the search's `GameInfo` is seeded from `board_.GetGameInfo()`, so they agree by
  construction; in game mode they are fed by two different writers. The D7 assert covers root
  `gameState` for exactly this reason. Verified once the assert runs clean.
- **The bench direction is unmeasured.** Removing a 20-byte copy and a redundant `UpdateBoardInfo` per
  node should outweigh D8's added 2-byte store, but "should" is not a measurement. `Run-Bench.ps1`
  before and after; the number gets reported, not predicted.
- **The legacy agents' behaviour is verified by argument plus one self-play game, not by a test
  suite.** D8's equivalence table is a reading of the code; no test asserts legacy move ordering. The
  type-3 AIAgent self-play run is the only end-to-end check, and it demonstrates "still plays", not
  "plays identically". This is a known thin spot, accepted because #307 and #256 will retire this code
  rather than grow it.

## Invariants

- Move generation's `epSquare` and `castlingRights` come from the same object that produced the
  position and its Zobrist hash. No second object can disagree, because no second object exists.
- `Compare-SearchEquivalence.ps1 -BaselineRef origin/main` is **identical** — every per-iteration
  `info` line and the `bestmove`, at fixed depth, `Threads=1`. This is the behaviour-preserving claim;
  a single differing line falsifies D3.
- `GetMove`'s return value and `GetLastResult()` are indistinguishable for the same call, at any
  thread count.
- `nodes_searched` and `qnodes_searched` are unchanged, and still sum to the reported `nodes`.
- Legacy agent move ordering is unchanged: `board.last_move()` equals the `GetParentMove(ply)` it
  replaces at every reachable ply (D8).
- Exactly one channel reports the game outcome to `Game`.
- A fifty-move draw can never overwrite a mate, a stalemate or `HUMAN_EXITED` (D9).
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
  measured; D8 adds a store per node, so a null or slightly negative result is a legitimate outcome.
- **Debug-build test run with the D7 assert in place**, before the removal commit.
- **The D11 `Game` tests** — the 99→100 transition, root mate, root stalemate, `HUMAN_EXITED`, and a
  high-clock custom FEN.
- **A `GetLastResult()`-vs-return-value test at `Threads > 1`** (D10).
- **A custom-FEN regression test** covering the `Config::ReadFEN` cleanup.
- **Self-play twice**: AIPerplex vs AIPerplex (`"type": 6`) for the search path, and AIAgent vs
  AIAgent (`"type": 3`) because `PlayerAiBase`/`PlayerBase` change — the skill file requires the
  second explicitly.
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
| `Board` is the sole authority for `epSquare`/`castlingRights`/`halfmoveClock`/`lastMove`; move generation takes no `GameInfo` | `CLAUDE.md` → Key Source Facts, and a comment on the new `Board` accessors |
| `GetMove` returns `SearchResult`; the outcome is a return value, not a mutated parameter, and it is the post-join aggregate | `CLAUDE.md` → Key Source Facts (replaces the current `GetMove(info, limits)` note), `Docs/Engine-Readme.md:77-78` |
| Why `board.last_move()` is equivalent to the legacy `GetParentMove(ply)` | comment on `Board::DoMove`'s `lastMove` write — it is the only thing explaining why the store is there |
| Why the two castling paths provably agreed (the unreachable rook gate) | PR body — the code it described is gone, so there is nothing left to comment |
| The fifty-move draw is the game controller's adjudication, and its precedence rule | comment in `Game::Run` where the check now lives |
| `Game::Run`'s retained-mover ordering constraint | comment in `Game::Run` — it is non-obvious and a future edit would reintroduce the bug |
| The new `Game` test seam and what it exists to cover | `Docs/TestDesign.md` |
| Measured bench before/after | `Docs/Changelog.md`, and the PR body |
| #292 re-scoped: the per-node copy is gone, `gameInfoHistory_` remains | comment on #292 |
| FIDE divergence on mate-vs-fifty-move precedence, deliberately preserved | PR body; file an issue only if it is ever worth changing |

**Approved decisions that changed during implementation:** none yet — fill in before opening the PR.
The cross-agent review of the first draft corrected its central premise (`Game::gameInfo_` is not
write-only) and added D8-D11; that is recorded here rather than only in the review thread.
