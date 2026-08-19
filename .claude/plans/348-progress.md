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
| 10 | Perft, bench, `search-reviewer` | perft running; review done |
| 11 | Harvest + PR | harvest done, PR pending |

## Evidence

`Compare-SearchEquivalence -BaselineRef origin/main`: **IDENTICAL, 90 lines, 6 positions, depth 12**,
re-run after steps 3, 4, 5 and 8. `Run-Bench` 4 alternating runs/side: **2,981,112 → 3,071,809 nps
(+3.0%)**, node counts identical. Release 7126 assertions / 451 cases; Debug 7122 / 450. Format and
clang-tidy Gate clean. Legacy self-play unchanged in game length; a full AIPerplex game terminates on
`Game::Run`'s own fifty-move check.

Both new tests were falsified before being trusted (breaking the D9 guard fails 6 of 8 `[game]` cases;
returning the pre-join `result` fails the D10 case).

## search-reviewer outcome

**LGTM, no correctness findings.** Six low-severity notes; four acted on, two carried to the PR body.

| # | Note | Action |
|---|---|---|
| F1 | Two comments described `UpdateBoardInfo`, which now has no production callers | fixed (`AIPerplex.cpp`, `BoardStateTests.cpp`) |
| F2 | The D10 test's `nodes > mainnodes()` can fail legitimately — a helper that never gets scheduled contributes 0 | fixed: asserts the exact sum via new fixture accessors |
| F3 | `Game::Run` spins forever in Release if `DoMove` rejects a move (pre-existing shape) | fixed: logs and breaks |
| F5 | `~Game()` assumed both players exist — true under `Init()`, not under the new seam | fixed: null-guarded |
| F4 | Pre-existing data race: `adjustScoreForGameState` writes `_bestScore` at ply 0 from helper threads | **not fixed** — untouched by this change; flagged in the PR body |
| F6 | `GetParentMove()` returning by value into a `const Move&` parameter | cleared by the reviewer, no dangling |

Also added, from the reviewer's suggested tests: three `[board_state]` cases pinning the `Board`
lifecycle coupling D8 creates — `SetupFromFEN` clears `lastMove`, a rejected `DoMove` leaves it
alone, and `DoNullMove`/`UndoNullMove` disturb neither it nor the clock.

Not taken: pinned-move tests for legacy agent ordering (the reviewer's suggestion 5). The design
already accepts that gap and #307 will retire that code; noted in the PR body.

## Deviations from the design (in the PR body)

`Config` loses its `Game*`; `Game::owns_logging_`; the extra legacy Debug probe; `fullMoveCount` is
the one field not preserved by value. Full reasoning is in the design doc's closing section.

## Next steps

1. Wait for `Run-PerftCheck.ps1` (was at 82%, 47 classified failures — a clean sweep is the script's
   classification, not zero).
2. Rebuild, full Release + Debug suites, format lint. The review fixes are uncommitted.
3. Commit the review round as one commit, then `New-PullRequest.ps1 -BodyFile` with the body drafted
   in the scratchpad (`pr_body.md`).
4. After the PR: it is **awaiting cross-agent review**, not done.
