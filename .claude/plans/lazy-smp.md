# Lazy SMP Parallel Search (Design + Plan)

**Status**: design approved 2026-07-03; implementation not started — resumable in any session.
**Prerequisite**: GetMove SearchLimits refactor (`.claude/plans/getmove-searchlimits-refactor.md`)
must land first — self-contained `GetMove` calls remove the pre-call ordering contract a
search thread could violate.
**Roadmap item**: ⚪ "Implement Lazy SMP" (promotes to active once this starts).

## Goal

Multiple threads search the same position sharing the transposition table; helper threads
warm the TT so the main thread's iterative deepening sees deeper entries and better move
ordering. Target: 2–3x effective speedup on 4 cores (measured as ELO gain at fixed time
control, not just NPS).

**Scope limits**: `AIPerplex` only — legacy AIs stay single-threaded. TT implementation
unchanged in v1 (existing per-bucket `shared_mutex`). No search-algorithm changes (no
depth-skip patterns, no helper-result voting). Main thread's result is the only result.

## Design Decisions (approved 2026-07-03)

1. **Spawn-per-search, not a persistent pool.** `GetMove()` at `threads=N` spawns N−1
   helper `std::jthread`s, runs the main search on the calling thread, stops and joins
   helpers before returning. Thread spawn is ~µs against seconds/move; a persistent pool
   is a follow-up only if profiling ever shows the cost. No idle-loop/condvar machinery.
2. **`threads=1` is byte-identical to today.** The single-threaded path must not touch any
   thread machinery — same code path, same node counts. This is Gate 1 and the rollback
   guarantee: the feature is inert until opted into.
3. **Main-is-authoritative.** Helpers never report moves; all quality assessment
   (rejection reasons, stability, emergency handling), root-state propagation, logging of
   results, and time management stay main-thread-only. Best move propagates through the
   shared TT, nothing else.
4. **No depth-skip patterns in v1.** Helpers run a plain iterative-deepening loop depth
   1→max; divergence comes from TT-timing races alone (Stockfish measured skip patterns
   at ~0 and removed them in SF9).
5. **TT keeps per-bucket locks in v1.** Already thread-safe, entries never torn, and the
   current single-threaded search already pays the lock cost — zero-regression. A lockless
   Hyatt-XOR entry redesign is a separate Roadmap follow-up with its own NPS/ELO gate
   (this plan adds that Roadmap entry).
6. **Threads config**: UCI `option name Threads type spin default 1 min 1 max 32` +
   `"threads"` field in `game_settings.json`. Engine default stays 1 permanently
   (convention — Stockfish ships Threads=1; match runners and the ELO harness need
   explicit, symmetric thread control; fixed-depth test modes stay reproducible). After
   validation, the JSON value is what flips to 4 (or whatever measures best) for play.

## Architecture

```
GetMove(info, limits)                         [main/calling thread]
 ├─ init_search, resolve limits, arm TimeManager (main only)
 ├─ threads_ == 1 ─────────────► exactly today's path: iterative_deepening(td_, ...)
 └─ threads_ == N > 1
     ├─ for i in 1..N-1: helper_tds_[i-1].board = m_Board copy; spawn jthread:
     │     helper_loop(*helper_tds_[i-1], effective_depth, tt)
     │       └─ for depth 1..max: search_with_aspiration(td, depth, seed, tt)
     │          break when IsAborted()          [relaxed atomic load per node]
     ├─ iterative_deepening(td_, effective_depth, tt)   [unchanged main search]
     ├─ time_manager_.stop()                    [latches abort → helpers collapse O(depth)]
     └─ join all helpers; nodes = td_.nodes + Σ helper nodes; return main's result
```

- **Per-thread state**: main keeps the persistent `td_` member; helpers get a persistent
  `std::vector<std::unique_ptr<ThreadData>> helper_tds_` (sized lazily on first use;
  `thread_id` = 1..N−1). Persistent so history/killers age across moves per thread, same
  as main. Each helper's `board` is copy-assigned from the game board every move (same as
  main's `td_.board`).
- **Helper loop** (`helper_loop(ThreadData&, int max_depth, TranspositionTable&)`): plain
  ID loop calling the existing `search_with_aspiration`. No `SearchState` quality gates,
  no `assess_iteration_quality`, no emergency moves, no `update_game_state`, no
  per-iteration logging. Seeds each aspiration window from its own previous iteration's
  score. Exits on `IsAborted()` or depth exhaustion, then simply returns (result discarded).
- **Stopping protocol**: `TimeManager::should_stop_search()` latches `should_stop_`
  (relaxed atomic) on hard-limit expiry; `stop()` sets it directly. Helpers only ever
  *read* via `IsAborted()`. Main sets `stop()` unconditionally after its ID loop returns,
  then joins. UCI `stop` already routes to `StopSearch()` → same latch. No condition
  variables, no new synchronization primitives anywhere.

## Shared-state audit (the real work — each item is a plan task)

| State | Today | Under SMP |
|---|---|---|
| `nodes_since_check_` (PlayerAiBase) | member, incremented in `pvs()` | move into `ThreadData`; only thread 0 performs clock checks — helpers use `IsAborted()` only |
| `Eval` (`unique_ptr<EvalManager>`, per player) | single instance called from search | **audit for mutable state**; if stateful, clone per `ThreadData`; if stateless, document const-ness and share |
| `tt_misses` debug multimap + `assert_tt_store` (AIPerplex) | unconditional debug helpers | gate to `thread_id == 0` or remove (multimap is not thread-safe) |
| `m_TotalTime`/`m_TotalCount` statics (PlayerAiBase) | updated in `StopTimerAndAdjustVars` | already main-only after join — verify, document |
| `last_result_`, `m_BestMove`, root `GameInfo` propagation | main path | main-only after join — no change, verify |
| spdlog `s_logger` | `basic_file_sink` | verify `_mt` sink or gate helper logging to off; helpers must not log in hot path anyway |
| `PVTable`, killers, history, `info_seq`, board | already per-`ThreadData` | none — this is what PR #74 bought |
| `SearchTuning tuning_` | read-only after config | shared read-only — document, never written during search |
| TT | per-bucket `shared_mutex` | unchanged (Decision 5) |
| Static attack/Zobrist tables (BitBoardHelper etc.) | const, init at startup | read-only — verify no lazy init races |

## Config plumbing

- `UCIHandler`: `setoption name Threads value N` → clamp to [1, 32] → `AIPerplex::SetThreads(n)`
  (declared on `PlayerAiBase` as virtual no-op so legacy AIs ignore it); advertise the
  option in `cmd_uci`'s option list.
- `Config`/`Game.cpp`: optional per-player `"threads"` field (default 1) applied in
  `SetPlayerParams`.
- `game_settings.json`: committed file keeps `"threads": 1` until Gate 3 passes; flipping
  it is the post-validation act.

## Key Correctness Properties

1. `threads=1`: byte-identical node/move/score/depth sequence vs pre-SMP baseline across a
   full fixed-depth self-play game (same validation as the ThreadData refactor).
2. No data races: every mutable datum is per-`ThreadData`, behind the TT's bucket locks,
   an atomic, or provably main-thread-only (the audit table above is the checklist).
3. Helpers can never outlive `GetMove()` — join is unconditional, including on the
   emergency/no-move paths.
4. A helper cannot corrupt the game: it owns no reference to the game board, never touches
   `m_Board`, `info` out-param, events, or `last_result_`.
5. Mate scores remain correct through the shared TT (normalize/denormalize is ply-relative
   and already lock-protected per entry).
6. UCI `stop` during a multi-threaded search terminates all threads promptly (latched
   abort → O(depth) collapse per thread).

## Validation Plan (merge gates, approved 2026-07-03)

- **Gate 1 — inert at 1 thread**: fixed-depth self-play, byte-identical node counts vs
  pre-SMP baseline. Any drift = the refactor changed the search; fix before proceeding.
- **Gate 2 — stable at 4 threads**: `tactical stability 20` at `threads=4` → 0 failing
  runs, 0 flips (this gate was built for exactly this moment). Plus
  `Validate-PrePR.ps1` full pass (its Step 3 covers threads=1 stability).
- **Gate 3 — measured gain**: `Run-EloMatch.ps1` candidate-at-threads=4 vs
  candidate-at-threads=1 (same binary, different Threads option): positive score with
  LOS > 95% before merge. Record NPS scaling at 2/4/8 threads in the PR body.
- Deep perft (unaffected code path, cheap regression check) + all Catch2 tiers.
- **AIAgent self-play** (`"type": 3` both sides): the `nodes_since_check_` move into
  `ThreadData` and the virtual `SetThreads` no-op touch `PlayerAiBase`, so legacy-AI
  regression coverage is required per CLAUDE.md (legacy AIs must be completely unaffected:
  single-threaded, same moves, no crash/hang).
- Note: `Run-EloMatch.ps1` needs a small pre-step — verify it can pass per-engine UCI
  options (fastchess `option.Threads=N` syntax) to drive Gate 3.

## Follow-ups this plan creates (Roadmap entries, not v1 work)

- **Lockless TT (Hyatt XOR)**: replace per-bucket locks with self-validating atomic
  entries; own PR, gated on measured NPS scaling + non-regression ELO match.
- **Persistent thread pool**: only if spawn-per-search ever shows in profiling.
- Flip `game_settings.json` `"threads"` to the measured best value post-merge.

## Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

Search internals change, so the **search-reviewer subagent must be dispatched** before the
PR (pre-PR checklist step 3).

**Estimate**: 3–5 days including the three gates (Gate 3 alone is ~2 h of match time).

### Global constraints (bind every task)

- MSVC Level4 + `/WX` on x64; `static_cast<>` for narrowing; no `#pragma warning(disable)`.
  C++20 (`<thread>`/`<jthread>` are in the PCH candidates — add to `StdAfx.h` if used in
  more than one TU).
- Prerequisite verified before Task 1: the SearchLimits refactor is merged
  (`GetMove(info, limits)` exists on `origin/main`).
- Work in a fresh worktree from `origin/main`. Capture the **pre-SMP baseline** (fixed-depth
  self-play node-count log per `.claude/plans/extract-threaddata-structure.md` method)
  BEFORE the first code change — Gates depend on it.
- After every task: `threads=1` remains byte-identical to the baseline. Any drift is a
  stop-the-line bug in that task, not a note for later.
- Names fixed by this design: `PlayerAiBase::SetThreads(unsigned)` (virtual no-op),
  `AIPerplex::helper_loop(ThreadData&, int max_depth, TranspositionTable&)`,
  `helper_tds_`, `threads_`, UCI option `Threads` (spin, default 1, min 1, max 32),
  JSON key `"threads"`.

### Task 0: harness pre-step — fastchess per-engine UCI options

- [ ] Verify `Run-EloMatch.ps1` can pass `option.Threads=N` per engine (fastchess
  `-engine ... option.Threads=4` syntax; check fastchess v1.8.0 docs/`--help`). If the
  script's engine-spec builder doesn't forward arbitrary options, add an optional
  `-CandidateOptions`/`-ReferenceOptions` passthrough parameter. Smoke: 2-game match,
  both sides threads=1, confirms no behavior change. Commit (script-only change).

### Task 1: shared-state audit sweep (still single-threaded after this task)

**Files**: `StratEngine/ThreadData.h`, `StratEngine/PlayerAI.h`, `StratEngine/AIPerplex.h/.cpp`,
`StratEngine/Eval.h/.cpp` (audit outcome may touch), `Docs/TestDesign.md` (audit record).

- [ ] Move `nodes_since_check_` from `PlayerAiBase` (`PlayerAI.h:238`) into `ThreadData`
  (init 0; reset where `td.nodes_searched` is reset). Update the two increment sites
  (`AIPerplex.cpp:304` and `:558`) to `td.nodes_since_check_`, **gated on thread 0**:

```cpp
if (td.thread_id == 0 && (++td.nodes_since_check_ & 1023) == 0) {
    if (ShouldStopSearch())
        return GameValues::Draw;
}
```

  (Helpers rely on the `IsAborted()` check already at the top of `pvs()`/`quiescence()` —
  a relaxed load per node, no clock calls off the main thread.)
- [ ] Gate the `tt_misses` multimap and `debug_tt_cache_misses`/`assert_tt_store` debug
  helpers to `td.thread_id == 0` (or delete `tt_misses` outright if it's dead debug code —
  check call sites; prefer deletion).
- [ ] **Eval mutability audit**: read `Eval.h/.cpp` + `EvalManager` for any member written
  during evaluation (caches, scratch buffers, lazily-built tables). Record the finding in
  the PR notes. If stateful: add `Eval` clone per `ThreadData` (factory call per helper).
  If stateless: add a comment on the class stating the SMP sharing contract.
- [ ] Verify and document (comment, not code): `m_TotalTime`/`m_TotalCount` statics and
  `StopTimerAndAdjustVars` are called only after helpers join; spdlog sinks — confirm
  `_mt` variants or that helper threads never log (design: helpers don't log in v1);
  static attack/Zobrist tables have no lazy init (grep for function-local statics in
  `BitBoardHelper`/Zobrist init paths).
- [ ] Validate: full fast tier + **byte-identical node counts vs the pre-SMP baseline**
  (this task must be provably inert). Commit.

### Task 2: Threads config plumbing (no threading yet)

**Files**: `StratEngine/PlayerAI.h`, `StratEngine/AIPerplex.h`, `StratEngine/UCIHandler.cpp`,
`StratEngine/Config.h/.cpp`, `StratEngine/Game.cpp`, `StratChessEvolved/game_settings.json`.

- [ ] `PlayerAiBase`: `virtual void SetThreads(unsigned) noexcept {}` (legacy AIs ignore).
  `AIPerplex`: `void SetThreads(unsigned n) noexcept override { threads_ = std::clamp(n, 1u, 32u); }`
  with `unsigned threads_{ 1 };`.
- [ ] UCI: advertise `option name Threads type spin default 1 min 1 max 32` in `cmd_uci`;
  handle `setoption name Threads value N` (follow the existing setoption parsing pattern
  in `UCIHandler.cpp`; if no setoption handler exists yet, add the minimal name/value
  parser for exactly this option).
- [ ] Config/Game: optional per-player `"threads"` (default 1) → `SetThreads` in
  `SetPlayerParams`. Committed `game_settings.json` gets `"threads": 1` for both players
  (comment: flip after ELO validation).
- [ ] Unit test `[smp]`: `SetThreads(0)` clamps to 1, `SetThreads(64)` clamps to 32.
  UCI smoke: `setoption name Threads value 4` accepted silently, search still works.
  Byte-identical check still holds (threads_ is set but unused). Commit.

### Task 3: helper threads (the core)

**Files**: `StratEngine/AIPerplex.h/.cpp`, `StratEngine/StdAfx.h` (`<thread>`).

- [ ] `AIPerplex.h`: add
  `std::vector<std::unique_ptr<ThreadData>> helper_tds_;` and
  `void helper_loop(ThreadData& td, int max_depth, TranspositionTable& tt);`
- [ ] `helper_loop` — plain ID, no quality gates, result discarded:

```cpp
void AIPerplex::helper_loop(ThreadData& td, int max_depth, TranspositionTable& tt)
{
    int seed_score = 0;
    for (int depth = 1; depth <= max_depth; ++depth) {
        if (IsAborted())
            break;
        const int score = search_with_aspiration(td, depth, seed_score, tt);
        if (IsAborted())
            break;            // partial iteration — discard score
        seed_score = score;
    }
}
```

- [ ] `GetMove`: after `init_search`/`ApplyLimits`, wrap the existing main search:

```cpp
std::vector<std::jthread> helpers;
if (threads_ > 1) {
    if (helper_tds_.size() < threads_ - 1) {
        const size_t old = helper_tds_.size();
        helper_tds_.resize(threads_ - 1);
        for (size_t i = old; i < helper_tds_.size(); ++i) {
            helper_tds_[i] = std::make_unique<ThreadData>();
            helper_tds_[i]->thread_id = static_cast<int>(i) + 1;
        }
    }
    helpers.reserve(threads_ - 1);
    for (size_t i = 0; i < threads_ - 1; ++i) {
        ThreadData& htd = *helper_tds_[i];
        htd.board = m_Board;                       // same seed as td_.board
        htd.info_seq.clear();
        htd.info_seq.emplace_back(info);           // same root info as init_search gives td_
        htd.clear_killers();
        htd.clear_null_move_flags();
        htd.nodes_searched = 0;
        helpers.emplace_back([this, &htd, effective_depth, this_tt = _tt.get()] {
            helper_loop(htd, static_cast<int>(effective_depth), *this_tt);
        });
    }
}

SearchResult result = iterative_deepening(td_, static_cast<int>(effective_depth), *_tt);

time_manager_.stop();          // latch abort — helpers collapse in O(depth)
helpers.clear();               // jthread joins on destruction

int64_t total_nodes = td_.nodes_searched;
for (size_t i = 0; i + 1 < static_cast<size_t>(threads_); ++i)
    total_nodes += helper_tds_[i]->nodes_searched;
```

  Exact helper root-state seeding must mirror whatever `init_search`/`iterative_deepening`
  do for `td_` — read `init_search` first and replicate per helper (extract a
  `prepare_thread(ThreadData&, const GameInfo&)` helper if the logic is more than the
  lines above, so main and helpers can't drift). **The `threads_ == 1` path must not
  construct the vector or touch any of this** (guard the whole block).
- [ ] Report `total_nodes` in the completion log and `StopTimerAndAdjustVars(...)`;
  `last_result_.nodes_searched = total_nodes`.
- [ ] Ensure join-before-return on ALL exits of `GetMove` (including the empty-move
  emergency path) — the `helpers` vector's scope already guarantees it if declared before
  the search call; verify no early `return` precedes the joins.
- [ ] Validate: **Gate 1** — threads=1 byte-identical vs baseline. Then threads=2 and 4
  smoke: tactical suite single run passes, self-play game completes, nodes scale up,
  no crash/hang under 60 s UCI session with `stop` mid-search. Commit.

### Task 4: Gate 2 — stability + regression sweep

- [x] `tactical stability 20` with a threads=4 configuration → 0 failing runs, 0 flips.
  (Mechanism: the tactical runner constructs its own AI — add an optional fourth CLI arg,
  `tactical stability 20 tactical_test_cases.json 4` = threads, default 1, plumbed to
  `SetThreads` in `TacticalTestRunner::run_position`; document in `Docs/TestDesign.md`.)
- [x] `.\build.ps1 extended-tests` (all tiers), deep perft from `Tests/`, AIAgent
  self-play (`"type": 3`) 60 s — legacy AIs unaffected.
- [x] `Validate-PrePR.ps1` full gate (covers threads=1 stability + self-play). Commit any
  fixes; document Gate 2 results in the plan file's Gate log (append section).

### Task 5: Gate 3 — measured gain + docs + PR

- [ ] NPS scaling: fixed position set (e.g. the 31 suite positions at fixed depth 8),
  record total nodes/time at threads=1/2/4/8 — table for the PR body.
- [ ] `Run-EloMatch.ps1` candidate-threads=4 vs candidate-threads=1 (same binary, Task 0
  options; 10+0.1 TC as standard). Merge bar: positive score, LOS > 95%. If negative or
  flat: stop, diagnose (TT contention? helper depth policy?) before any merge.
- [ ] Roadmap: move "Implement Lazy SMP" to Completed Work with measured numbers; add the
  ⚪ **Lockless TT (Hyatt XOR)** follow-up entry (own PR, NPS + ELO gated) and the
  persistent-thread-pool note; update `CLAUDE.md` key-files section (ThreadData comment
  re: helper threads now real).
- [ ] Dispatch **search-reviewer** on the diff; address findings.
- [ ] Sync `origin/main`, `Validate-PrePR.ps1` once more if code changed, PR with
  Why/Summary/Test plan (three gates + numbers)/Notes. Post-merge follow-up (user
  decision): flip `game_settings.json` `"threads"` to the measured best.

## Gate Results

### Gate 2 — stability + regression sweep (2026-07-23, Task 4)

Implementation: added an optional 4th CLI positional arg (threads, default 1) to
`tactical stability` in `StratChessEvolved.cpp`; validated as a positive integer by the
CLI (upper bound left to `AIPerplex::SetThreads`'s own `[1,32]` clamp). Threaded a new
`unsigned threads = 1` parameter through `TacticalTestRunner::run_stability_suite` →
`run_test_suite` → `run_position`, applied via `dynamic_cast<PlayerAiBase*>(ai.get())->SetThreads(threads)`
before `GetMove()` (same pattern as `Game.cpp`'s `SetPlayerParams`). Documented in
`Docs/TestDesign.md`.

| Check | Command | Result |
|---|---|---|
| Tactical stability @ threads=4 | `Tests/` → `../x64/Release/StratChessEvolved.exe tactical stability 20 tactical_test_cases.json 4` | **PASS** — 20/20 runs, 31/31 positions each run, **0 failing runs, 0 flipped positions** |
| Extended Catch2 tiers | `.\build.ps1 extended-tests` | **PASS** — 2311 assertions in 170 test cases |
| Deep perft | `Tests/` → `../x64/Release/StratChessEvolved.exe perft test` | **PASS** — 640/640 |
| AIAgent self-play (legacy AI, `"type": 3` both sides) | `Start-Process StratChessEvolved.exe game`, 60 s timeout | **PASS** — 4 moves completed, no crash/hang/error in stdout or stderr; `game_settings.json` restored afterward |
| Full pre-PR gate | `Scripts\Validate-PrePR.ps1` (build + extended tests + tactical stability@1 (default) + AIPerplex self-play) | **PASS** — Full build / Extended tests / Tactical suite / Self-play all green; tactical suite ran 10/10 with 0 flips at the default threads=1, confirming the new optional arg didn't disturb the existing invocation |

**Conclusion**: Gate 2 passes cleanly. Lazy SMP helper threads (Task 3) introduce no
observed nondeterminism in the tactical suite at threads=4 across 20 repeated runs, and
the `PlayerAiBase::SetThreads` no-op / `nodes_since_check_` relocation (Tasks 1-2) leave
legacy AIs (`AIAgent`) fully functional. Proceed to Task 5 (Gate 3 — measured ELO gain).
