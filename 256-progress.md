# Issue #256 implementation progress

## Current status

Task 2 is complete on `codex/issue-256-design` in the isolated
`.claude/worktrees/issue-256-design` worktree: completed searches return elapsed/node telemetry,
and `Game` owns the combined-player six-column performance totals. Task 3, establishing the
concrete `AIPerplex` service API, is next. The approved design is
`.claude/plans/aiperplex-production-search-boundary.md`; the executable plan is
`Docs/superpowers/plans/2026-08-21-aiperplex-production-search-boundary.md`.

## Just completed

- Completed Task 2: `SearchResult` now carries each completed search's elapsed time. Legacy AIs
  return their complete `m_SearchCount` in `nodes_searched` and retain zero
  `qnodes_searched`; AIPerplex returns post-join aggregated main/quiescence counts together with
  the composed `SearchControl` elapsed value.
- Moved `SimplePerfStats` accounting into `Game`. Every AI `GetMove()` result is accumulated
  immediately into Game-lifetime totals; each existing six-column row retains the ordering
  `nodes, ms, nodes/ms, total nodes, total ms, total nodes/ms`, uses both players' combined
  totals, and excludes human moves. The 0ms-to-1ms guard applies only to displayed/divisor values,
  not the stored elapsed sum.
- Added focused real-boundary tests for returned elapsed telemetry, legacy unsplit counts, and a
  two-player Game perf row. The Task 2 plan's illustrative assertions named node columns as
  `fields[1]` and `fields[4]`; that conflicts with the preserved six-column contract, so the
  approved correction tests them at indices `0` and `3` and verifies all six fields.
- Task 2 TDD evidence: RED used the configured-preset build command
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build --preset windows-clang-cl --target StratChessTests`,
  which failed before production edits because `SearchResult` had no `elapsed` member in the new
  search and Game-loop tests. GREEN rebuilt with that command, then
  `StratChessTests.exe "[game_loop],[search]"` passed 231 assertions in 58 test cases. The final
  `Validate-PreCommit.ps1` GREEN run passed 7170 assertions in 480 non-slow test cases.

- Completed Task 1: added `SearchControl` to own resolved limits, the timer, abort latch, effective
  depth, and node budget; `PlayerAiBase` now forwards legacy control access to its value member.
- Replaced every legacy `effective_depth_` read with `EffectiveDepth()` and moved AIPerplex's direct
  timer operations behind the composed member. `AIAgent.cpp`, `ABIterative.cpp`, and `AIBasic.cpp`
  were the necessary omitted transitive callers; no Task 2 telemetry work was done.
- Focused tests prove latch reset, exact node-budget latching, the legacy stop guard, and the real
  AIPerplex node-limit path. Existing deterministic 1024-entry overshoot assertions remain unchanged.
- Test evidence: RED `./build.ps1 tests` failed because `SearchControl.h` did not exist; GREEN
  `./build.ps1 tests` built successfully, `StratChessTests.exe "[search_control]"` passed 11
  assertions in 4 test cases, `StratChessTests.exe "[search]"` passed 219 assertions in 55 test
  cases, and the full `StratChessTests.exe` passed 7213 assertions in 479 test cases.
- Follow-up correction: node-budget state remains engaged when the requested limit is zero. RED
  `[search_control]` failed the explicit `nodes = 0` regression under the old sentinel conversion;
  GREEN `[search_control]` passed 13 assertions in 5 test cases, `[search]` passed 219 assertions in
  55 test cases, and the full suite passed 7215 assertions in 480 test cases.

- Approved the revised production-search boundary design.
- Verified the worktree is a linked worktree rather than the primary checkout.
- Established a fresh baseline with the Windows clang-cl Release build: all 476 test cases pass.
- Completed three read-only delegated maps covering shared search control, AIPerplex decoupling, and
  front-end/factory/statistics migration.
- Converted the approved design into seven buildable, test-first checkpoints. Every checkpoint ends
  by updating this document and committing a safe park point.

## Key learnings

- `PlayerAiBase` retains configured default depth/time values so either setter can pass both to
  `SearchControl::SetDefaults`; the per-call resolved state exists only in `SearchControl`.
- Existing fixture helpers armed `TimeManager` directly. They now apply a fixed limit through the
  composed legacy interface, preserving the real poll path under test.
- `SearchLimits::nodes` uses engagement to distinguish no node limit from an explicit zero-node
  limit; `SearchControl` must retain that distinction rather than normalize zero to unlimited.

- `SearchPlayer` is required by the unchanged `IPlayer::GetMove(const SearchLimits&)` contract and
  the board-per-search-call design; only `ISearchEngine` is deferred.
- `Config::PlayerConfig` contains evaluator, limits, tuning, and threads, but not game-mode logging
  policy. `PlayerCreationOptions` must carry that front-end policy into the player factory.
- The implementation must preserve UCI's single `AIPerplex` lifetime across `ucinewgame`, the
  existing `threads_` snapshot, and the six-column combined-player `SimplePerfStats.txt` contract.
- Search-related production changes require test-first checkpoints, search-focused review, full
  test validation, and self-play validation before completion.

## Next steps

1. Establish the concrete `AIPerplex` service API.
2. Give UCI concrete ownership and per-call observation.
3. Add `SearchPlayer` and the config-aware player factory.
4. Remove stale surfaces and update durable documentation.
5. Prove behavior, timing, and operational neutrality.

## Safe park point

Safe to stop after the Task 2 commit. Search telemetry and combined-player performance totals are
owned by `SearchResult` and `Game`; Task 3 can establish the concrete AIPerplex service API without
reopening timer, abort-latch, effective-depth, node-budget, or Game perf-accounting ownership.
