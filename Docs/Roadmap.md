# StratChess Engine Roadmap

**Last Updated**: July 4, 2026
**Current Version**: AIPerplex 2.0

---

## Overview

The active backlog lives in **GitHub Issues** as of July 2026 (see PR #80) — this file no
longer tracks open work. Use the issue tracker, filtered by:

- **`category:`** labels — `search`, `eval`, `test`, `elo`, `infra`, `refactor`,
  `build-tooling`
- **`priority:`** labels — `critical`, `high`, `medium`, `longterm`
- **`status:incoming`** — a new idea, minimally captured (what + why/potential),
  awaiting a triage sweep before it earns a priority label
- **`type:epic`** — a tracking issue grouping related sub-issues via GitHub's native
  sub-issue links (parent/child, with a progress bar)
- **`low-hanging-fruit`** — small, isolated, no dependencies; good to grab anytime
  without much upfront thought
- GitHub's native **"Blocked by" / "Blocking"** issue links express `#A requires #B`
  relationships directly on the issue, independent of the epic/sub-issue tree

This file retains: general engineering principles (below) and the historical record of
completed work.

---

## Measurement & Success Criteria

### Performance Metrics
- **Nodes per second**: Track baseline, target +20% with optimizations
- **Depth reached**: 15 seconds reaches depth 13-15 (with LMR; was 8-9 before)
- **Win rate**: Maintain or improve vs AIAgent baseline

### Code Quality Metrics
- **Test coverage**: Target 80% for search algorithms
- **Build warnings**: Zero with `/W4` or `-Wall -Wextra`
- **Static analysis**: Zero critical issues (PVS-Studio)

### Regression Prevention
- Save positions from each bug fix as test cases
- Run full test suite before each release
- Track performance on standard benchmark suite

---

## Decision Framework

### When to Implement
Consider this order:
1. **Bug fixes** - Always first
2. **Blocking items** - Required for parallel search
3. **High-impact perf** - LMR, killer moves
4. **Infrastructure** - Testing, profiling (enables confidence)
5. **Advanced features** - NNUE, tablebases (after solid baseline)

### When to Skip
Avoid these traps:
- ❌ Premature optimization (profile first!)
- ❌ Feature creep (parallel search is the north star)
- ❌ Perfect-is-enemy-of-good (working > elegant)
- ❌ Over-engineering (YAGNI principle)

---

## ✅ Completed Work

### Search Algorithm Fixes (Feb 11, 2026)
- Fixed iterative deepening timeout move selection bug
- Fixed score=0 acceptance on interrupted searches
- Fixed PV table/move mismatch issue
- Fixed mate emergency infinite loop
- Fixed mate-found not stopping iteration
- Removed false-positive score-swing rejections (Case 5b)

### Refactor iterative_deepening (Feb 11, 2026)
- Extracted helper methods: `assess_iteration_quality`, `should_stop_early`, `handle_empty_move_emergency`
- Introduced `SearchResult` struct for clean interface
- Added `SearchTuning` struct for runtime parameters
- Improved log levels (debug/info/critical)
- Fixed mate-found iteration continuation bug

### Correctness: Threefold / Twofold Repetition (Feb 22, 2026)
- BUG-1: `push_position()` now called after `ChangePlayer()` in both `DoMove()` branches
- BUG-2: `is_repetition()` loop start corrected (parity fix)
- BUG-3: Twofold-in-search branch was unreachable; fixed dead condition
- BUG-4: Castling rights and en-passant square changes now included in Zobrist hash
- `zobrist_hash_` widened from `unsigned int` to `uint64_t`
- Dead `mark_irreversible()` removed; `updateThreefoldRep` made private and renamed

### Performance: Delta Pruning in Quiescence (Feb 26, 2026)
- `tuning_.delta_pruning_margin = 200` added to `SearchTuning`
- Guard: skip captures where `stand_pat + piece_value + margin < alpha` (not in check, not promotion)
- Consistently deeper search — mate at depth 14 observed where it wasn't reached before
- Verified: unit tests, perft, self-play — no regressions

### Infrastructure: TranspositionTable Thread-Safety
- Per-bucket `shared_mutex` locks: probes use `shared_lock`, stores use `unique_lock`
- Global `shared_mutex` for resize/clear operations
- Atomic counters for O(1) diagnostics
- TT only cleared on new game (preserves TT across moves for 10-15% self-play improvement)

### Infrastructure: Perft Testing Framework
- Implementation: `StratEngine/Tests/Perft.h/cpp`
- Test runner: `StratEngine/Tests/PerftRunner.cpp`
- Test cases: `Tests/perft_test_cases.json` — all passing
- Integrated into main binary: `StratChessEvolved.exe perft test`

### Infrastructure: Migrate to Catch2 v3 (March 2026)
- Catch2 v3 amalgamated (2-file drop-in, no pre-build step required)
- Test project `StratChessTests/StratChessTests.vcxproj` rebuilt from empty placeholder
- Tests migrated: `RepetitionTests.cpp` (TC1-TC7, TC9), `MoveFieldTests.cpp`, `PerftTests.cpp`
- Retired: `TestFramework.h`, `Unittests.h`, `Perft_unittests.h`
- Tags: `[repetition]`, `[moves]`, `[perft]`

### Infrastructure: Phase 0 Test Coverage (March 2026)
- `[tt]` — TranspositionTable unit tests (store/probe, mate normalization, replacement, counters)
- `[eval]` — Evaluation position tests (symmetry, material advantage, doubled pawns, rook bonus)
- `[tactical]` — Fast search regression tests (mate-in-1, hanging piece capture via AIPerplex depth 4)
- `STRAT_ENABLE_TEST_ACCESS` friend stub added to `AIPerplex.h` for future Phase 1 search tests

### Refactoring: Archive Broken Algorithms
- `ABIterTrans.cpp/h` and `AITrans.cpp/h` moved to `StratEngine/Archived/`
- `Archived/README.md` explains historical context
- Both removed from build system

### Refactoring: Restrict Board Piece-Setup API to Private (commit d4b1db6)
- `ClearBoard`, `SetInitialColor`, and `AddPieceToBoard` moved to `private:`
- `SetupFromFEN` is now the sole public API for board configuration
- Motivated by retirement of `MoveGeneratorPromotionTests.h` (PR #20)

### Features: Killer Moves + History Heuristic
- Fully implemented in `AIPerplex`: `killers_[MAX_PLY][MAX_KILLERS]`, `history_[2][64][64]`
- Methods: `clear_killers`, `store_killer`, `clear_history`, `age_history`, `update_history`
- Killers cleared at search start; history aged each iteration (halved to prevent overflow)
- Scoring integrated inline in `pvs()` — relocation to `MoveSorter` is a separate task

### Refactoring: State Management — GameInfo History in Board
- `GameInfo` history moved from Player into `Board`: `capturedHistory_`, `gameInfoHistory_`,
  `irreversiblePlyHistory_`, `zobrist_history_` ply-indexed arrays added directly to `Board.h/cpp`
- Clean separation of concerns; prerequisite data for De-Singleton Board work

### Refactoring: Introduce MoveFormatter — centralise move presentation (March 2026)
- **Stateless class** with four static methods in `StratEngine/MoveFormatter.h/cpp`:
  - `ToShort(Move, Board)` — pseudo-LAN + `+` check annotation (e.g. `"Rc1xc7+"`)
  - `ToVerbose(Move, Board)` — verbose English (e.g. `"White rook captures on c7 and checks!"`)
  - `ToUCI(Move)` — UCI wire format (e.g. `"e2e4"`, `"b7b8q"`) — no board context needed
  - `FromUCI(string_view, Board)` — parse UCI move from board pre-DoMove state
- **Gaps fixed**: verbose line restored; `+` now in `gamelist.txt`; fragile `\n`-surgery
  in `PrintBoardAndMove` removed; promotion-captures now get suffix in perft divide output
- **`Move::Output()` / `Move::Output(ePiece)`**: kept as-is (callers migrated separately —
  see the "Migrate Move::Output() callers" issue)
- **`ToSAN`** (Standard Algebraic Notation): omitted — deferred until PGN export is needed
- **Tests**: `StratChessTests/MoveFormatterTests.cpp` tag `[formatter]` — 65 assertions, 6 test cases
- **Plan**: `.claude/plans/move-formatter.md`

### Move class → 16-bit layout (Phases 1–4, March 2026)
- **Phase 1**: Removed `Move::IsCheck` field
- **Phase 2**: Removed `[from|to]IsNoSquare` fields
- **Phase 3**: Removed `MovPiece` field — `Board::GetEffectiveMovPiece()` added; `MoveFactory`
  drops movPiece param; `MoveHelper`/`GameState`/`Sort` thread explicit movPiece; Zobrist hash
  corruption in `UndoMove` fixed; `BoardTests.cpp` added (6 `[board]` test cases; all 47 tests pass)
- **Phase 4**: Removed `Content` (captured-piece) field — `sizeof(Move) == 2` enforced via
  `static_assert`; 4 `PROMOTION_*_CAPTURE` MoveType variants added (capture bit 2 + promotion bit 3);
  `Board::GetCapturedPiece()` public API added; `MoveFactory` drops captured param;
  `Move::Value` / `MoveHelper::IsValid` / `IsPieceCapturedAt` gain explicit `content` param;
  `IsCapture` / `IsPromote` simplified to pure flag-bit tests; all 47 tests pass

### Move sorting: Stack-allocated sort buffer
- `pvs()` uses `std::array<std::pair<int,int>, MoveList::MAX_MOVES>` on the stack
- Zero heap allocation per call; `thread_local` buffer approach ruled out (extra copy required for recursion safety)

### Performance: Aspiration Windows in Iterative Deepening (PR #30, March 2026)
- Narrow alpha/beta window around previous depth's score; gradual widening (25cp → 75cp → full) on fail-high/fail-low
- Kill-switch: `tuning_.aspiration_enabled` — depth 1 always uses full window regardless
- `search_with_aspiration()` extracted into its own method in `AIPerplex.cpp`
- Verified: no search regressions; self-play shows stable score progression across iterations

### Infrastructure: Expand PCH Coverage in StdAfx.h (March 2026)
- Added 9 STL headers: `<algorithm>`, `<array>`, `<cassert>`, `<cstdint>`, `<functional>`, `<memory>`, `<sstream>`, `<string>`, `<utility>`
- Removed redundant per-TU includes from `AIPerplex.cpp`, `Sort.cpp`, `MoveGenerator.cpp`, `Move.cpp`, `MoveFormatter.cpp`, `PlayerHuman.cpp`, `Utils/FENParser.cpp`
- Verified: full rebuild + all 214 test assertions pass

### Refactoring: Extract Magic Numbers into SearchTuning (March 2026)
- `barelySearched` threshold → `tuning_.min_nodes_threshold = 1000`
- `probablyIncomplete` threshold → `tuning_.min_completion_ratio = 0.10`
- `pvTooShort` threshold → `tuning_.min_pv_ratio = 0.33`
- All three values exposed via `game_settings.json` for runtime tuning

### Refactoring: Remove Dead Code (March 2026)
- Commented-out old `Search()` method body removed from `AIPerplex.cpp`

### Infrastructure: Fix zobrist::initialize() Never Called (March 2026)
- `zobrist::initialize()` now called in `Board` constructor (`Board.cpp` line 50)
- All castling, en-passant, and side-to-move Zobrist keys initialised before any Board method runs
- Stale "never called" comment in `Board.h` removed

### Logging: spdlog Level Gate + outLegalMoves Removal (PR #34, March 2026)
- 3-line per-call logging boilerplate in `AIPerplex` replaced with spdlog level gate (`s_logger->set_level(...)`)
- Global `outLegalMoves` stream removed; board/root-move diagnostics now flow through default spdlog logger at `debug` level
- `Board::test_bitboards` signature simplified
- All four runtime log files documented in `CLAUDE.md`
- Plan: `.claude/plans/logging-spdlog-gate-and-outlegalmoves-removal.md`

### Time Management: Clock-Aware Soft/Hard Limits (PR #5, March 2026)
- `Engine::compute_budget(remaining, increment, moves_to_go)` free function in `TimeUtils.h/cpp`
  — pure math, independently testable; formula: `soft = usable/horizon + inc*80%`, `hard = min(soft*3, usable/2)`
- `TimeManager` gains two-arg `start(soft, hard)` + `should_stop_iteration()` (soft limit check)
- `PlayerAiBase::SetClockInfo()` public method: computes budget and arms timer (superseded —
  see "GetMove SearchLimits Refactor" below; `SetClockInfo()` has since been deleted)
- `AIPerplex::iterative_deepening()` soft-limit gate: stop after depth if `should_stop_iteration()`
  and best move was stable; allow one extra depth if best move just changed (verify the new move)
- Node-based time polling in `pvs()`: check every 1,024 nodes instead of every call
  (amortises `chrono::now()` overhead on deep searches)
- `[time_mgr]` test tag: 10 assertions (6 formula, 4 timing), total < 300 ms
- Plan: `.claude/plans/time-management-clock-aware.md`

### Move Sorting: Extract ScoreMoves + [sort] Tests (PR #38, March 2026)
- Inline move scoring loop extracted from `pvs()` into `MoveSorter::ScoreMoves()` static method
- Precondition asserts + `isKiller1` short-circuit for fast killer detection
- 5 `[sort]` test cases, 14 assertions — full priority order locked in
- Plan: `.claude/plans/move-scoring-extraction-and-sort-tests.md`

### Performance: Late Move Reductions (PR #38, March 2026)
- sqrt formula: `R = min(max(1, sqrt(depth-1) * sqrt(si-1)), depth-1)`
- Applied to quiet, non-killer, non-evasion, non-PV-node moves (si≥3, depth≥3); 2-step re-search
- Kill-switch: `tuning_.lmr_enabled` (parallels `aspiration_enabled`)
- Observed: depth 13-15 vs 8-9 (without LMR) in same 15-second budget; ~31M vs ~36M nodes/move
- 10 `[search]` test cases via `AIPerlexTestFixture` friend class; `STRAT_ENABLE_TEST_ACCESS` gate
- Plan: `.claude/plans/lmr-and-search-tests.md`

### Refactoring: C++20 Adoption — `<bit>` and `<format>` (PR #32, March 2026)
- `std::countr_zero` replaces `_tzcnt_u64` `#ifdef` block in `Board::GetFirstPiece`
- `std::format` replaces `std::stringstream` in `Move::Output()`
- `<bit>` and `<format>` added to `StdAfx.h` PCH
- C++23 upgrade path documented in `.claude/plans/cpp23-upgrade.md`

### Performance: Null-Move Pruning (PR #55, June 2026)
- `tuning_.null_move_enabled` defaults to `true`; guard helper `should_try_null_move()` centralises every condition: zugzwang (no non-pawn material), mate-score contamination, consecutive-null, PV/in-check, min-depth
- Two real bugs fixed along the way: `Board::DoNullMove()` wasn't forfeiting en-passant rights (zobrist/EP desync); `PlayerAiBase::m_infoSeq` wasn't sized for null-move plies (out-of-bounds crash the first time NMP recursed in a real self-play game, not caught by unit tests alone)
- Plan: `.claude/plans/null-move-pruning.md`

### Correctness: Decouple `Board::currentPly_` from Game Length (PR #57, issue #53, June 2026)
- `currentPly_` indexes four fixed `MAX_PLY=256` ply-history arrays but was never reset after a permanently-committed move, so it grew with total game length while search recursion added depth on top — overflowed the arrays after ~250 real moves (self-play access violation around move 247-249)
- `Board::ResetSearchDepth()` added, called after every permanent move commit (`Game::Run`'s real move, `UCIHandler`'s position-moves replay), so `currentPly_` only ever spans in-flight search recursion depth, never game length
- `assert(currentPly_ < MAX_PLY)` guard added in `DoMove`/`UndoMove` as defense in depth
- Regression tests: undo-stack depth doesn't accumulate across 320 simulated real moves; full search-recursion headroom remains after a 260-move game

### Refactoring: De-Singleton Board (PR #67, July 2026)
- `Board::Instance()` singleton accessor removed entirely; `Board` is now an ordinary constructible, copyable value type (public default ctor, `explicit Board(const std::string& fen)` convenience ctor, defaulted copy/move)
- `MoveGenerator` (`ComputeLegalMoves`, `ComputeCaptures`, `GetAttackBoard` + private helpers) and `EvalManager::Evaluate` now take an explicit `const Board&` parameter instead of reaching for the singleton
- `PlayerBase::Create()` and every player constructor (`PlayerAiBase`, `AIPerplex`, `AIBasic`, `AIAgent`, `ABIterative`, `PlayerHuman`) take `Board&` by injection; `PlayerAiBase`'s now-meaningless default constructor removed
- `Game` and `UciHandler` each own their own `Board board_` member (declared before the players/AI that hold a reference into it); `Config`'s FEN/board-setup methods take an explicit `Board&`
- All Catch2 test files construct their own local `Board` — no more shared global board state between `TEST_CASE`s; three dead legacy test headers retired (`Unittests.h`, `RepetitionTests.h`, `Perft_unittests.h`, unused since the March 2026 Catch2 migration)
- `zobrist::initialize()` changed from "runs on every Board construction" to a thread-safe run-once magic static, so two boards built from the same FEN are guaranteed to hash identically — required for a future shared TT across per-thread boards
- `GetBitBoards()` changed to a `const` accessor returning `std::span<const BITBOARD>` (every caller was already read-only), which is what unlocked `const Board&` parameters on `MoveGenerator`/`EvalManager`
- Validated throughout: perft 640/640 with identical node counts at every phase, self-play reproducing byte-identical node counts vs. the pre-refactor baseline (fully deterministic — no behavioral change), self-play through both `PlayerAI`/`PlayerBase`-derived hierarchies (AIPerplex and AIAgent) after the base-class changes in the player-injection phase
- New `[board_instance]` test tag (`BoardInstanceTests.cpp`) covers instance independence, copy-then-diverge semantics, and cross-instance zobrist identity
- Plan: `.claude/plans/de-singleton-board.md` (7 phases, each independently built/tested/committed)
- **Unblocks**: Extract ThreadData Structure, and the "with De-Singleton Board" C++23 item (`std::expected` in `FENParser::ParseFEN`)

### Correctness: NMP Single-Piece Zugzwang Guard (issue #66, July 2026)
- QFORK-001 (`8/8/8/3r4/4k3/8/8/3QK3 w`, KQ vs KR) regressed to 7/8 on the exe tactical suite when null-move pruning landed (PR #55): the zugzwang guard only refused NMP for a side with *zero* non-pawn material, so the side with a lone rook could "pass", hiding the domination/zugzwang rook win
- `should_try_null_move()` now requires ≥ 2 non-pawn pieces for the side to move (`std::popcount`); single-piece endgames (K+R, K+Q, K+minor ± pawns) are exactly the zugzwang-prone class and their subtrees are cheap, so the lost pruning is negligible
- Verification gap closed: the exe tactical suite (never run by any automated gate — how the 7/8 went unnoticed) now runs in `Validate-PrePR.ps1` Step 3, and QFORK-001 is mirrored as a Catch2 `[tactical]` case (pre-commit + CI); new `[search]` unit tests cover the guard branch
- Future NMP enhancement (not needed for this fix): verification search — on a null-move fail-high at high depth, re-search at reduced depth without the null to confirm; generalizes zugzwang safety beyond material heuristics
- Plan: `.claude/plans/nmp-single-piece-zugzwang-guard.md`

### Refactoring: Extract ThreadData Structure (July 2026)
- All per-search mutable state used by `AIPerplex` now lives in a single `ThreadData` struct
  (`StratEngine/ThreadData.h`): thread-local `Board` copy, node counter, `PVTable`, `GameInfo`
  sequence, killers, history, null-move flags — plus the maintenance methods that operate on them
- `ThreadData&` is passed explicitly as the **first** parameter through the whole search call
  chain (`iterative_deepening` → `search_with_aspiration` → `pvs` → `quiescence` and helpers);
  the `TranspositionTable` stays a separate explicit parameter because it remains *shared*
  across threads under Lazy SMP — every call site documents the shared-vs-local split
- The search runs entirely on `td.board` (copy-assigned from the game board per `GetMove()`);
  the only remaining side effect on the real board — root game state (mate/stalemate/draw) —
  is propagated back explicitly after the search returns
- `PlayerAiBase::StopTimerAndAdjustVars(size_t node_count)` takes the node count explicitly;
  legacy AIs (`AIBasic`/`AIAgent`/`ABIterative`) pass `m_SearchCount` and are otherwise untouched
- Time control (`time_manager_`, stop flags), `SearchTuning`, and the TT stay OUT of ThreadData
  by design (control plane / read-only / shared); noted for Lazy SMP: `nodes_since_check_` and
  the static `m_TotalTime`/`m_TotalCount` perf stats will need per-thread treatment
- Validated: byte-identical move/score/depth/nodes sequence across a full 137-move fixed-depth
  self-play game vs. the pre-refactor baseline; deep perft 640/640; all Catch2 tiers +
  exe tactical suite 31/31 (`Validate-PrePR.ps1` full gate)
- Plan: `.claude/plans/extract-threaddata-structure.md` (3 steps, each independently
  built/tested/committed)
- **Unblocks**: Lazy SMP (helper thread = another `ThreadData` + same search functions), and
  the "with ThreadData extraction" C++23 slice (`std::mdspan` history table)

### Infrastructure: ELO Baseline Measurement (July 2026)
- `Scripts\Run-EloMatch.ps1`: one-command differential strength measurement — candidate build
  vs. pinned reference (`elo-reference-v1` tag), fastchess v1.8.0, 250 committed openings
  (color-swapped pairs), 10+0.1 TC, adjudication, per-engine working dirs; auto-rebuilds the
  cached reference exe from its tag via a temp worktree on cache miss; appends every result
  to `Docs/EloLog.md` and exits non-zero on illegal-move/disconnect/stall losses (harness
  failures, never strength data)
- Sanity baseline established: identical builds (SHA256-verified) pooled −1.4 ELO over
  2×500 games — no instrument bias; measured per-batch noise ±25 ELO at this draw ratio
  (both batches individually hit opposite 2σ edges — pool before acting on edge results)
- **The smoke match found a real crash on first contact with a match runner**: games longer
  than MAX_PLY (256) plies overflowed the ply-indexed history arrays during UCI `position`
  replay (Release access violation) — `cmd_position` reset the undo cursor only after the
  whole replay loop. Fixed by per-move `ResetSearchDepth()` (matching `Game.cpp`); issue #53
  follow-up; 300-ply regression test via new `UciHandlerTestFixture` (`[uci]`)
- Plan: `.claude/plans/elo-baseline-measurement.md`; full setup/interpretation: `Docs/EloLog.md`
- Deliberately out of scope: SPRT gating (fastchess supports it — adopt when wanted),
  absolute ELO vs third-party engines, CI integration

### Infrastructure: UCI Protocol (March 2026)
- `UCIHandler` class: synchronous command loop with search on `std::thread`
- Commands: `uci`, `isready`, `ucinewgame`, `position` (startpos + fen + moves), `go`, `stop`, `quit`
- Time control: `movetime`, `wtime`/`btime`/`winc`/`binc`/`movestogo`, `depth`, `infinite`
- `AIPerplex::GetLastResult()` exposes `SearchResult` for post-search `info` line
- `PlayerAiBase::StopSearch()` public API calls `time_manager_.stop()` (thread-safe)
- All spdlog output silenced via `spdlog::set_level(off)` — stdout is UCI protocol only
- Invoked via: `StratChessEvolved.exe uci`; game mode unchanged (no args)
- Validated: pipe-based functional smoke test (uci → isready → position → go movetime → quit)

### Planning: Near-Term Sequence Before Lazy SMP (decided + completed July 2026)
- Ordering agreed after the issue #66 post-mortem: (1) tactical suite expansion — WAC
  mate-in-2/3/4 + non-mate tactical wins, 8 → 31 gated positions, 100%-mate-category pass
  policy; (2) Extract ThreadData Structure (see above); (3) ELO baseline + deferred-suite
  scope revisit before Lazy SMP
- Key decision: **stability mode** (`tactical stability N`) adopted as the pre-SMP
  correctness artifact — runs the gated suite N consecutive times, fails on any per-run
  gate failure or any position flipping pass/fail between runs; gated at N=10 in
  `Validate-PrePR.ps1` Step 3. Chosen because once Lazy SMP threads race on a shared TT,
  byte-identical node-count equivalence (this repo's strongest per-refactor check) stops
  being available, so a flakiness detector was needed before that point
- BT2630/ECM-GCP tactical suite additions deferred until deeper search (SEE/futility
  pruning) — a gate expected to fail on a large fraction of positions at current depth is
  no gate; endgame tablebase positions scheduled alongside future eval progress work as
  its regression suite
- **Outcome**: both 🔴 Critical refactoring blockers (ThreadData, De-Singleton Board) and
  the ELO-baseline pre-work are done

### Refactoring: GetMove SearchLimits Refactor (PR #80, July 2026)
- Every `GetMove()` call now takes an explicit `const SearchLimits&` (`StratEngine/SearchLimits.h`:
  clock/movetime/depth/infinite), resolved via a pure `Engine::resolve_limits()` function;
  `PlayerAiBase::ApplyLimits()` replaces `StartTimer()` + the `clock_info_set_` flag dance
- `SetClockInfo()` deleted entirely; `UCIHandler::cmd_go` and `Game::SetPlayerParams` both
  build a `SearchLimits` and pass it per call instead of pre-configuring AI state — removes
  the pre-call setter-ordering contract a Lazy SMP helper thread could otherwise violate
- `game_settings.json` migrated to a `"search_limits"` block; legacy `max_depth`/`time_limit`
  keys still work via a fallback with a one-time deprecation warning
- Delta from the original roadmap sketch (a `GameInfo`-field `TimeControl` struct): rejected
  because `GameInfo` is copied into `info_seq` at every search ply
- Validated: byte-identical fixed-depth self-play node counts vs. pre-refactor baseline,
  AIAgent self-play regression (base classes changed), full `Validate-PrePR.ps1` gate, UCI
  smoke tests across all `go` modes
- `search-reviewer` caught a real gap in the legacy-config fallback (a `max_depth`-only
  config would have silently gotten an unbounded 1-hour search instead of the old 15s cap)
  — fixed before merge
- Plan: `.claude/plans/getmove-searchlimits-refactor.md`
- **Unblocks**: Lazy SMP — no remaining refactoring blockers

---

**Document Version**: 2.0 — active backlog migrated to GitHub Issues; this file is now
principles + history only
**Owner**: Thees
