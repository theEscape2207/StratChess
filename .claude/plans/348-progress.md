# #348 Stage 1 — implementation progress

Design: `.claude/plans/gameinfo-board-single-authority.md` (approved; cross-agent reviewed).
This file is the session-to-session handover: what just landed, what was learned, what is next.

## Step order

Ordering is forced by the design: D7's evidence probe precedes D3's deletion, and D8 + the FEN
cleanup precede D4.

| # | Step | State |
|---|---|---|
| 1 | D7 — Debug-only assert that `info_seq[ply]` agrees with `td.board`; run Debug tests | **done** |
| 2 | D2 — `Board` accessors `ep_square()` / `castling_rights()` / `halfmove_clock()` / `last_move()` | not started |
| 3 | D1 — movegen drops its `const GameInfo&` parameter (4 entry points, 29 call sites) | not started |
| 4 | D8 — `Board::DoMove` writes `gameInfo_.lastMove` | not started |
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
it is a no-op in Release. Called at the top of `pvs()` and before `quiescence()`'s draw check, so it
covers every node the search visits. It gets deleted together with `info_seq` in step 5.

Evidence it ran clean, all on the **Debug** clang-cl build:

| Probe | Result |
|---|---|
| Full test suite including `[slow]` | 7082 assertions / 439 cases, pass |
| Game-mode self-play to game end (`StratChessEvolved.exe game`, movetime 800) | 158 moves, 0 assertion failures |
| UCI, `Threads=4` — ep square, Kiwipete castling rights, `halfmoveClock 98`, 16-ply move list | 4 searches, no failure |
| UCI, `Threads=1` — pawn endgame | no failure |

## Key learnings

- `Board` already maintains `epSquare`, `castlingRights`, `halfmoveClock` in `DoMove`/`UndoMove`
  (`Board.cpp:412-454`, `:671-673`) and saves/restores the whole `GameInfo` via `gameInfoHistory_`.
  It never writes `lastMove` and never sets `DRAW_50_MOVES` — both design premises, confirmed.
- `AIPerplex::GetMove` seeds `info_seq[0]` from the **caller's** `GameInfo`, not from `m_Board`
  (`AIPerplex.cpp:157-165`, `:228-229` for helper threads). That is the one place the two can
  diverge, which is why the probe checks root `gameState`. Game-mode self-play is the probe that
  exercises it — under UCI the two agree by construction.
- The design's assumption "`Game::gameInfo_.gameState` and the board's never diverge at the root"
  now has evidence: a full game plus the whole Debug suite, clean.
- **Driving the UCI probe needs a synchronous driver.** Piping a command file into the exe fails
  silently: stdin is drained in one go, `go` is asynchronous, and every following `position` is
  answered with `info string position: ignored, a search is in progress` — the engine then searches
  the *startpos* five times and prints five plausible-looking `bestmove` lines. Wait for the
  `bestmove`/`readyok` marker before sending the next command (scratchpad `uci_drive.py`).
- Test-runner invocation traps in this session's shell: `"~[slow]"` is refused by the worktree guard
  (the `~` reads as a home-directory escape) — use Catch2's `"exclude:[slow]"`; and `Run-Tests.ps1`
  has no `-Config`, so a Debug run means invoking `build/windows-clang-cl-debug/StratChessTests.exe`
  directly, from `StratChessEvolved/` so it finds `game_settings.json`.

## Next steps

Step 2 (D2): add the four narrow `const` accessors to `Board`. Then step 3 (D1) — that is the one
with breadth (29 call sites across `ComputeLegalMoves`, `ComputeCaptures`, `GeneratePawnCaptures`,
`AddCastleMoves`), so it is the natural one to hand to a subagent with the call-site list closed
first.
