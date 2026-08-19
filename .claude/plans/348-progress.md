# #348 Stage 1 — implementation progress

Design: `.claude/plans/gameinfo-board-single-authority.md`.

| # | Step | State |
|---|---|---|
| 1 | D7 — Debug probe: `info_seq[ply]` agrees with `td.board` | done |
| 2-3 | D2/D1 — `Board` accessors; movegen drops `const GameInfo&` | done |
| 4 | D8 — `Board::DoMove` writes `lastMove` | done |
| 5 | D3 — both `GameInfo` sequences deleted, with both probes | done |
| 6 | FEN cleanup — `SetGameParams`/`SetCustomGame`/`customGame_` out; `Config` loses its `Game*` | done |
| 7 | D4/D10 — `SearchResult` header; `GetMove(const SearchLimits&) -> SearchResult` | done |
| 8 | D5/D9/D6 — `Game::Run` retained-mover order, fifty-move adjudication, `EGameStateChanged` retired | done |
| 9 | D11 — `Game` test seam + transition tests | **next** |
| 10 | Perft, bench, `search-reviewer` | not started |
| 11 | Harvest (CLAUDE.md / Docs) + PR | not started |

## Evidence so far

`Compare-SearchEquivalence -BaselineRef origin/main`: **IDENTICAL, 90 lines, 6 positions, depth 12**
— re-run after steps 3, 4, 5 and 8. Release suite 7099 assertions / 442 cases. Debug suite green.
Legacy self-play (all three agents) played to checkmate at unchanged game lengths. A full AIPerplex
game now terminates on `Game::Run`'s own fifty-move check.

## Deviations from the design (for the PR body)

- **`Config` loses its `Game*`.** `pGame_`'s only use was `SetCustomGame`; once that goes, clang's
  `-Wunused-private-field` makes `/WX` fail, so the member and the `explicit Config(Game*)` parameter
  go too. Also closes a latent null-deref — `ConfigTests.cpp` constructs `Config(nullptr)` while
  `ReadFEN` dereferenced `pGame_` unconditionally.
- **An extra Debug probe** (`assert_parent_move_matches_board`) was added and deleted alongside D7's,
  asserting the legacy agents' parent move already equals `board.last_move()`. The design accepted
  that equivalence as argument only; this made it executed evidence.
- **`fullMoveCount`** is the one field step 5 does not preserve by value: `Game::gameInfo_`'s copy was
  stale, the board's is correct. Nothing read it, and step 6 deleted the field.
- `Perft.cpp` lost the comment *"MoveGenerator needs latest GameInfo, which only board has"* — it was
  the design's own evidence for D1 and no longer describes anything.
- `FiftyMoveRuleTests`' threshold case now asserts the new contract: the search returns
  `STILL_PLAYING` and the clock crosses the limit on the caller's board.

## Next steps

**Step 9 (D11)** — the remaining design requirement. `Game` owns its `Board` and constructs its
players, so it needs a minimal seam: inject scripted `SearchResult`s and observe the resulting
`game_state_` and termination, without a console. Keep it to injecting results — restructuring
ownership is #256, not this. Tests to add: 99→100 transition, root mate, root stalemate,
`HUMAN_EXITED`, and a custom-FEN start with an already-high clock (the D9 precondition).

Then step 10 (`Run-PerftCheck.ps1`, `Run-Bench.ps1` before/after, `search-reviewer` dispatch) and
step 11 (harvest into CLAUDE.md / `Docs/Engine-Readme.md` / `Docs/TestDesign.md` / `Docs/Changelog.md`,
then `New-PullRequest.ps1`).

A `GetLastResult()`-vs-return-value test at `Threads > 1` (D10) is still outstanding — fold it into
step 9.
