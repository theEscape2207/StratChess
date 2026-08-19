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
| 9 | D11 — `Game` test seam + transition tests, and the D10 aggregation test | done |
| 10 | Perft, bench, `search-reviewer` | **next** |
| 11 | Harvest (CLAUDE.md / Docs) + PR | not started |

## Evidence

`Compare-SearchEquivalence -BaselineRef origin/main`: **IDENTICAL, 90 lines, 6 positions, depth 12**,
re-run after steps 3, 4, 5 and 8. Release 7126 assertions / 451 cases; Debug 7122 / 450. clang-format
and clang-tidy Gate clean. Legacy self-play (all three agents) reached checkmate at unchanged game
lengths; a full AIPerplex game terminates on `Game::Run`'s own fifty-move check.

Both new tests were falsified: breaking the D9 precedence guard fails 6 of the 8 `[game]` cases, and
returning the pre-join `result` from `GetMove` fails the D10 case.

## Deviations from the design (for the PR body)

- **`Config` loses its `Game*`.** `pGame_`'s only use was `SetCustomGame`; once that goes, clang's
  `-Wunused-private-field` makes `/WX` fail, so the member and the `explicit Config(Game*)` parameter
  go too. Also closes a latent null-deref — `ConfigTests.cpp` constructs `Config(nullptr)` while
  `ReadFEN` dereferenced `pGame_` unconditionally.
- **`Game::owns_logging_`.** `~Game()` calls `spdlog::drop_all()`, which nulls the default logger for
  every later test in the process — a segfault the first time two seam-built `Game`s run in one
  binary. A `Game` that never set logging up must not tear it down.
- **An extra Debug probe** (`assert_parent_move_matches_board`), added and deleted alongside D7's,
  asserting the legacy agents' parent move already equals `board.last_move()`. The design accepted
  that equivalence as argument only.
- **`fullMoveCount`** is the one field step 5 does not preserve by value: `Game::gameInfo_`'s copy was
  stale, the board's is correct. Nothing read it, and step 6 deleted the field.
- `Perft.cpp` lost the comment *"MoveGenerator needs latest GameInfo, which only board has"* — the
  design's own evidence for D1, no longer describing anything.
- `FiftyMoveRuleTests`' threshold case now asserts the new contract: the search returns
  `STILL_PLAYING` and the clock crosses the limit on the caller's board.

## Next steps

**Step 10** — `Run-PerftCheck.ps1` (~25 min; movegen signatures changed and `Perft` is a caller — a
clean sweep is the script's classification, not zero failures); `Run-Bench.ps1` before/after against
an `origin/main` build, same compiler, `Threads=1`, reported as measured; then dispatch
`search-reviewer` (the diff touches `AIPerplex.cpp` and `ThreadData.h`).

**Step 11** — harvest into `CLAUDE.md` (Key Source Facts: the board is the sole authority; `GetMove`
returns the post-join `SearchResult`), `Docs/Engine-Readme.md:77-78`, `Docs/TestDesign.md` (the new
`Game` seam), `Docs/Changelog.md` (with the measured bench). Then `New-PullRequest.ps1`. The PR body
must list the deviations above, and note that #308 is closed outright and #292 is re-scoped.
