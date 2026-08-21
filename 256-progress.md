# Issue #256 implementation progress

## Current status

Task 1 composes shared search control on `codex/issue-256-design` in the isolated
`.claude/worktrees/issue-256-design` worktree. The approved design is
`.claude/plans/aiperplex-production-search-boundary.md`; the executable plan is
`Docs/superpowers/plans/2026-08-21-aiperplex-production-search-boundary.md`.

## Just completed

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

- `SearchPlayer` is required by the unchanged `IPlayer::GetMove(const SearchLimits&)` contract and
  the board-per-search-call design; only `ISearchEngine` is deferred.
- `Config::PlayerConfig` contains evaluator, limits, tuning, and threads, but not game-mode logging
  policy. `PlayerCreationOptions` must carry that front-end policy into the player factory.
- The implementation must preserve UCI's single `AIPerplex` lifetime across `ucinewgame`, the
  existing `threads_` snapshot, and the six-column combined-player `SimplePerfStats.txt` contract.
- Search-related production changes require test-first checkpoints, search-focused review, full
  test validation, and self-play validation before completion.

## Next steps

1. Return elapsed/node telemetry and move cumulative performance accounting into `Game`.
2. Convert `AIPerplex` into a concrete board-per-call search service with construction-time config
   and a per-call iteration observer.
3. Add the by-value `SearchPlayer`, config-aware player factory, and Game-owned perf accounting.
4. Move UCI to concrete `AIPerplex`/`EvalComplex` ownership while preserving lifecycle and output.
5. Remove obsolete player/result capability surfaces, migrate all construction sites, and update
   durable documentation.
6. Run equivalence, timed-UCI, benchmark, pre-PR, race/self-play, and search-review gates.

## Safe park point

Safe to stop after the Task 1 commit. Task 2 can consume elapsed telemetry without reopening ownership
of the timer, abort latch, effective depth, or node budget.
