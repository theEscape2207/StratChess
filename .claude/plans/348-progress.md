# #348 Stage 1 — implementation progress

Design: `.claude/plans/gameinfo-board-single-authority.md` (approved; cross-agent reviewed).
This file is the session-to-session handover: what just landed, what was learned, what is next.

## Step order

Ordering is forced by the design: D7's evidence probe precedes D3's deletion, and D8 + the FEN
cleanup precede D4.

| # | Step | State |
|---|---|---|
| 1 | D7 — Debug-only assert that `info_seq[ply]` agrees with `td.board` | **done** |
| 2 | D2 — `Board` accessors `ep_square()` / `castling_rights()` / `halfmove_clock()` / `last_move()` | **done** |
| 3 | D1 — movegen drops its `const GameInfo&` parameter | **done** |
| 4 | D8 — `Board::DoMove` writes `gameInfo_.lastMove` | **done** |
| 5 | D3 — both `GameInfo` sequences deleted, with both probes | **done** |
| 6 | FEN cleanup — `Game::SetGameParams` / `SetCustomGame` / `customGame_` out, `Config::ReadFEN` simplified | next |
| 7 | D4/D10 — `SearchResult` to its own header + `game_state`; `GetMove(const SearchLimits&) -> SearchResult` | not started |
| 8 | D5/D9/D6 — `Game::Run` retained-mover order, fifty-move adjudication, retire `EGameStateChanged` | not started |
| 9 | D11 — `Game` test seam + transition tests | not started |
| 10 | Validation sweep — perft, bench, `search-reviewer` | not started |
| 11 | Harvest (CLAUDE.md / Docs) + PR | not started |

## What just completed

**Step 5 (D3)** — `ThreadData::info_seq` and `PlayerAiBase::m_infoSeq` are gone, with
`get_last_info`/`GetLastBoardInfo`, `store_info_at_ply`/`StoreInfoAtPly`,
`add_move_to_seq`/`AddMoveToSeq`, `add_null_move_to_seq`/`AddNullMoveToSeq`, and both temporary
probes. Net −235 lines across 10 files. Specifically:

- `check_draws`/`checkDraws` take only a ply and read `board.halfmove_clock()`.
- `update_game_state`/`UpdateGameState` write only `board.SetGameState` — the only-if-changed guard
  went with the second copy it was guarding.
- `GetParentMove()` lost its ply parameter; it is `m_Board.last_move()`.
- `InitMoveVariables()` lost its `GameInfo` parameter in both the base and the iterative override.
- `CheckGameOver` lost its `fromBoard` flag — only the `true` branch had callers.
- `AIPerplex::init_search()` lost its parameter; `GetMove` reads the root state from the board.
- `SearchTests.cpp`'s four fixture helpers no longer seed a sequence to match the ply under test.

Earlier steps: the four `Board` accessors and the movegen parameter removal (steps 2-3, 18 files,
including `PlayerHuman::IsAnyLegalMoves` which the design's scope list also names); `DoMove` writing
`lastMove` (step 4).

### Evidence

| Probe | Result |
|---|---|
| `Compare-SearchEquivalence -BaselineRef origin/main`, after each of steps 3, 4 and 5 | IDENTICAL, 90 lines, 6 positions, depth 12 |
| Release suite after step 5 | 7087 assertions / 440 cases, pass |
| Debug suite after step 5 | 7082 assertions / 439 cases, pass |
| Debug AIPerplex game-mode self-play, probe active | 158 moves, 0 assertion failures |
| Debug UCI `Threads=4` — ep square, Kiwipete castling, `halfmoveClock 98`, 16-ply move list | clean |
| Debug legacy self-play with the parent-move probe active | AIAgent 130 / ABIterative 130 / AIBasic 140 boards → checkmate, 0 failures |
| Release legacy self-play after the deletion | AIAgent 130 / ABIterative 130 / AIBasic 140 boards → checkmate |

The legacy runs matched their pre-deletion game lengths and results exactly, which is a stronger
statement than "still plays" — it is what the design could only argue for.

The equivalence run is the D3 claim itself: identical `info` lines and `bestmove` at fixed depth,
`Threads=1`. It has now held across every step that could have broken it.

## Key learnings

- `Board` already maintained `epSquare`, `castlingRights`, `halfmoveClock` and saved/restored the
  whole `GameInfo` via `gameInfoHistory_`. It never set `lastMove` (now it does) and never sets
  `DRAW_50_MOVES` — both design premises, confirmed.
- **`DoMove`'s illegal-move rollback already covers `lastMove`**: it calls `UndoMove(m)`, which
  restores the whole `gameInfo_` from `gameInfoHistory_`.
- **The FEN path clears `lastMove`**: `SetupFromFEN` → `setup_board` → `clear_board` →
  `gameInfo_.Reset()`. So a FEN load cannot inherit the previous game's last move.
- `AIPerplex::GetMove` seeded `info_seq[0]` from the **caller's** `GameInfo`, not from `m_Board` —
  the one place the two could diverge, and why the D7 probe checked root `gameState`. Game-mode
  self-play is what exercised it; under UCI they agree by construction. The design's stated open
  assumption is now evidenced.
- **One field the D7 probe did not cover: `fullMoveCount`.** `Game::gameInfo_`'s copy is stale (only
  `SetGameParams` ever wrote it; `UpdateBoardInfo` does not touch it) while the board's increments.
  So `info = m_Board.GetGameInfo()` in `GetMove` now writes a *different* — and correct —
  `fullMoveCount` back to the caller than `info_seq.at(0)` did. Nothing reads it, and step 6 deletes
  the field, but it is the one place this step is not literally value-preserving.
- **`Config`'s `Game*` goes with `SetCustomGame` (step 6).** `pGame_` has exactly one use,
  `pGame_->SetCustomGame(info)` at `Config.cpp:82`. Once that goes the member is unused, and clang's
  `-Wunused-private-field` is in `-Wall`, so `/WX` forces removing it and the `explicit Config(Game*)`
  parameter with it — one step past the design's stated scope. It also closes a latent null-deref:
  `ConfigTests.cpp:68` constructs `Config(nullptr)` while `ReadFEN` dereferences `pGame_`
  unconditionally on its success path.
- **Driving a UCI probe needs a synchronous driver.** Piping a command file into the exe fails
  silently: stdin drains in one go, `go` is asynchronous, and every following `position` is answered
  with `info string position: ignored, a search is in progress`. It then searches *startpos* five
  times and prints five plausible-looking `bestmove` lines. Wait for the `bestmove`/`readyok` marker
  (scratchpad `uci_drive.py`).
- Invocation traps in this session's shell: `"~[slow]"` is refused by the worktree guard (the `~`
  reads as a home-directory escape) — use Catch2's `"exclude:[slow]"`; `Run-Tests.ps1` has no
  `-Config`, so a Debug run means invoking `build/windows-clang-cl-debug/StratChessTests.exe`
  directly, from `StratChessEvolved/` so it finds `game_settings.json`.
- Don't build or commit while `Compare-SearchEquivalence.ps1` is running — it drives the candidate
  exe in this worktree's `build/`, and the pre-commit hook rebuilds it underneath the comparison.
- Release and Debug test counts differ by design (440/7087 vs 439/7082) — something is gated on
  `NDEBUG`. Not a regression; it was true before this branch.

## For the harvest / PR body

- `Perft.cpp` lost the comment *"MoveGenerator needs latest GameInfo, which only board has — so
  ignore passed info"*. That was the design's own evidence for D1 and no longer describes anything.
- `Config` losing its `Game*` dependency is a deviation from the design's stated scope.
- The extra legacy `assert_parent_move_matches_board` probe and its three self-play runs are an
  addition to D7's plan — worth a line, since the design flagged legacy verification as a thin spot,
  and the matching game lengths are the result.
- The `fullMoveCount` note above: the one field where step 5 changes a written value.
- Why the two castling paths provably agreed (the unreachable rook gate) — the code is gone, so the
  design's D3 argument only survives in the PR body.

## Next steps

Step 6, the FEN cleanup, is small and self-contained: delete `Game::SetGameParams`,
`Game::SetCustomGame` and `customGame_`; reduce `Config::ReadFEN` to the `SetupFromFEN` call plus its
fallback; drop `Config`'s `pGame_` and the `Game*` constructor parameter; simplify
`ConfigTests.cpp:68`'s `MakeReader()`. `Game::gameInfo_` cannot collapse to a `GameStates` until
step 7 changes the `GetMove` signature, so leave it a `GameInfo` for now.

Step 7 is the large one. `AIPerplex::GetMove` still ends with the out-parameter write
(`info = m_Board.GetGameInfo()`, the `EGameStateChanged.fire`, and `info.UpdateBoardInfo`), and
`PlayerAiIterBase::GetBestMove(GameInfo&)` does the same for the legacy agents — both become the
returned `SearchResult`.
