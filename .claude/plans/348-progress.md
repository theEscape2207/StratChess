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
| 4 | D8 — `Board::DoMove` writes `gameInfo_.lastMove` | next |
| 5 | D3 — delete `ThreadData::info_seq` and `PlayerAiBase::m_infoSeq` + helpers; delete the D7 assert | not started |
| 6 | FEN cleanup — `Game::SetGameParams` / `SetCustomGame` / `customGame_` out, `Config::ReadFEN` simplified | not started |
| 7 | D4/D10 — `SearchResult` to its own header + `game_state`; `GetMove(const SearchLimits&) -> SearchResult` | not started |
| 8 | D5/D9/D6 — `Game::Run` retained-mover order, fifty-move adjudication, retire `EGameStateChanged` | not started |
| 9 | D11 — `Game` test seam + transition tests | not started |
| 10 | Validation sweep — equivalence, perft, bench, Debug tests, 2× self-play, `search-reviewer` | not started |
| 11 | Harvest (CLAUDE.md / Docs) + PR | not started |

## What just completed

**Step 1 (D7)** — `ThreadData::assert_info_matches_board(ply)` compares `info_seq[ply]` against
`board.GetGameInfo()` (`epSquare`, `castlingRights`, `halfmoveClock`, plus `gameState` at ply 0);
no-op in Release. Called at the top of `pvs()` and before `quiescence()`'s draw check, so it covers
every node. It gets deleted together with `info_seq` in step 5.

| Probe (Debug, clang-cl) | Result |
|---|---|
| Full test suite including `[slow]` | 7082 assertions / 439 cases, pass |
| Game-mode self-play to game end (`.exe game`, movetime 800) | 158 moves, 0 assertion failures |
| UCI `Threads=4` — ep square, Kiwipete castling, `halfmoveClock 98`, 16-ply move list | clean |
| UCI `Threads=1` — pawn endgame | clean |

**Steps 2-3 (D2, D1)** — the four `Board` accessors, and the `const GameInfo&` parameter gone from
`ComputeLegalMoves`, `ComputeCaptures`, `GeneratePawnCaptures` and `AddCastleMoves`. 18 files. The
subagent that did the plumbing also dropped the parameter from `PlayerHuman::IsAnyLegalMoves`, which
is in the design's scope list: it only ever forwarded `info` into `ComputeLegalMoves`, so the
argument's removal left an unused parameter and a `/WX` failure.

Verified independently of the subagent's report: Release suite 7031 assertions / 437 cases pass, and

```
Compare-SearchEquivalence.ps1 -BaselineRef origin/main
  IDENTICAL across 90 compared lines, 6 positions at depth 12.
```

That is the design's central invariant, holding at the point where D1 could have broken it.

## Key learnings

- `Board` already maintains `epSquare`, `castlingRights`, `halfmoveClock` in `DoMove`/`UndoMove`
  (`Board.cpp:412-454`, `:671-673`) and saves/restores the whole `GameInfo` via `gameInfoHistory_`.
  It never writes `lastMove` and never sets `DRAW_50_MOVES` — both design premises, confirmed.
- `AIPerplex::GetMove` seeds `info_seq[0]` from the **caller's** `GameInfo`, not from `m_Board`
  (`AIPerplex.cpp:157-165`, `:228-229` for helper threads). That is the one place the two can
  diverge, which is why the probe checks root `gameState`. Game-mode self-play is what exercises it —
  under UCI the two agree by construction. The design's stated open assumption is now evidenced.
- **`DoMove`'s illegal-move rollback already covers `lastMove`.** When a move leaves the own king in
  check, `DoMove` calls `UndoMove(m)`, which restores the whole `gameInfo_` from `gameInfoHistory_`.
  So D8's write needs no rollback handling of its own — worth saying in the comment, since it is the
  non-obvious half.
- **`Config`'s `Game*` goes with `SetCustomGame` (step 6).** `pGame_` has exactly one use in the
  codebase, the `pGame_->SetCustomGame(info)` at `Config.cpp:82`. Once that goes the member is
  unused, and clang's `-Wunused-private-field` is in `-Wall`, so `/WX` forces removing it and the
  `explicit Config(Game*)` parameter with it. This is one step past what the design's scope list
  says; record it as a deviation in the PR body. It also closes a latent null-deref —
  `ConfigTests.cpp:68` already constructs `Config(nullptr)` while `ReadFEN` dereferences `pGame_`
  unconditionally on its success path.
- **Driving a UCI probe needs a synchronous driver.** Piping a command file into the exe fails
  silently: stdin drains in one go, `go` is asynchronous, and every following `position` is answered
  with `info string position: ignored, a search is in progress`. It then searches *startpos* five
  times and prints five plausible-looking `bestmove` lines. Wait for the `bestmove`/`readyok` marker
  before sending the next command (scratchpad `uci_drive.py`).
- Invocation traps in this session's shell: `"~[slow]"` is refused by the worktree guard (the `~`
  reads as a home-directory escape) — use Catch2's `"exclude:[slow]"`; and `Run-Tests.ps1` has no
  `-Config`, so a Debug run means invoking `build/windows-clang-cl-debug/StratChessTests.exe`
  directly, from `StratChessEvolved/` so it finds `game_settings.json`.
- Don't build or commit while `Compare-SearchEquivalence.ps1` is running — it drives the candidate
  exe in this worktree's `build/`, and the pre-commit hook rebuilds it underneath the comparison.

## For the harvest / PR body

- `Perft.cpp` lost the comment *"MoveGenerator needs latest GameInfo, which only board has — so
  ignore passed info"*. That comment was the design's own evidence for D1 and no longer describes
  anything, so it belongs in the PR body rather than the tree.
- `Config` losing its `Game*` dependency is a deviation from the design's stated scope (see above).

## Next steps

Step 4 (D8): `Board::DoMove` writes `gameInfo_.lastMove`. Check that the `SetupFromFEN` path clears
it — `Board.cpp:175-184` sets `epSquare`/`castlingRights`/`halfmoveClock`/`fullMoveCount` but not
`lastMove`, so confirm `gameInfo_.Reset()` runs first, otherwise a FEN load inherits the previous
game's last move and legacy move ordering changes on move 1.

Then step 5, the deletion. Its scope, mapped: `PlayerAI.h:81-91,152-232` (`AddMoveToSeq`,
`AddNullMoveToSeq`, `StoreInfoAtPly`, `InitMoveVariables`, `GetParentMove`, `GetLastBoardInfo`,
`UpdateGameState`, `CheckGameOver`, `checkDraws`, `m_infoSeq`), `PlayerAiIterBase.h:15-23`, and the
consumers in `ABIterative.cpp`, `AIAgent.cpp`, `AIBasic.cpp`, `PlayerAI.cpp`, plus `ThreadData.h`
and `AIPerplex.cpp`.
