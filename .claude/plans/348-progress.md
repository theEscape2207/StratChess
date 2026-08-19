# #348 Stage 1 — implementation progress

Design: `.claude/plans/gameinfo-board-single-authority.md` (approved; cross-agent reviewed).
This file is the session-to-session handover: what just landed, what was learned, what is next.

## Step order

Ordering is forced by the design: D7's evidence probe precedes D3's deletion, and D8 + the FEN
cleanup precede D4.

| # | Step | State |
|---|---|---|
| 1 | D7 — Debug-only assert that `info_seq[ply]` agrees with `td.board`; run Debug tests | **done** |
| 2 | D2 — `Board` accessors `ep_square()` / `castling_rights()` / `halfmove_clock()` / `last_move()` | **done** |
| 3 | D1 — movegen drops its `const GameInfo&` parameter | **done** |
| 4 | D8 — `Board::DoMove` writes `gameInfo_.lastMove` | **done** |
| 5 | D3 — delete `ThreadData::info_seq` and `PlayerAiBase::m_infoSeq` + helpers; delete both probes | next |
| 6 | FEN cleanup — `Game::SetGameParams` / `SetCustomGame` / `customGame_` out, `Config::ReadFEN` simplified | not started |
| 7 | D4/D10 — `SearchResult` to its own header + `game_state`; `GetMove(const SearchLimits&) -> SearchResult` | not started |
| 8 | D5/D9/D6 — `Game::Run` retained-mover order, fifty-move adjudication, retire `EGameStateChanged` | not started |
| 9 | D11 — `Game` test seam + transition tests | not started |
| 10 | Validation sweep — perft, bench, Debug tests, self-play, `search-reviewer` | not started |
| 11 | Harvest (CLAUDE.md / Docs) + PR | not started |

## What just completed

**Step 1 (D7)** — `ThreadData::assert_info_matches_board(ply)` compares `info_seq[ply]` against
`board.GetGameInfo()` (`epSquare`, `castlingRights`, `halfmoveClock`, plus `gameState` at ply 0);
no-op in Release. Called at the top of `pvs()` and before `quiescence()`'s draw check, so it covers
every node. Deleted together with `info_seq` in step 5.

**Steps 2-3 (D2, D1)** — the four `Board` accessors, and the `const GameInfo&` parameter gone from
`ComputeLegalMoves`, `ComputeCaptures`, `GeneratePawnCaptures` and `AddCastleMoves`. 18 files. The
subagent that did the plumbing also dropped the parameter from `PlayerHuman::IsAnyLegalMoves`, which
is in the design's scope list: it only ever forwarded `info` into `ComputeLegalMoves`, so removing
the argument left an unused parameter and a `/WX` failure.

**Step 4 (D8)** — `Board::DoMove` writes `gameInfo_.lastMove`, next to the en-passant update, with a
comment explaining why the store exists at all (move ordering is its only consumer) and why it needs
no rollback handling. Added alongside it: `PlayerAiBase::assert_parent_move_matches_board(ply)`, a
second temporary probe asserting `GetParentMove(ply) == m_Board.last_move()` at all three legacy
sort sites. It is not in the design — the design accepts the legacy equivalence as "verified by
argument plus one self-play game" — but it is the same discipline as D7 applied to the one claim D8
rests on, and it costs nothing. Deleted with `m_infoSeq` in step 5.

### Evidence

| Probe | Result |
|---|---|
| Debug suite incl. `[slow]`, step 1 | 7082 assertions / 439 cases, pass |
| Debug suite incl. `[slow]`, step 4 | 7082 assertions / 439 cases, pass |
| Debug AIPerplex game-mode self-play (`.exe game`, movetime 800) | 158 moves, 0 assertion failures |
| Debug UCI `Threads=4` — ep square, Kiwipete castling, `halfmoveClock 98`, 16-ply move list | clean |
| Debug AIAgent (type 3) self-play | 129 moves → checkmate, 0 assertion failures |
| Debug ABIterative (type 2) self-play | 130 moves → checkmate, 0 assertion failures |
| Debug AIBasic (type 1) self-play | 140 moves → checkmate, 0 assertion failures |
| `Compare-SearchEquivalence -BaselineRef origin/main`, after step 3 | IDENTICAL, 90 lines, 6 positions, depth 12 |
| `Compare-SearchEquivalence -BaselineRef origin/main`, after step 4 | IDENTICAL, 90 lines, 6 positions, depth 12 |

Those three legacy self-play runs are what turn D8's equivalence table from a reading of the code
into executed evidence, for all three agents rather than the one the design names.

## Key learnings

- `Board` already maintains `epSquare`, `castlingRights`, `halfmoveClock` in `DoMove`/`UndoMove` and
  saves/restores the whole `GameInfo` via `gameInfoHistory_`. It never set `lastMove` (now it does)
  and never sets `DRAW_50_MOVES` — both design premises, confirmed.
- **`DoMove`'s illegal-move rollback already covers `lastMove`.** When a move leaves the own king in
  check, `DoMove` calls `UndoMove(m)`, which restores the whole `gameInfo_` from `gameInfoHistory_`.
- **The FEN path clears `lastMove`.** `SetupFromFEN` → `setup_board` → `clear_board` →
  `gameInfo_.Reset()`, which calls `lastMove.Clear()`. So a FEN load cannot inherit the previous
  game's last move and change legacy move ordering on move 1.
- `AIPerplex::GetMove` seeds `info_seq[0]` from the **caller's** `GameInfo`, not from `m_Board`
  (`AIPerplex.cpp:157-165`, `:228-229` for helper threads) — the one place the two can diverge, which
  is why the D7 probe checks root `gameState`. Game-mode self-play is what exercises it; under UCI
  they agree by construction. The design's stated open assumption is now evidenced.
- **`Config`'s `Game*` goes with `SetCustomGame` (step 6).** `pGame_` has exactly one use in the
  codebase, `pGame_->SetCustomGame(info)` at `Config.cpp:82`. Once that goes the member is unused,
  and clang's `-Wunused-private-field` is in `-Wall`, so `/WX` forces removing it and the
  `explicit Config(Game*)` parameter with it — one step past the design's stated scope. It also
  closes a latent null-deref: `ConfigTests.cpp:68` constructs `Config(nullptr)` while `ReadFEN`
  dereferences `pGame_` unconditionally on its success path.
- **Driving a UCI probe needs a synchronous driver.** Piping a command file into the exe fails
  silently: stdin drains in one go, `go` is asynchronous, and every following `position` is answered
  with `info string position: ignored, a search is in progress`. It then searches *startpos* five
  times and prints five plausible-looking `bestmove` lines. Wait for the `bestmove`/`readyok` marker
  before sending the next command (scratchpad `uci_drive.py`).
- Invocation traps in this session's shell: `"~[slow]"` is refused by the worktree guard (the `~`
  reads as a home-directory escape) — use Catch2's `"exclude:[slow]"`; `Run-Tests.ps1` has no
  `-Config`, so a Debug run means invoking `build/windows-clang-cl-debug/StratChessTests.exe`
  directly, from `StratChessEvolved/` so it finds `game_settings.json`.
- Don't build or commit while `Compare-SearchEquivalence.ps1` is running — it drives the candidate
  exe in this worktree's `build/`, and the pre-commit hook rebuilds it underneath the comparison.

## For the harvest / PR body

- `Perft.cpp` lost the comment *"MoveGenerator needs latest GameInfo, which only board has — so
  ignore passed info"*. That was the design's own evidence for D1 and no longer describes anything,
  so it belongs in the PR body rather than the tree.
- `Config` losing its `Game*` dependency is a deviation from the design's stated scope (see above).
- The extra legacy `assert_parent_move_matches_board` probe, and the three self-play runs it turned
  into evidence, are an addition to D7's plan — worth a line, since the design explicitly flagged
  legacy verification as a known thin spot.

## Next steps

Step 5 — the deletion, and the largest step. Scope, mapped:

- `ThreadData.h`: `info_seq`, `get_last_info`, `store_info_at_ply`, `add_move_to_seq`,
  `add_null_move_to_seq`, `assert_info_matches_board`; `check_draws` takes no `GameInfo` and reads
  `board.halfmove_clock()`; `update_game_state` writes only `board.SetGameState`.
- `PlayerAI.h:81-91,152-232`: `AddMoveToSeq`, `AddNullMoveToSeq`, `StoreInfoAtPly`,
  `InitMoveVariables`'s seeding, `GetLastBoardInfo`, `UpdateGameState`, `CheckGameOver`,
  `checkDraws`, `m_infoSeq`, `assert_parent_move_matches_board`; `GetParentMove(ply)` becomes
  `m_Board.last_move()`.
- `PlayerAiIterBase.h:15-23`, and consumers in `ABIterative.cpp`, `AIAgent.cpp`, `AIBasic.cpp`,
  `PlayerAI.cpp`, `AIPerplex.cpp`.

`AIPerplex::GetMove` still reads `td_.info_seq.at(0).gameState` at `:247` and `:294`; both become
board reads. That is where step 5 meets step 7, so keep them separable.

Re-run after it: full Debug suite, the equivalence gate, and one self-play per legacy agent — the
probes are gone by then, so self-play is checking "still plays", and the equivalence gate is what
carries the behaviour claim.
