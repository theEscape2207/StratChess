# Issue #256 implementation progress

## Current status

The checkpointed implementation plan is complete on `codex/issue-256-design` in the isolated
`.claude/worktrees/issue-256-design` worktree. The approved design is
`.claude/plans/aiperplex-production-search-boundary.md`; the executable plan is
`Docs/superpowers/plans/2026-08-21-aiperplex-production-search-boundary.md`. Implementation code has
not changed yet.

## Just completed

- Approved the revised production-search boundary design.
- Verified the worktree is a linked worktree rather than the primary checkout.
- Established a fresh baseline with the Windows clang-cl Release build: all 476 test cases pass.
- Completed three read-only delegated maps covering shared search control, AIPerplex decoupling, and
  front-end/factory/statistics migration.
- Converted the approved design into seven buildable, test-first checkpoints. Every checkpoint ends
  by updating this document and committing a safe park point.

## Key learnings

- `SearchPlayer` is required by the unchanged `IPlayer::GetMove(const SearchLimits&)` contract and
  the board-per-search-call design; only `ISearchEngine` is deferred.
- `Config::PlayerConfig` contains evaluator, limits, tuning, and threads, but not game-mode logging
  policy. `PlayerCreationOptions` must carry that front-end policy into the player factory.
- The implementation must preserve UCI's single `AIPerplex` lifetime across `ucinewgame`, the
  existing `threads_` snapshot, and the six-column combined-player `SimplePerfStats.txt` contract.
- Search-related production changes require test-first checkpoints, search-focused review, full
  test validation, and self-play validation before completion.

## Next steps

1. Extract and test the composed `SearchControl` used by both legacy and production search.
2. Return elapsed/node telemetry and move cumulative performance accounting into `Game`.
3. Convert `AIPerplex` into a concrete board-per-call search service with construction-time config
   and a per-call iteration observer.
4. Add the by-value `SearchPlayer`, config-aware player factory, and Game-owned perf accounting.
5. Move UCI to concrete `AIPerplex`/`EvalComplex` ownership while preserving lifecycle and output.
6. Remove obsolete player/result capability surfaces, migrate all construction sites, and update
   durable documentation.
7. Run equivalence, timed-UCI, benchmark, pre-PR, race/self-play, and search-review gates.

## Safe park point

Safe to stop after the planning checkpoint commit. The branch contains design/planning documents
only; no production or test behavior has changed, and Task 1 can start directly from the executable
plan.
