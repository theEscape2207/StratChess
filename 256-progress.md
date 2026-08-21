# Issue #256 implementation progress

## Current status

Task 6 is complete on `codex/issue-256-design` in the isolated
`.claude/worktrees/issue-256-design` worktree: Game now owns type-erased players produced by the
config-aware free factory, while `SearchPlayer` owns the concrete search service by value and
supplies Game's live board on every call. `AIPerplex` no longer inherits any player class. The
approved design is
`.claude/plans/aiperplex-production-search-boundary.md`; the executable plan is
`Docs/superpowers/plans/2026-08-21-aiperplex-production-search-boundary.md`.

## Just completed

- Task 7 harness repair: fixed the two custom UCI fixed-depth drivers rather than changing the
  correct immediate-stop production behavior. `Compare-SearchEquivalence.ps1` and `Run-Bench.ps1`
  now leave stdin open after `go`, drain stderr asynchronously while they read stdout line-by-line,
  wait for `bestmove`, then send `quit`, close stdin, and wait for clean shutdown under the existing
  600-second overall ceiling. Timeout, premature-exit, and nonzero-exit errors now include the
  captured stdout and stderr. Both reject a result without a `bestmove` or the requested depth.
- Harness TDD evidence: the new comparator invariant was RED against the candidate with the old
  driver at depth 2: `startpos` returned only `info depth 0 ... nodes 0` plus emergency
  `bestmove a2a4`, so it failed the requested-depth check. GREEN: comparator self-test passed 21
  cases (including depth-0 emergency and missing-bestmove transcripts); same-binary depth-2 compare
  passed all six positions / 30 lines; `Run-Bench` depth 2 returned nonzero completed search rows
  for all eight positions; and the final candidate versus merge-base `54d3e54dbac8` depth-12 gate
  passed all six positions / 90 compared lines. `git diff --check` remained clean.

- Completed Task 6 fix round 1: replaced the two stale `ai.tuning()` examples with compile-valid
  `SearchTuning` values passed through `AIPerplexConfig` before construction. Corrected the service
  ownership wording: it retains no caller Board/reference, but copies each supplied root into owned
  `ThreadData`. Also updated stale AIPerplex `GetMove` comments to `Search` in the implementation
  and shared include guidance.
- Fix validation: `./build.ps1 tests` passed; `[search],[uci]` passed 1,552 assertions in 167 cases;
  the full suite passed 7,272 assertions in 495 cases. Documentation/stale-API scans and
  `git diff --check` were clean. The built executable is intentionally stale after the StdAfx comment
  touch; Task 7 must run `build.ps1 all` before executable-based validation.

- Completed Task 6: audited owned production surfaces after Task 5 and found no remaining
  AIPerplex player/result/observer compatibility state or production implementation downcasts to
  remove. Existing behavior coverage already proves independent returned results across later
  searches, `StartNewGame()` TT/heuristic clearing, legacy aspiration's local score helper, and UCI
  `eval` breakdown consistency, so no artificial RED change was warranted.
- Updated durable architecture, test-design, changelog and contributor guidance for standalone
  `AIPerplex`, the mandatory by-value `SearchPlayer` adapter, deferred `ISearchEngine`, config-aware
  factory, composed `SearchControl`, per-call observers/returned telemetry, Game totals, and direct
  UCI lifecycle. Corrected stale comments that still named the removed Game setup path.
- Task 6 validation: focused search/UCI tests, `./build.ps1 tests`, full suite, stale-surface and
  documentation scans, `git diff --check`, and pre-commit are recorded with exact results in the
  Task 6 report. The concurrent `isready` overlap remains heuristic because it does not require an
  observed `info` line; Task 7 must replace it with deterministic test-only synchronization or prove
  interleaved output.

- Completed Task 5 fix round 1: `AIPerplex` now rejects `EvalTypes::NONE` during concrete
  construction with `AIPerplex requires a SIMPLE or COMPLEX evaluator`, so neither direct callers
  nor `CreatePlayer` can produce a service with a null evaluator. The factory again warns when
  `search_tuning` is supplied to any non-AIPerplex player, matching the diagnostic Game provided
  before Task 5 moved composition.
- Added behavior coverage proving the factory-configured default depth is used when
  `SearchLimits{}` is empty. Restored the reviewed concurrent `isready` line-integrity and exact
  `readyok`-count regression alongside the unchanged no-sleep immediate-stop test; the separate
  overlap test retains its short wait solely to make search-thread `info` output concurrent with
  command-thread replies.
- Fix-round TDD evidence: RED `[factory],[output_integrity],[immediate_stop]` ran 9 cases and failed
  exactly 3 of 61 assertions: direct NONE construction did not throw, factory NONE construction
  did not throw, and the legacy tuning warning was absent. The default-depth and both UCI tests
  already passed. GREEN passed 61 assertions in 9 cases; broader
  `[player],[search_player],[search],[uci]` passed 1580 assertions in 174 cases; the full suite passed 7272 assertions in
  495 cases; and pre-commit passed 7216 assertions in 492 non-slow cases.

- Completed Task 5: added `SearchPlayer final : IPlayer` with a `Board&`, by-value `AIPerplex`,
  immutable description, fixed type string, and inert generic PV event. `GetMove(limits)` calls
  `search_.Search(board_, limits)`, so every move sees the current Game board instead of a retained
  copy. `AIPerplex` is now a standalone non-copyable/non-movable service; its Board constructor,
  inherited `GetMove`, player metadata, result cache, static logging bridge, instance compatibility
  setter, and inherited stop bridge are gone.
- Added the free `CreatePlayer(PlayerConfig, Board&, PlayerCreationOptions)` composition root.
  AIPerplex evaluator/depth/tuning/threads and the instance verbose policy are mapped into
  `AIPerplexConfig` before type erasure; legacy evaluators and lifecycle are configured while the
  concrete `PlayerAiBase` pointer is still available. Both AI paths receive their initial
  `StartNewGame()` before return. Game opts into verbose search logging and subscribes to the
  generic event only after construction. UCI remains direct and unchanged.
- Removed `IPlayer::GetBestScore` and `IPlayer::SetEvalEngine`; the nonvirtual legacy score helper
  remains on `PlayerBase`, and the legacy evaluator setter remains concrete on `PlayerAiBase` for
  the factory. Tactical runners now construct concrete AIPerplex services with requested threads
  and call `Search(board, limits)` directly.
- Task 5 TDD evidence: RED `./build.ps1 tests` failed at the intended missing
  `PlayerFactory.h`/`SearchPlayer.h` boundary and because the removed-from-test
  `GetBestScore` override still left `ScriptedPlayer` abstract. GREEN focused
  `[player],[search_player],[game_loop],[search],[tactical]` passed 294 assertions in 66 cases;
  the full suite passed 7234 assertions in 490 cases; pre-commit passed 7178 assertions in 487
  non-slow cases. A bounded 30-second AIPerplex self-play logged eight completed moves; a bounded
  AIAgent self-play committed two moves without a crash. Production scans find no
  `PlayerBase::Create`, AIPerplex/PlayerAiBase downcast, or stale AIPerplex compatibility API.

- Completed Task 4 fix round 1: UCI's immediate `stop` can no longer be lost between thread launch
  and `SearchControl::ApplyLimits()`. `AIPerplex::PrepareSearch()` arms a small mutex-protected
  launch handshake before UCI schedules the search thread; `Stop()` records a request throughout
  that launch and `Search()` reapplies it after arming limits. Completion clears the handshake, so
  a `stop` that races a finished search cannot poison the next command. Direct `Search` callers
  self-arm the same state, and the synchronous three-argument API is unchanged.
- Replaced the sleep-based UCI stop regression with immediate `go infinite` then `stop`, no startup
  wait. It asserts exactly one bestmove and then proves a following depth-2 `go` completes. RED
  exceeded the test process's five-second bound; GREEN passed 4 assertions in one case. The focused
  immediate-stop/back-to-back run passed 10 assertions in 2 cases; `[uci]` passed 1276 assertions
  in 105 cases, `[search]` 245 in 61, the full suite 7221 in 487, and pre-commit 7165 in 484
  non-slow cases. A bounded 20-run immediate-stop race probe completed with no timeout or failure.

- Completed Task 4: `UciHandler` owns `std::unique_ptr<AIPerplex>` and
  `std::unique_ptr<EvalComplex>` directly. `init_ai()` constructs the configured concrete service
  once; `ucinewgame` keeps that identity, stops/joins first, and clears its per-game state through
  `StartNewGame()`. UCI's Hash and Threads options now target the concrete service directly.
- UCI passes a fresh iteration observer into every `Search(board_, limits, observer)` invocation.
  The stored `SetIterationObserver` state is deleted; final UCI time and node telemetry comes from
  that call's returned `SearchResult`. UCI no longer downcasts either its evaluator or search
  engine, and its direct-search test reads returned telemetry rather than `GetLastResult()`.
- Lifecycle evidence remains end-to-end: the UCI fixture observes the same AIPerplex address
  across repeated `ucinewgame`, while a seeded TT entry is cleared. A back-to-back `go` test
  captures each command separately and proves both begin their own iteration stream at depth 1.
- Task 4 TDD evidence: RED `./build.ps1 tests` failed at the intended boundary because
  `UciHandler::ai_` was still `PlayerAiBase` and lacked concrete `threads_`, `_tt`, and `Search`.
  GREEN rebuilt successfully; `[uci]` passed 1301 assertions in 105 cases, `[search]` passed 245
  assertions in 61 cases, the full suite passed 7249 assertions in 487 cases, and pre-commit
  passed 7191 assertions in 484 non-slow cases. The bounded concurrent UCI `stop`/`isready`
  regression passed 32 assertions in one case.

- Completed Task 3 fix round 1: `AIPerplex` is again the sole owner of its stop latch. `PlayerAiBase::StopSearch()` is virtual, and the AIPerplex override stops owned `SearchControl`; all inherited-control reads were removed from the concrete search internals. A real asynchronous regression stops one direct search through a `PlayerAiBase&`, then proves the next direct fixed-depth `Search` reaches depth 2 rather than inheriting the prior abort.
- Made verbosity policy per AIPerplex instance. The shared logger is now only a sink; construction and Game's temporary cast branch configure the individual engine, so constructing an opposite-policy engine cannot alter an existing engine. The retained static verbosity call is a no-op source-compatibility bridge for deferred UCI/tactical callers, scheduled for Task 4/5 cleanup.
- Strengthened direct observer-lifetime coverage: the first observer is unchanged by the second search, and a third observer-free search changes neither earlier counter. `ThreadData` now documents AIPerplex/SearchControl ownership instead of PlayerAiBase ownership.
- Fix-round TDD evidence: RED `[search][service_api]` produced the intended failures — a direct search after a compatibility stop completed depth 0, and constructing verbose engine B flipped quiet engine A's policy. GREEN `[search]` passed 245 assertions in 61 cases; `[uci],[game_loop]` passed 1312 assertions in 106 cases; the full suite passed 7249 assertions in 487 cases; and pre-commit passed 7193 assertions in 484 non-slow cases. A bounded 30-second AIPerplex self-play completed eight logged moves without a crash or missing move result before timeout.

- Completed Task 3: introduced `AIPerplexConfig`, namespace-level `SearchTuning`, and the
  `Search(const Board&, const SearchLimits&, IterationObserver)` service API. Construction now
  owns the evaluator, `SearchControl`, transposition table sizing, tuning, logging policy, and
  clamped Lazy SMP thread count. `Search` copies only its supplied root into `ThreadData`, takes
  its observer per call, snapshots `threads_`, joins helpers, aggregates both node counters, and
  returns elapsed telemetry after the join.
- The main thread remains authoritative under Lazy SMP; helper results are discarded except for
  their post-join node totals. The exact 1024-entry limit polling and accepted-iteration-only
  observer emission contracts are unchanged.
- Direct tactical/full and fifty-move callers now construct concrete AIPerplex services and use
  returned `SearchResult`s. New `[search][service_api]` coverage proves consecutive roots and
  observers do not leak between calls, and that `AIPerplexConfig` selects the evaluator through
  observable returned search scores.
- Compatibility retained solely for Tasks 4-5: `PlayerAiBase` inheritance, `AIPerplex(Board&,
  unsigned)`, `GetMove(m_Board, limits)`, inherited metadata/evaluator storage, the UCI setter
  observer, `GetLastResult()`, and the narrow inherited stop-latch bridge. Task 4 migrates UCI;
  Task 5 removes the final player/factory surface.
- Task 3 TDD evidence: RED `./build.ps1 tests` failed with the intended missing
  `AIPerplexConfig` and `AIPerplex::Search` symbols. GREEN build succeeded; focused
  `[search],[tactical],[fifty_move]` passed 297 assertions in 68 cases and the full suite passed
  7236 assertions in 485 cases. A bounded 30-second AIPerplex-vs-AIPerplex self-play completed
  seven logged moves without a crash or missing move result before the validation timeout stopped
  the still-running game.

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
- A fixed-depth UCI harness must not batch `quit` (or EOF) behind `go`: `quit` correctly stops and
  joins the running search, including the intentional pending-stop launch window. Waiting for
  `bestmove` is therefore part of the measurement protocol, not a timing workaround.

## Next steps

1. Complete Task 7 final timing and operational-neutrality validation. The repaired equivalence and
   benchmark drivers are ready for the remaining validation work; Task 7 is still incomplete.

## Safe park point

Safe to stop after the Task 7 harness-fix commit. Game owns `unique_ptr<IPlayer>` instances from the free
factory; an AIPerplex game player is `SearchPlayer { Board&, AIPerplex value, const description }`.
The standalone service owns its evaluator, search control, tuning, TT, helper/thread state,
logging policy, and stop handshake. UCI still owns its concrete AIPerplex directly and its tested
pending-stop handshake is unchanged. Legacy players retain `PlayerAiBase` and the nonvirtual
`PlayerBase::GetBestScore` aspiration seed only. Durable docs now match that boundary. Task 7 is the
only remaining work: validate fixed-depth equivalence, timed/node UCI behavior, operational smoke,
bench noise, full gates and self-play without reopening composition, search-tree behavior, UCI
lifecycle, or Game perf ownership. The only outstanding work is Task 7's remaining full operational
validation and final review; do not reintroduce batch `go`/`quit` validation drivers.
