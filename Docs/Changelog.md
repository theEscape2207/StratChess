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

Dates are PR merge dates, verified against `gh pr list`/`gh pr view` — the original
Roadmap.md's dates and PR citations had several errors (wrong PR numbers, entries filed
under the wrong month) that surfaced during this migration; see the entries themselves
for what was corrected. A handful of entries have no PR reference in the original text
and couldn't be matched with confidence — those remain in the undated pocket below.

Newest first.

---

## 2026-07-26 — ELO Match Resume Support (issue #119)

### Added
- `-ResumeDir`/`-AutosaveInterval` parameters on `Scripts\Run-EloMatch.ps1`, letting an
  interrupted match resume from fastchess's own autosaved checkpoint (`config.json`) instead of
  restarting the full batch — discovered as a prerequisite during #70's validation, where a
  500-game match was killed by a background-task duration cap at 491/500 games after ~60 minutes
- Fixed `Docs/EloLog.md`'s recorded game count to come from fastchess's own final `Games: N`
  summary rather than the (in resume mode, meaningless) `-Games` parameter
- Validated: a real kill-and-resume test (interrupted after 2/20 games, resumed via `-ResumeDir`)
  correctly continued from game 3 with no replay/duplication, finishing at exactly 20/20

## 2026-07-26 — Mop-Up Evaluation for Won Pawnless Endgames (issue #70)

### Added
- Mop-up evaluation term in `EvalComplex::Evaluate` giving the engine a gradient toward
  converting decisively-won pawnless endgames (e.g. KQ vs KR), driving the losing king
  toward a corner once material lead clears `MOPUP_MATERIAL_THRESHOLD`; `Tactical -
  QFORK-001` (issue #66 regression test) hidden via Catch2 `[.]` tag pending a broader
  WAC-style tactical suite — see issue #118
- Validated: build clean (Level4/`/WX`, 0 warnings); full extended suite 174/174 passing
  (`Tactical - QFORK-001` intentionally hidden, see above; exe tactical suite now 30/31,
  see `Docs/TestDesign.md`); 1 self-play game to checkmate (move 217), no crash; ELO
  +15.94 ± 27.62 over 491/500 games (partial batch, see `Docs/EloLog.md`)

## 2026-07-23 — Lazy SMP Parallel Search (PR #109)

### Added
- Multi-threaded search for `AIPerplex`: `GetMove()` at `threads_ > 1` spawns
  `threads_ - 1` helper `std::jthread`s (`AIPerplex::helper_loop`) that run a plain
  iterative-deepening loop from depth 1, sharing the transposition table with the main
  search thread; main thread's result stays authoritative (helpers never report moves,
  no depth-skip patterns, no voting) — best move propagates through TT timing alone
- `PlayerAiBase::SetThreads(unsigned)` (virtual no-op on legacy AIs); `AIPerplex::threads_`
  clamped to `[1, 32]`; UCI `setoption name Threads value N` (advertised via
  `option name Threads type spin default 1 min 1 max 32`); `game_settings.json` per-player
  `"threads"` key (default 1, unchanged by this PR — flipping it to the measured-best
  value for actual play is a post-merge, user-decided follow-up)
- `nodes_since_check_` moved from `PlayerAiBase` into `ThreadData`, gated to
  `thread_id == 0` for clock checks; helpers rely solely on the existing `IsAborted()`
  relaxed-atomic check (no clock calls off the main thread)
- `tactical stability N [file] [threads]` CLI form (`StratChessEvolved.exe`) — forwards a
  threads arg through `TacticalTestRunner::run_stability_suite` → `run_test_suite` →
  `run_position`; the pre-merge nondeterminism detector for once threads race on a shared
  TT (byte-identical node-count equivalence stops being available past `threads=1`)
- `Scripts\Run-EloMatch.ps1`: `-CandidateOptions`/`-ReferenceOptions` (arbitrary fastchess
  per-engine UCI option tokens, e.g. `option.Threads=4`) and `-ReferenceExe` (point the
  reference side at an explicit exe instead of always rebuilding from a pinned git tag) —
  needed to measure the same binary against itself under different `Threads` settings

### Fixed
- UCI `Threads` option no longer resets on `ucinewgame`: `UciHandler::cmd_ucinewgame()`
  rebuilds `ai_` from scratch via `init_ai()`, which previously left a fresh `AIPerplex`
  defaulted to `threads_ == 1` — silently discarding any prior `setoption name Threads
  value N` under standard UCI usage (`setoption` once at session start, `ucinewgame` before
  every game), making `Threads` effectively non-functional. Cost real time during this PR's
  own NPS measurement (below) before a probe comparing total nodes at threads=1 vs threads=4
  under a fixed `movetime` caught it. Fixed via `UciHandler::configured_threads_`, set by
  `cmd_setoption()` and reapplied by `init_ai()` on every call (initial `run()` startup and
  every `ucinewgame`); regression-tested in `UCITests.cpp` (`[uci][smp]`).

### Three-gate validation
- **Gate 1 (inert at threads=1)**: byte-identical node/move/score/depth sequence vs the
  pre-SMP baseline — the single-threaded path never touches any thread machinery
- **Gate 2 (stable at threads=4)**: `tactical stability 20 tactical_test_cases.json 4` —
  20/20 runs, 31/31 positions each run, 0 failing runs, 0 flipped positions;
  `Validate-PrePR.ps1` full pass (extended Catch2 tiers, deep perft 640/640, AIAgent
  self-play unaffected — legacy AIs stay single-threaded)
- **Gate 3 (measured gain)**: `Run-EloMatch.ps1`, same binary, `option.Threads=4` vs
  `option.Threads=1`, 500 games @ 10+0.1: **Elo +128.55 ± 28.36, LOS 100.00%**
  (286W/109L/105D, 67.70%) — comfortably clears the positive-score/LOS>95% merge bar.
  Recorded in `Docs/EloLog.md`'s history table.

### NPS scaling (31-position tactical suite, fixed depth 8, driven directly over UCI)

| Threads | Total nodes | Total time (ms) | Aggregate NPS | Scaling vs 1T |
|---|---|---|---|---|
| 1 | 1,534,954 | 1,281 | 1,198,247 | 1.00x |
| 2 | 2,702,646 | 1,122 | 2,408,775 | 2.01x |
| 4 | 5,224,431 | 1,186 | 4,405,085 | 3.68x |
| 8 | 8,901,291 | 1,283 | 6,937,873 | 5.79x |

Depth 8 finishes fast per position (25–105 ms at threads=1), and `go depth N` is
depth-limited rather than time-limited, so total wall time stays roughly flat across
thread counts while combined node throughput scales — more threads do more work in the
same wall time rather than finishing sooner. That is a real but secondary signal; Gate 3's
ELO match is what actually measures the production benefit (better move quality per unit
of *game clock* time). Sub-linear scaling past 2 threads matches Lazy SMP expectations
(shared-TT search overlap between helpers, diminishing marginal value per added thread,
plus physical core count capping it).

Plan: `.claude/plans/lazy-smp.md`. `Docs/TestDesign.md` documents the `tactical stability`
threads arg.

### Follow-ups (deferred; not filed as GitHub issues yet — pending triage)
- **Lockless TT (Hyatt XOR)**: replace the TT's per-bucket `shared_mutex` with
  self-validating atomic entries; own PR, gated on measured NPS scaling + a
  non-regression ELO match. v1 deliberately keeps per-bucket locks — already thread-safe,
  and the pre-SMP single-threaded search already paid the lock cost, so it's a
  zero-regression starting point
- **Persistent thread pool**: current design is spawn-per-search (`GetMove()` spawns
  `threads_ - 1` helper `std::jthread`s and joins them before returning); only worth
  revisiting if profiling ever shows thread-spawn cost mattering against seconds-per-move
  search budgets
- **`game_settings.json` `"threads"` flip**: post-merge, user-decided — flip both
  players' `"threads"` from 1 to the measured-best value (4, per this measurement) to
  actually play with Lazy SMP enabled

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

## 2026-07-03 — ELO Baseline Measurement (PR #75)

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

## 2026-07-03 — Extract ThreadData Structure (PR #74)

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

## 2026-07-02 — Near-Term Sequence before Lazy SMP (PR #71, #72)

### Changed
- Ordering agreed after the issue #66 post-mortem (PR #71): (1) tactical suite
  expansion; (2) Extract ThreadData Structure; (3) ELO baseline + deferred-suite scope
  revisit before Lazy SMP
- Step 1 implemented same day (PR #72): WAC mate-in-2/3/4 + non-mate tactical wins,
  8 → 31 gated positions, 100%-mate-category pass policy
- **Stability mode** (`tactical stability N`) adopted as the pre-SMP correctness
  artifact — runs the gated suite N consecutive times, fails on any per-run gate failure
  or any position flipping pass/fail between runs; gated at N=10 in
  `Validate-PrePR.ps1` Step 3. Chosen because once Lazy SMP threads race on a shared TT,
  byte-identical node-count equivalence stops being available, so a flakiness detector
  was needed before that point
- BT2630/ECM-GCP tactical suite additions deferred until deeper search (SEE/futility
  pruning); endgame tablebase positions scheduled alongside future eval progress work

## 2026-07-02 — NMP Single-Piece Zugzwang Guard (PR #69, issue #66)

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

## 2026-07-02 — De-Singleton Board (PR #67)

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

## 2026-06-20 — Decouple `Board::currentPly_` from Game Length (PR #57, issue #53)

### Fixed
- `currentPly_` indexes four fixed `MAX_PLY=256` ply-history arrays but was never reset
  after a permanently-committed move, so it grew with total game length while search
  recursion added depth on top — overflowed the arrays after ~250 real moves (self-play
  access violation around move 247-249)
- `Board::ResetSearchDepth()` added, called after every permanent move commit, so
  `currentPly_` only ever spans in-flight search recursion depth, never game length
- `assert(currentPly_ < MAX_PLY)` guard added in `DoMove`/`UndoMove` as defense in depth

## 2026-06-20 — Null-Move Pruning (PR #55)

### Added
- `tuning_.null_move_enabled` defaults to `true`; guard helper `should_try_null_move()`
  centralises every condition: zugzwang (no non-pawn material), mate-score
  contamination, consecutive-null, PV/in-check, min-depth

### Fixed
- `Board::DoNullMove()` wasn't forfeiting en-passant rights (zobrist/EP desync)
- `PlayerAiBase::m_infoSeq` wasn't sized for null-move plies (out-of-bounds crash the
  first time NMP recursed in a real self-play game, not caught by unit tests alone)

Plan: `.claude/plans/null-move-pruning.md`.

## 2026-03-14 — UCI Protocol (PR #42)

### Added
- `UCIHandler` class: synchronous command loop with search on `std::thread`. Commands:
  `uci`, `isready`, `ucinewgame`, `position` (startpos/fen/moves), `go`, `stop`, `quit`.
  Time control: `movetime`, `wtime`/`btime`/`winc`/`binc`/`movestogo`, `depth`,
  `infinite`. `AIPerplex::GetLastResult()` exposes `SearchResult` for the post-search
  `info` line. `UCIHandler` moved to `StratEngine/` per `CLAUDE.md` structure.
- 8 `parse_go` `[uci]` test cases (324 total assertions, all pass)

### Fixed
- `movetime 5000` was completing in 11s+ instead of ~5.3s — three root causes: hard
  limit was 3× soft (reduced to 1.5×, so opening moves no longer consume the full hard
  budget); no fast-exit path on timeout (`IsAborted()` latch + check added to `pvs()`
  and `quiescence()`, collapsing the stack in O(depth) instead of O(tree_size)); the
  "move changed" soft-limit extension had no cap (new `extra_depth_used` flag limits it
  to exactly one extra depth per search)
- `std::cout` fallback removed from `StopTimerAndAdjustVars()`; `SetVerboseLogging(true)`
  removed from the `AIPerplex` constructor (verbose logging is now opt-in per call site)

Validated: pipe-based functional smoke test; `go movetime 5000` completes in ~5.3s;
tested in CuteChess GUI (human vs. engine and engine vs. engine).

## 2026-03-12 — Time Management: Clock-Aware Soft/Hard Limits (PR #41)

### Added
- `Engine::compute_budget(remaining, increment, moves_to_go)` free function in
  `TimeUtils.h/cpp` — pure math, independently testable; formula:
  `soft = usable/horizon + inc*80%`, `hard = min(soft*3, usable/2)`
- `TimeManager` gains a two-arg `start(soft, hard)` overload +
  `should_stop_iteration()` (soft-limit gate); one-arg `start(allocated)` delegates
  unchanged for backward compatibility
- `PlayerAiBase::SetClockInfo()`: computes the budget and arms the timer;
  `clock_info_set_` flag prevents `StartTimer()` from overwriting clock-aware budgets
  (this method and flag were later superseded and deleted by the GetMove SearchLimits
  refactor, 2026-07-04)
- Node-based time polling in `pvs()` every 1,024 nodes (amortises `chrono::now()`
  overhead on deep searches)
- `[time_mgr]` test tag: 10 assertions (6 formula, 4 timing)

Plan: `.claude/plans/time-management-clock-aware.md`.

## 2026-03-10 — Late Move Reductions + Move Sorting Extraction (PR #38)

### Added
- **Late Move Reductions**: sqrt formula
  `R = min(max(1, sqrt(depth-1) * sqrt(si-1)), depth-1)`, applied to quiet/non-killer/
  non-evasion/non-PV-node moves (si≥3, depth≥3); 2-step re-search. Kill-switch
  `tuning_.lmr_enabled`. Observed depth 13-15 vs 8-9 (without LMR) in the same
  15-second budget; ~31M vs ~36M nodes/move. 10 `[search]` test cases via
  `AIPerlexTestFixture`. Plan: `.claude/plans/lmr-and-search-tests.md`.

### Changed
- **Move Sorting**: inline move scoring loop extracted from `pvs()` into
  `MoveSorter::ScoreMoves()` static method; precondition asserts + `isKiller1`
  short-circuit for fast killer detection. 5 `[sort]` test cases, 14 assertions.
  Plan: `.claude/plans/move-scoring-extraction-and-sort-tests.md`.

## 2026-03-08 — spdlog Level Gate + outLegalMoves Removal (PR #34)

### Changed
- 3-line per-call logging boilerplate in `AIPerplex` replaced with a spdlog level gate
  (`s_logger->set_level(...)`)
- Global `outLegalMoves` stream removed; board/root-move diagnostics now flow through
  the default spdlog logger at `debug` level
- `Board::test_bitboards` signature simplified

Plan: `.claude/plans/logging-spdlog-gate-and-outlegalmoves-removal.md`.

## 2026-03-07 — Aspiration Windows, C++20 Adoption, PCH Expansion, SearchTuning exposure (PR #29, #30, #31, #32)

### Added
- **Aspiration Windows in Iterative Deepening** (PR #30): narrow alpha/beta window
  around the previous depth's score, gradual widening (25cp → 75cp → full) on
  fail-high/fail-low. Kill-switch `tuning_.aspiration_enabled`;
  `search_with_aspiration()` extracted into its own method.
- **Expose SearchTuning via game_settings.json** (PR #31): all 8
  `AIPerplex::SearchTuning` parameters (including the `barelySearched`/
  `probablyIncomplete`/`pvTooShort` thresholds) exposed as a `search_tuning` JSON
  block, parsed into `Config::SearchTuningConfig`; absent block leaves AIPerplex
  defaults untouched. Also consolidated `m_MaxDepth`/`max_depth_` into a single
  canonical depth field across all AI types, and added a `time_limit` JSON key.

### Changed
- **C++20 Adoption — `<bit>` and `<format>`** (PR #32): `std::countr_zero` replaces
  `_tzcnt_u64` `#ifdef` in `Board::GetFirstPiece`; `std::format` replaces
  `std::stringstream` in `Move::Output()`. C++23 upgrade path documented in
  `.claude/plans/cpp23-upgrade.md`.
- **Expand PCH Coverage in StdAfx.h** (PR #29): 9 STL headers added; redundant per-TU
  includes removed from 7 source files. Zero warnings enforced (`/WX`).

## 2026-03-04 — Introduce MoveFormatter (PR #26)

### Added
- Stateless class centralizing move presentation in
  `StratEngine/MoveFormatter.h/cpp`: `ToShort` (pseudo-LAN + `+` check annotation),
  `ToVerbose` (verbose English), `ToUCI` (wire format), `FromUCI` (parse from board
  pre-`DoMove` state)
- Fixed gaps: verbose line restored; `+` now in `gamelist.txt`; fragile `\n`-surgery in
  `PrintBoardAndMove` removed; promotion-captures get a suffix in perft divide output
- 65 assertions, 6 test cases (`[formatter]`)

`Move::Output()`/`Move::Output(ePiece)` kept as-is (callers migrated separately — see
the "Migrate Move::Output() callers" issue). `ToSAN` omitted, deferred until PGN export
is needed. Plan: `.claude/plans/move-formatter.md`.

## 2026-03-03 — Move class → 16-bit layout, Phases 3-4 (PR #24)

### Changed
- **Phase 3**: Removed `MovPiece` field — `Board::GetEffectiveMovPiece()` added;
  `MoveFactory` drops the movPiece param; `MoveHelper`/`GameState`/`Sort` thread it
  explicitly instead; a Zobrist hash corruption bug in `UndoMove` fixed along the way
- **Phase 4**: Removed `Content` (captured-piece) field — `sizeof(Move) == 2` enforced
  via `static_assert`; 4 `PROMOTION_*_CAPTURE` MoveType variants added (capture bit 2 +
  promotion bit 3); `Board::GetCapturedPiece()` public API added; `MoveFactory` drops
  the captured param; `Move::Value`/`MoveHelper::IsValid`/`IsPieceCapturedAt` gain an
  explicit `content` param; `IsCapture`/`IsPromote` simplified to pure flag-bit tests

`BoardTests.cpp` added (6 `[board]` test cases); all 47 tests pass.

## 2026-03-01 — Phase 0 Test Coverage (PR #18)

### Added
- `[tt]` — TranspositionTable unit tests (store/probe, mate normalization, replacement,
  counters)
- `[eval]` — Evaluation position tests (symmetry, material advantage, doubled pawns,
  rook bonus)
- `[tactical]` — Fast search regression tests (mate-in-1, hanging piece capture via
  AIPerplex depth 4)
- `STRAT_ENABLE_TEST_ACCESS` friend stub added to `AIPerplex.h` for future Phase 1
  search tests

## 2026-03-01 — Restrict Board Piece-Setup API to Private (PR #21, commit d4b1bd6)

### Changed
- `ClearBoard`, `SetInitialColor`, and `AddPieceToBoard` moved to `private:` — no longer
  called from test code after PR #20; `SetupFromFEN` is the sole public board-setup API

## 2026-02-28 — Move class Phases 1-2 + Catch2 v3 migration (PR #12, #16)

### Changed
- **Move class → 16-bit layout, Phases 1-2** (PR #12): removed `Move::IsCheck` field
  (Phase 1); removed `[from|to]IsNoSquare` fields (Phase 2)
- **Migrate to Catch2 v3** (PR #16): 2-file amalgamated drop-in, no pre-build step
  required; test project `StratChessTests/StratChessTests.vcxproj` rebuilt from empty
  placeholder; tests migrated: `RepetitionTests.cpp` (TC1-TC7, TC9), `MoveFieldTests.cpp`,
  `PerftTests.cpp`; retired `TestFramework.h`, `Unittests.h`, `Perft_unittests.h`; tags
  `[repetition]`, `[moves]`, `[perft]`

## 2026-02-27 — Delta Pruning in Quiescence (PR #11)

### Added
- `tuning_.delta_pruning_margin = 200` added to `SearchTuning`; guard skips captures
  where `stand_pat + piece_value + margin < alpha` (not in check, not promotion)

Consistently deeper search — mate at depth 14 observed where it wasn't reached before.

## 2026-02-25 — Archive Broken Algorithms (PR #9)

### Removed
- `ABIterTrans.cpp/h` and `AITrans.cpp/h` moved to `StratEngine/Archived/`,
  `Archived/README.md` explains historical context, both removed from the build

## 2026-02-22 — Threefold / Twofold Repetition Correctness

### Fixed
- `push_position()` now called after `ChangePlayer()` in both `DoMove()` branches
- `is_repetition()` loop start corrected (parity fix)
- Twofold-in-search branch was unreachable; fixed dead condition
- Castling rights and en-passant square changes now included in Zobrist hash;
  `zobrist_hash_` widened from `unsigned int` to `uint64_t`

## 2026-02-20 — Killer Moves + History Heuristic (PR #6)

### Added
- Fully implemented in `AIPerplex`: `killers_[MAX_PLY][MAX_KILLERS]`,
  `history_[2][64][64]`; methods `clear_killers`, `store_killer`, `clear_history`,
  `age_history`, `update_history`
- Killers cleared at search start; history aged each iteration (halved to prevent
  overflow); scoring integrated inline in `pvs()` (relocation to `MoveSorter` handled
  separately, see 2026-03-10)

## 2026-02-19 — Perft Testing Framework (commit c13c7f2c)

### Added
- `StratEngine/Tests/Perft.h/cpp` + `PerftRunner.cpp`; `Tests/perft_test_cases.json`
  (128 positions, depth 1-6); integrated into main binary
  (`StratChessEvolved.exe perft test`)
- Surfaced design issues around `GameInfo` state (en-passant, etc.) that had been
  stored in `Game` rather than `Board`; one `MoveGenerator` defect found and fixed
- All 128 test cases passing at every depth where sensible; ~21M NPS in Release

Direct commit, predates PR-based workflow (before PR #1) — not part of any pull request.

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

## Undated

These entries have no PR reference in the original Roadmap.md text and couldn't be
matched to a specific PR with confidence via title/content search. Believed to fall
roughly within the Feb–March 2026 window based on their original position in the
document, but treat the ordering here as unverified.

### Changed
- **TranspositionTable Thread-Safety**: per-bucket `shared_mutex` locks (probes
  `shared_lock`, stores `unique_lock`); global `shared_mutex` for resize/clear; TT only
  cleared on new game.
- **GameInfo History in Board**: history arrays moved from Player into `Board`;
  prerequisite for De-Singleton Board.
- **Move sorting: stack-allocated sort buffer**: `pvs()` uses a stack `std::array`
  instead of heap allocation per call.

### Removed
- **Remove Dead Code**: commented-out old `Search()` method body removed from
  `AIPerplex.cpp`.

### Fixed
- **Fix `zobrist::initialize()` Never Called**: now called in the `Board` constructor;
  all castling/en-passant/side-to-move Zobrist keys initialised before any `Board`
  method runs.
