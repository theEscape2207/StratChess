# Changelog

All notable changes to StratChessEvolved are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), adapted for a project without
external releases: sections are dated rather than versioned, and entries keep the level
of implementation detail (files touched, validation method, "why") that's useful for a
solo/AI-assisted dev workflow, rather than the terse end-user-facing style the original
convention assumes. `Deprecated`/`Security` are dropped — they essentially never apply to
engine internals.

This is the permanent historical record migrated from `Docs/Roadmap.md`'s old
`## ✅ Completed Work` section (see PR #105); `Roadmap.md` itself now holds only active
principles, pointing to GitHub Issues for the live backlog.

Newest first.

---

## 2026-07-04 — Roadmap → GitHub Issues migration (PR #105)

### Changed
- Active backlog (all open Roadmap items) migrated to GitHub Issues under a new label
  taxonomy: `category:*` (search/eval/test/elo/infra/refactor/build-tooling),
  `priority:*` (critical/high/medium/longterm), `status:incoming` (new idea, awaiting a
  triage sweep), `type:epic`, `low-hanging-fruit`
- Native GitHub sub-issues used for the Build System Modernization epic (#81 → #82-84,
  #92); native "Blocked by" links used for real dependencies (#83, #84, #92 blocked by
  #82)
- `Docs/Roadmap.md` gutted to principles + this changelog; nothing lost — the two items
  that lived in now-deleted sections (Near-Term Sequence outcome, GetMove SearchLimits
  refactor) were promoted to changelog entries first

## 2026-07-04 — GetMove SearchLimits Refactor (PR #80)

### Changed
- Every `GetMove()` call now takes an explicit `const SearchLimits&`
  (`StratEngine/SearchLimits.h`: clock/movetime/depth/infinite), resolved via a pure
  `Engine::resolve_limits()` function; `PlayerAiBase::ApplyLimits()` replaces
  `StartTimer()` + the `clock_info_set_` flag dance
- `SetClockInfo()` deleted entirely; `UCIHandler::cmd_go` and `Game::SetPlayerParams`
  both build a `SearchLimits` and pass it per call instead of pre-configuring AI state —
  removes the pre-call setter-ordering contract a Lazy SMP helper thread could otherwise
  violate
- `game_settings.json` migrated to a `"search_limits"` block; legacy
  `max_depth`/`time_limit` keys still work via a fallback with a one-time deprecation
  warning
- Delta from the original roadmap sketch (a `GameInfo`-field `TimeControl` struct):
  rejected because `GameInfo` is copied into `info_seq` at every search ply

### Fixed
- `search-reviewer` caught a real gap in the legacy-config fallback (a `max_depth`-only
  config would have silently gotten an unbounded 1-hour search instead of the old 15s
  cap) — fixed before merge

Validated: byte-identical fixed-depth self-play node counts vs. pre-refactor baseline,
AIAgent self-play regression (base classes changed), full `Validate-PrePR.ps1` gate, UCI
smoke tests across all `go` modes. Plan: `.claude/plans/getmove-searchlimits-refactor.md`.
**Unblocks**: Lazy SMP — no remaining refactoring blockers.

## 2026-07 — Near-Term Sequence before Lazy SMP (decided + completed)

### Changed
- Ordering agreed after the issue #66 post-mortem: (1) tactical suite expansion — WAC
  mate-in-2/3/4 + non-mate tactical wins, 8 → 31 gated positions, 100%-mate-category pass
  policy; (2) Extract ThreadData Structure; (3) ELO baseline + deferred-suite scope
  revisit before Lazy SMP
- **Stability mode** (`tactical stability N`) adopted as the pre-SMP correctness
  artifact — runs the gated suite N consecutive times, fails on any per-run gate failure
  or any position flipping pass/fail between runs; gated at N=10 in
  `Validate-PrePR.ps1` Step 3. Chosen because once Lazy SMP threads race on a shared TT,
  byte-identical node-count equivalence stops being available, so a flakiness detector
  was needed before that point
- BT2630/ECM-GCP tactical suite additions deferred until deeper search (SEE/futility
  pruning); endgame tablebase positions scheduled alongside future eval progress work

## 2026-07 — ELO Baseline Measurement

### Added
- `Scripts\Run-EloMatch.ps1`: one-command differential strength measurement — candidate
  build vs. pinned reference (`elo-reference-v1` tag), fastchess v1.8.0, 250 committed
  openings (color-swapped pairs), 10+0.1 TC, adjudication, per-engine working dirs;
  auto-rebuilds the cached reference exe from its tag via a temp worktree on cache miss;
  appends every result to `Docs/EloLog.md`

### Fixed
- Games longer than `MAX_PLY` (256) plies overflowed the ply-indexed history arrays
  during UCI `position` replay (Release access violation) — found on first contact with
  a real match runner. `cmd_position` reset the undo cursor only after the whole replay
  loop; fixed by per-move `ResetSearchDepth()` (matching `Game.cpp`); issue #53
  follow-up; 300-ply regression test via new `UciHandlerTestFixture` (`[uci]`)

Sanity baseline: identical builds (SHA256-verified) pooled −1.4 ELO over 2×500 games — no
instrument bias; measured per-batch noise ±25 ELO at this draw ratio. Plan:
`.claude/plans/elo-baseline-measurement.md`; full setup/interpretation: `Docs/EloLog.md`.

## 2026-07 — Extract ThreadData Structure

### Changed
- All per-search mutable state used by `AIPerplex` now lives in a single `ThreadData`
  struct (`StratEngine/ThreadData.h`): thread-local `Board` copy, node counter,
  `PVTable`, `GameInfo` sequence, killers, history, null-move flags — plus the
  maintenance methods that operate on them
- `ThreadData&` passed explicitly as the **first** parameter through the whole search
  call chain (`iterative_deepening` → `search_with_aspiration` → `pvs` → `quiescence`
  and helpers); `TranspositionTable` stays a separate explicit parameter (shared across
  threads under Lazy SMP)
- The search runs entirely on `td.board`; the only remaining side effect on the real
  board — root game state — is propagated back explicitly after the search returns
- `PlayerAiBase::StopTimerAndAdjustVars(size_t node_count)` takes the node count
  explicitly; legacy AIs (`AIBasic`/`AIAgent`/`ABIterative`) pass `m_SearchCount`

Validated: byte-identical move/score/depth/nodes sequence across a full 137-move
fixed-depth self-play game vs. pre-refactor baseline; deep perft 640/640; all Catch2
tiers + exe tactical suite 31/31. Plan: `.claude/plans/extract-threaddata-structure.md`.
**Unblocks**: Lazy SMP, and the "with ThreadData extraction" C++23 slice (`std::mdspan`).

## 2026-07 — NMP Single-Piece Zugzwang Guard (issue #66)

### Fixed
- QFORK-001 (`8/8/8/3r4/4k3/8/8/3QK3 w`, KQ vs KR) regressed to 7/8 on the exe tactical
  suite when null-move pruning landed: the zugzwang guard only refused NMP for a side
  with *zero* non-pawn material, so a lone-rook side could "pass," hiding the
  domination/zugzwang rook win. `should_try_null_move()` now requires ≥ 2 non-pawn
  pieces for the side to move
- Verification gap closed: the exe tactical suite (never run by any automated gate — how
  the 7/8 went unnoticed) now runs in `Validate-PrePR.ps1` Step 3; QFORK-001 mirrored as
  a Catch2 `[tactical]` case

Plan: `.claude/plans/nmp-single-piece-zugzwang-guard.md`.

## 2026-07 — De-Singleton Board (PR #67)

### Changed
- `Board::Instance()` singleton accessor removed entirely; `Board` is now an ordinary
  constructible, copyable value type
- `MoveGenerator` and `EvalManager::Evaluate` take an explicit `const Board&` parameter
  instead of reaching for the singleton
- `PlayerBase::Create()` and every player constructor take `Board&` by injection
- `Game` and `UciHandler` each own their own `Board board_` member; `Config`'s FEN/board-
  setup methods take an explicit `Board&`
- All Catch2 test files construct their own local `Board` — no more shared global board
  state between `TEST_CASE`s; three dead legacy test headers retired
- `zobrist::initialize()` changed to a thread-safe run-once magic static, so two boards
  built from the same FEN are guaranteed to hash identically
- `GetBitBoards()` changed to a `const` accessor returning `std::span<const BITBOARD>`

Validated: perft 640/640 with identical node counts at every phase; self-play byte-
identical vs. pre-refactor baseline through both `PlayerAI`/`PlayerBase` hierarchies.
Plan: `.claude/plans/de-singleton-board.md` (7 phases). **Unblocks**: Extract ThreadData
Structure, and the "with De-Singleton Board" C++23 item (`std::expected`).

## 2026-06 — Decouple `Board::currentPly_` from Game Length (PR #57, issue #53)

### Fixed
- `currentPly_` indexes four fixed `MAX_PLY=256` ply-history arrays but was never reset
  after a permanently-committed move, so it grew with total game length while search
  recursion added depth on top — overflowed the arrays after ~250 real moves (self-play
  access violation around move 247-249)
- `Board::ResetSearchDepth()` added, called after every permanent move commit, so
  `currentPly_` only ever spans in-flight search recursion depth, never game length
- `assert(currentPly_ < MAX_PLY)` guard added in `DoMove`/`UndoMove` as defense in depth

## 2026-06 — Null-Move Pruning (PR #55)

### Added
- `tuning_.null_move_enabled` defaults to `true`; guard helper `should_try_null_move()`
  centralises every condition: zugzwang (no non-pawn material), mate-score
  contamination, consecutive-null, PV/in-check, min-depth

### Fixed
- `Board::DoNullMove()` wasn't forfeiting en-passant rights (zobrist/EP desync)
- `PlayerAiBase::m_infoSeq` wasn't sized for null-move plies (out-of-bounds crash the
  first time NMP recursed in a real self-play game, not caught by unit tests alone)

Plan: `.claude/plans/null-move-pruning.md`.

## March 2026

Several entries below have no day-level date recorded in the original roadmap; relative
order is preserved as originally listed.

### Added
- **UCI Protocol**: `UCIHandler` class, synchronous command loop with search on
  `std::thread`. Commands: `uci`, `isready`, `ucinewgame`, `position`
  (startpos/fen/moves), `go`, `stop`, `quit`. Time control: `movetime`,
  `wtime`/`btime`/`winc`/`binc`/`movestogo`, `depth`, `infinite`.
  `AIPerplex::GetLastResult()` exposes `SearchResult` for the post-search `info` line.
  Validated via pipe-based functional smoke test.
- **Late Move Reductions** (PR #38): sqrt formula
  `R = min(max(1, sqrt(depth-1) * sqrt(si-1)), depth-1)`, applied to quiet/non-killer/
  non-evasion/non-PV-node moves; kill-switch `tuning_.lmr_enabled`. Observed depth 13-15
  vs 8-9 (without LMR) in the same 15-second budget. 10 `[search]` test cases. Plan:
  `.claude/plans/lmr-and-search-tests.md`.
- **Time Management: Clock-Aware Soft/Hard Limits** (PR #5): `Engine::compute_budget()`
  pure formula (`soft = usable/horizon + inc*80%`, `hard = min(soft*3, usable/2)`);
  `TimeManager` two-arg `start(soft, hard)`; node-based time polling in `pvs()` every
  1,024 nodes. `PlayerAiBase::SetClockInfo()` introduced here — later superseded and
  deleted by the GetMove SearchLimits refactor (2026-07-04). Plan:
  `.claude/plans/time-management-clock-aware.md`.
- **Aspiration Windows in Iterative Deepening** (PR #30): narrow alpha/beta window
  around the previous depth's score, gradual widening (25cp → 75cp → full) on
  fail-high/fail-low. Kill-switch `tuning_.aspiration_enabled`.
- **Killer Moves + History Heuristic**: `killers_[MAX_PLY][MAX_KILLERS]`,
  `history_[2][64][64]` in `AIPerplex`; killers cleared at search start, history aged
  each iteration (halved to prevent overflow).
- **MoveFormatter**: stateless class centralizing move presentation —
  `ToShort`/`ToVerbose`/`ToUCI`/`FromUCI` in `StratEngine/MoveFormatter.h/cpp`. Fixed
  gaps: verbose line restored, `+` now in `gamelist.txt`, fragile `\n`-surgery removed.
  65 assertions, 6 test cases (`[formatter]`). Plan: `.claude/plans/move-formatter.md`.
- **Perft Testing Framework**: `StratEngine/Tests/Perft.h/cpp` + `PerftRunner.cpp`;
  `Tests/perft_test_cases.json`; integrated into main binary
  (`StratChessEvolved.exe perft test`).
- **Phase 0 Test Coverage**: `[tt]`, `[eval]`, `[tactical]` Catch2 tags;
  `STRAT_ENABLE_TEST_ACCESS` friend stub added to `AIPerplex.h`.

### Changed
- **C++20 Adoption — `<bit>` and `<format>`** (PR #32): `std::countr_zero` replaces
  `_tzcnt_u64` `#ifdef` in `Board::GetFirstPiece`; `std::format` replaces
  `std::stringstream` in `Move::Output()`.
- **Move Sorting: Extract ScoreMoves + `[sort]` Tests** (PR #38): inline move scoring
  loop extracted from `pvs()` into `MoveSorter::ScoreMoves()`; 5 test cases, 14
  assertions.
- **spdlog Level Gate + outLegalMoves Removal** (PR #34): 3-line per-call logging
  boilerplate replaced with a spdlog level gate; global `outLegalMoves` stream removed.
  Plan: `.claude/plans/logging-spdlog-gate-and-outlegalmoves-removal.md`.
- **Extract Magic Numbers into SearchTuning**: `barelySearched`/`probablyIncomplete`/
  `pvTooShort` thresholds moved into `SearchTuning`, exposed via `game_settings.json`.
- **Expand PCH Coverage in StdAfx.h**: 9 STL headers added; redundant per-TU includes
  removed from 7 source files.
- **Move class → 16-bit layout** (Phases 1-4): removed `IsCheck`, `[from|to]IsNoSquare`,
  `MovPiece` (→ `Board::GetEffectiveMovPiece()`), and `Content`/captured-piece fields;
  `sizeof(Move) == 2` enforced via `static_assert`.
- **Move sorting: stack-allocated sort buffer**: `pvs()` uses a stack `std::array`
  instead of heap allocation per call.
- **GameInfo History in Board**: history arrays moved from Player into `Board`;
  prerequisite for De-Singleton Board.
- **Migrate to Catch2 v3**: 2-file amalgamated drop-in; test project rebuilt from empty
  placeholder; three legacy test headers retired.
- **TranspositionTable Thread-Safety**: per-bucket `shared_mutex` locks (probes
  `shared_lock`, stores `unique_lock`); global `shared_mutex` for resize/clear; TT only
  cleared on new game.

### Removed
- **Archive Broken Algorithms**: `ABIterTrans.cpp/h` and `AITrans.cpp/h` moved to
  `StratEngine/Archived/`, removed from the build.
- **Remove Dead Code**: commented-out old `Search()` method body removed from
  `AIPerplex.cpp`.

### Fixed
- **Fix `zobrist::initialize()` Never Called**: now called in the `Board` constructor;
  all castling/en-passant/side-to-move Zobrist keys initialised before any `Board`
  method runs.

## 2026-03-01 — Restrict Board Piece-Setup API to Private (PR #21, commit d4b1bd6)

### Changed
- `ClearBoard`, `SetInitialColor`, and `AddPieceToBoard` moved to `private:` — no longer
  called from test code after PR #20; `SetupFromFEN` is the sole public board-setup API

## 2026-02-26 — Delta Pruning in Quiescence

### Added
- `tuning_.delta_pruning_margin = 200` added to `SearchTuning`; guard skips captures
  where `stand_pat + piece_value + margin < alpha` (not in check, not promotion)

Consistently deeper search — mate at depth 14 observed where it wasn't reached before.

## 2026-02-22 — Threefold / Twofold Repetition Correctness

### Fixed
- `push_position()` now called after `ChangePlayer()` in both `DoMove()` branches
- `is_repetition()` loop start corrected (parity fix)
- Twofold-in-search branch was unreachable; fixed dead condition
- Castling rights and en-passant square changes now included in Zobrist hash;
  `zobrist_hash_` widened from `unsigned int` to `uint64_t`

## 2026-02-11 — Search Algorithm Fixes + iterative_deepening Refactor

### Fixed
- Iterative deepening timeout move selection bug
- Score=0 acceptance on interrupted searches
- PV table/move mismatch issue
- Mate emergency infinite loop
- Mate-found not stopping iteration
- False-positive score-swing rejections (Case 5b)

### Changed
- Extracted helper methods: `assess_iteration_quality`, `should_stop_early`,
  `handle_empty_move_emergency`
- Introduced `SearchResult` struct for a clean interface
- Added `SearchTuning` struct for runtime parameters
- Improved log levels (debug/info/critical)
