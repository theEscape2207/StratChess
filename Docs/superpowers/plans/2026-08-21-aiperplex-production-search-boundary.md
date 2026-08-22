# AIPerplex Production-Search Boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `AIPerplex` into a concrete board-per-call search service, retain game mode through a
by-value `SearchPlayer`, and remove production downcasts and hidden search-result state without
changing the search tree, time controls, UCI output, or performance-log contract.

**Architecture:** A composed `SearchControl` centralizes limit and abort behavior for both legacy and
production search. `AIPerplex` owns its evaluator, TT, thread data, tuning, and control and returns all
completed-search telemetry in `SearchResult`. Game mode constructs it through a config-aware player
factory and stores it by value in `SearchPlayer`; UCI owns the concrete service directly.

**Tech Stack:** C++20, CMake/Ninja with Windows clang-cl Release preset, Catch2, PowerShell validation
and equivalence scripts, fastchess operational smoke testing.

**Spec:** `.claude/plans/aiperplex-production-search-boundary.md`

## Global Constraints

- Keep `IPlayer::GetMove(const SearchLimits&)` unchanged; `SearchPlayer` is required and owns
  `AIPerplex` by value.
- Do not introduce `ISearchEngine`, a generic search factory, or a fake-search UCI seam.
- Keep `AIPerplex::Search(const Board&, const SearchLimits&, IterationObserver)` synchronous and copy
  the supplied root exactly once into each participating `ThreadData`.
- Preserve fixed-depth `Threads=1` accepted iterations, scores, node counts, PVs, terminal result, and
  best move exactly.
- Preserve time-limit precedence, soft/hard clock behavior, explicit stop, node polling every 1024
  node entries on thread 0, and the entry-time `threads_` snapshot.
- Preserve UCI's single `AIPerplex` identity across `ucinewgame`, option/output ordering, node split,
  and measurement-contract version 1.
- Preserve all six `SimplePerfStats.txt` columns and the final three columns' cumulative totals across
  both game players.
- Remove production implementation downcasts, `AIPerplex::last_result_`/`GetLastResult()`, and
  `IPlayer::GetBestScore()`; keep the non-virtual legacy `PlayerBase::GetBestScore()` helper.
- Use strict red-green-refactor: every behavioral production change starts with a focused test that
  fails for the intended missing behavior.
- After every task, update and commit `256-progress.md` with just completed work, key learnings, exact
  next steps, and an explicit safe park point.

---

### Task 1: Compose shared search control

**Files:**
- Create: `StratEngine/SearchControl.h`
- Create: `StratEngine/SearchControl.cpp`
- Create: `StratChessTests/SearchControlTests.cpp`
- Modify: `StratEngine/PlayerAI.h`
- Modify: `StratEngine/PlayerAI.cpp`
- Modify: `StratEngine/AIPerplex.h`
- Modify: `StratEngine/AIPerplex.cpp`
- Modify: `StratChessTests/SearchTests.cpp`
- Modify: `256-progress.md`

**Interfaces:**
- Consumes: `Engine::resolve_limits`, `chess::TimeManager`, `SearchLimits`.
- Produces: `SearchControl` with `SetDefaults`, `ApplyLimits`, `Stop`, `StopRequested`,
  `ShouldStopIteration`, `NodeLimitReached`, `IsAborted`, `EffectiveDepth`, and `Elapsed`.
- Produces: one `SearchControl` value member in `PlayerAiBase`; `AIPerplex` initially reaches it
  through the legacy base, then owns it directly in Task 3.

- [ ] **Step 1: Write failing value and wiring tests**

Add focused Catch2 cases tagged `[search_control]` that hand-derive the expected behavior:

```cpp
TEST_CASE("SearchControl resets a latched stop when new limits are applied", "[search_control]")
{
	SearchControl control(6, 15000ms);
	control.ApplyLimits(SearchLimits{.depth = 4});
	control.Stop();
	REQUIRE(control.IsAborted());

	control.ApplyLimits(SearchLimits{.depth = 3});
	CHECK_FALSE(control.IsAborted());
	CHECK(control.EffectiveDepth() == 3);
}

TEST_CASE("SearchControl latches the node budget at the first reached total", "[search_control]")
{
	SearchControl control(6, 15000ms);
	control.ApplyLimits(SearchLimits{.nodes = 2048});
	CHECK_FALSE(control.NodeLimitReached(2047));
	CHECK(control.NodeLimitReached(2048));
	CHECK(control.IsAborted());
}
```

Extend the existing legacy fixture and AIPerplex node-limit case so each proves that calling `Stop`
or reaching the limit through the composed member aborts the real search path, not only the value
type.

- [ ] **Step 2: Run the focused tests and verify RED**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[search_control]"
```

Expected: compilation fails because `SearchControl` and its header do not exist. After the test file
is discoverable, the tests must still fail until limit application resets the abort latch and the
node budget latches at the exact requested total.

- [ ] **Step 3: Implement the minimal composed component and forward legacy access**

Use this public shape:

```cpp
class SearchControl final {
  public:
	SearchControl(unsigned default_depth, std::chrono::milliseconds default_time) noexcept;
	void SetDefaults(unsigned depth, std::chrono::milliseconds time) noexcept;
	void ApplyLimits(const SearchLimits& limits);
	void Stop() noexcept;
	bool StopRequested() const noexcept;
	bool ShouldStopIteration() const noexcept;
	bool NodeLimitReached(int64_t searched_nodes) noexcept;
	bool IsAborted() const noexcept;
	unsigned EffectiveDepth() const noexcept;
	std::chrono::milliseconds Elapsed() const noexcept;

  private:
	chess::TimeManager time_manager_;
	unsigned default_depth_;
	std::chrono::milliseconds default_time_;
	unsigned effective_depth_{0};
	int64_t node_limit_{0};
};
```

`ApplyLimits` calls `Engine::resolve_limits`, arms `TimeManager`, and replaces every per-call resolved
field. `PlayerAiBase::SetMaxDepth`, `SetTimeLimit`, `ApplyLimits`, `StopRequested`,
`NodeLimitReached`, `IsAborted`, `ShouldStopIteration`, `Elapsed`, and `StopSearch` forward to its
`SearchControl` value member. Move every legacy and AIPerplex read of `effective_depth_` to
`EffectiveDepth()`. Remove the duplicate `time_manager_`, `_startingTime`, `effective_depth_`,
`node_limit_`, and unused `stop_search_` state only after every current caller has moved.

- [ ] **Step 4: Run focused and search tests and verify GREEN**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[search_control]"
.\build\windows-clang-cl\StratChessTests.exe "[search]"
```

Expected: all focused control and search cases pass; the existing deterministic 1024-entry
node-overshoot assertions remain unchanged.

- [ ] **Step 5: Update the durable progress document and commit**

Record the exact tests run, any control-state ownership surprises, and Task 2 as the next step in
`256-progress.md`. Then commit only Task 1 files and the progress update:

```powershell
git add StratEngine/SearchControl.h StratEngine/SearchControl.cpp StratEngine/PlayerAI.h `
  StratEngine/PlayerAI.cpp StratEngine/AIPerplex.h StratEngine/AIPerplex.cpp `
  StratChessTests/SearchControlTests.cpp StratChessTests/SearchTests.cpp 256-progress.md
git commit -m "refactor: compose shared search control (#256)"
```

---

### Task 2: Return elapsed time and move performance totals into Game

**Files:**
- Modify: `StratEngine/SearchResult.h`
- Modify: `StratEngine/PlayerAI.h`
- Modify: `StratEngine/PlayerAI.cpp`
- Modify: `StratEngine/AIBasic.cpp`
- Modify: `StratEngine/AIAgent.cpp`
- Modify: `StratEngine/ABIterative.cpp`
- Modify: `StratEngine/AIPerplex.cpp`
- Modify: `StratEngine/Game.h`
- Modify: `StratEngine/Game.cpp`
- Modify: `StratChessTests/GameLoopTests.cpp`
- Modify: `StratChessTests/SearchTests.cpp`
- Modify: `256-progress.md`

**Interfaces:**
- Consumes: `SearchControl::Elapsed()` from Task 1.
- Produces: `SearchResult::elapsed`, with legacy algorithms reporting `m_SearchCount` in
  `nodes_searched` and zero in `qnodes_searched`.
- Produces: Game-owned cumulative elapsed/node totals used by the existing `SimplePerfStats` logger.

- [ ] **Step 1: Write failing result and Game accounting tests**

Add a search test proving a completed result carries non-negative elapsed time and a legacy search
returns its real unsplit nodes. Add a Game-loop test that feeds two AI results and checks the logged
six fields use current-result values for the first three and the sum of both results for the final
three:

```cpp
CHECK(fields.size() == 6);
CHECK(fields[1] == std::to_string(second.nodes_searched + second.qnodes_searched));
CHECK(fields[4] == std::to_string(first.nodes_searched + first.qnodes_searched +
                                  second.nodes_searched + second.qnodes_searched));
```

The fixture must use the real Game logging boundary or a private friend fixture; do not add a
production getter used only by tests.

- [ ] **Step 2: Run focused tests and verify RED**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[game_loop],[search]"
```

Expected: compilation fails because `SearchResult::elapsed` and Game-owned totals do not exist.

- [ ] **Step 3: Implement returned telemetry and Game-owned accumulation**

Add:

```cpp
std::chrono::milliseconds elapsed{0};
```

to `SearchResult`. Delete `PlayerAiBase::StopTimerAndAdjustVars`; populate each legacy result from
the composed control's elapsed value and its unsplit `m_SearchCount`. Populate AIPerplex after helpers
are joined and node totals are aggregated. Remove `m_TotalTime`/`m_TotalCount` and all perf logging
from `PlayerAiBase`.

Add private Game totals initialized to zero with Game lifetime. Immediately after an AI `GetMove`
returns, add elapsed and `nodes_searched + qnodes_searched`, then write the same six columns and keep
the existing 0ms-to-1ms display guard where division requires it. Do not log human moves.

- [ ] **Step 4: Run focused tests and the fast suite and verify GREEN**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[game_loop],[search]"
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PreCommit.ps1
```

Expected: focused tests and all non-slow tests pass; `SimplePerfStats.txt` remains six columns.

- [ ] **Step 5: Update progress and commit**

Record the exact accounting semantics and Task 3 next actions in `256-progress.md`, then commit:

```powershell
git add StratEngine/SearchResult.h StratEngine/PlayerAI.h StratEngine/PlayerAI.cpp `
  StratEngine/AIBasic.cpp StratEngine/AIAgent.cpp StratEngine/ABIterative.cpp `
  StratEngine/AIPerplex.cpp StratEngine/Game.h StratEngine/Game.cpp `
  StratChessTests/GameLoopTests.cpp StratChessTests/SearchTests.cpp 256-progress.md
git commit -m "refactor: return search telemetry to game (#256)"
```

---

### Task 3: Establish the concrete AIPerplex service API

**Files:**
- Modify: `StratEngine/AIPerplex.h`
- Modify: `StratEngine/AIPerplex.cpp`
- Modify: `StratEngine/ThreadData.h`
- Modify: `StratChessTests/SearchTests.cpp`
- Modify: `StratChessTests/TacticalTestHelpers.h`
- Modify: `StratChessTests/TacticalFullTests.cpp`
- Modify: `StratChessTests/FiftyMoveRuleTests.cpp`
- Modify: `256-progress.md`

**Interfaces:**
- Consumes: `SearchControl` and returned telemetry from Tasks 1-2.
- Produces: `using IterationObserver = std::function<void(const IterationInfo&)>`,
  `AIPerplexConfig`, and
  `SearchResult Search(const Board&, const SearchLimits&, IterationObserver = {})`.
- Transitional constraint: retain the inherited `GetMove`, metadata, and constructor accepting
  `Board&` only until Tasks 4-5 migrate the remaining front ends; all search internals must use the
  new owned fields and supplied root.

- [ ] **Step 1: Write failing construction, board-per-call, and observer-lifetime tests**

Construct two distinct boards and one search object. Search each at a shallow fixed depth and verify
the second result corresponds to the second board, proving no constructor board is retained by the
new path. Add a repeated-call observer case:

```cpp
int first_observations = 0;
const auto first = ai.Search(first_board, SearchLimits{.depth = 2},
                             [&](const IterationInfo&) { ++first_observations; });
int second_observations = 0;
const auto second = ai.Search(second_board, SearchLimits{.depth = 2},
                              [&](const IterationInfo&) { ++second_observations; });
CHECK(first_observations > 0);
CHECK(second_observations > 0);
```

The production mutation these tests catch is reading inherited `m_Board` or retaining the first
observer between calls. Add config assertions through observable behavior: requested evaluator
description is not the test target; use the resulting score or evaluator type only where the current
test fixture already exposes the real evaluator.

- [ ] **Step 2: Run the search tests and verify RED**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[search]"
```

Expected: compilation fails because `AIPerplexConfig` and `Search` do not exist.

- [ ] **Step 3: Add complete config and move AIPerplex internals to owned state**

Define the observer next to `IterationInfo`:

```cpp
using IterationObserver = std::function<void(const IterationInfo&)>;
```

Use this configuration boundary:

```cpp
struct AIPerplexConfig {
	EvalManager::EvalTypes evaluator{EvalManager::EvalTypes::COMPLEX};
	unsigned default_depth{4};
	std::chrono::milliseconds default_time{15000};
	unsigned hash_mb{AIPerplex::DEFAULT_HASH_MB};
	unsigned threads{1};
	SearchTuning tuning{};
	bool verbose_logging{false};
};
```

If the nested-type dependency makes `AIPerplex::DEFAULT_HASH_MB` or `SearchTuning` unavailable at
declaration, move the constants and tuning value type to namespace-level names used by both config
and class; do not duplicate defaults.

Implement `Search(root, limits, observer)` by copying `root` into `td_.board`, passing the observer
down the accepted-iteration emission path without storing it across calls, snapshotting `threads_`,
joining helpers, aggregating nodes, filling elapsed, and returning a local `SearchResult`.

Create the evaluator, TT, control defaults, tuning, logging policy, and thread count during
construction. Replace inherited evaluator, control, score-cache, and default-limit reads with owned
members. During this task only, the compatibility `GetMove(limits)` may call `Search(m_Board,
limits)` so Game still builds; mark it for deletion in Task 5. Retain `last_result_`,
`GetLastResult()`, and setter-style observer only where an unmigrated front-end still requires them;
Task 4 removes the UCI dependencies and Task 5 deletes the final compatibility surface.

- [ ] **Step 4: Migrate direct search/tactical callers to returned results and verify GREEN**

Update direct fixtures to construct `AIPerplexConfig`, call `Search(board, limits)`, and carry the
returned result instead of reading `GetLastResult()`. Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[search],[tactical],[fifty_move]"
```

Expected: all selected cases pass, including consecutive searches with different roots and
observers.

- [ ] **Step 5: Update progress and commit**

Document which inherited dependencies remain solely for Game compatibility and make Task 4 the
next step. Commit:

```powershell
git add StratEngine/AIPerplex.h StratEngine/AIPerplex.cpp StratEngine/ThreadData.h `
  StratChessTests/SearchTests.cpp StratChessTests/TacticalTestHelpers.h `
  StratChessTests/TacticalFullTests.cpp StratChessTests/FiftyMoveRuleTests.cpp 256-progress.md
git commit -m "refactor: establish concrete AIPerplex search API (#256)"
```

---

### Task 4: Give UCI concrete ownership and per-call observation

**Files:**
- Modify: `StratEngine/UCIHandler.h`
- Modify: `StratEngine/UCIHandler.cpp`
- Modify: `StratChessTests/UCITests.cpp`
- Modify: `StratEngine/AIPerplex.h`
- Modify: `StratEngine/AIPerplex.cpp`
- Modify: `256-progress.md`

**Interfaces:**
- Consumes: concrete `AIPerplexConfig` and `Search` from Task 3.
- Produces: `UciHandler::ai_` as `std::unique_ptr<AIPerplex>` and `eval_` as
  `std::unique_ptr<EvalComplex>`.
- Produces: one per-call iteration observer passed into the search thread; no observer registration
  state or UCI result-cache reads.

- [ ] **Step 1: Write failing concrete-lifecycle and observer-reset tests**

Update the fixture to access `handler.ai_` directly with no cast. Preserve the existing object
identity/TT-clearing test across `ucinewgame`. Add a back-to-back `go` case where the first command
has UCI iteration output and the second produces only its own iterations, catching a stale observer.
Assert final time/node information from captured UCI output rather than `GetLastResult()`.

- [ ] **Step 2: Run UCI tests and verify RED**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[uci]"
```

Expected: compilation fails while `ai_` is `unique_ptr<PlayerAiBase>` and `cmd_go` still registers a
stored observer.

- [ ] **Step 3: Implement concrete UCI ownership**

`init_ai()` constructs exactly once from UCI defaults and does not use the player factory.
`cmd_go()` captures the per-command observer and calls:

```cpp
const SearchResult result = ai_->Search(board_, limits, std::move(observer));
```

Use `result.elapsed` and returned node fields for the final info line. `stop_and_join()` calls
`ai_->Stop()`. `cmd_ucinewgame()` keeps the same pointer and calls `StartNewGame()` after join. Hash
and thread options use the concrete API. Construct `EvalComplex` directly and call `Breakdown()`
without a cast. Delete `SetIterationObserver` and any result-cache dependency once the UCI tests no
longer use them.

- [ ] **Step 4: Run UCI and search tests and verify GREEN**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[uci],[search]"
```

Expected: all cases pass; the lifecycle test observes the same object address across
`ucinewgame`, and no UCI production downcast remains.

- [ ] **Step 5: Update progress and commit**

Record concrete ownership and lifecycle evidence, identify the Game/player adapter migration as the
next step, then commit:

```powershell
git add StratEngine/UCIHandler.h StratEngine/UCIHandler.cpp StratEngine/AIPerplex.h `
  StratEngine/AIPerplex.cpp StratChessTests/UCITests.cpp 256-progress.md
git commit -m "refactor: give UCI concrete search ownership (#256)"
```

---

### Task 5: Add SearchPlayer and the config-aware player factory

**Files:**
- Create: `StratEngine/SearchPlayer.h`
- Create: `StratEngine/SearchPlayer.cpp`
- Create: `StratEngine/PlayerFactory.h`
- Create: `StratEngine/PlayerFactory.cpp`
- Modify: `StratEngine/AIPerplex.h`
- Modify: `StratEngine/AIPerplex.cpp`
- Modify: `StratEngine/IPlayer.h`
- Modify: `StratEngine/PlayerBase.h`
- Modify: `StratEngine/PlayerBase.cpp`
- Modify: `StratEngine/Game.h`
- Modify: `StratEngine/Game.cpp`
- Modify: `StratEngine/Config.h`
- Modify: `StratEngine/Tests/TacticalTestRunner.cpp`
- Modify: `StratChessTests/PlayerHumanTests.cpp`
- Modify: `StratChessTests/SearchTests.cpp`
- Modify: `StratChessTests/GameLoopTests.cpp`
- Modify: `256-progress.md`

**Interfaces:**
- Consumes: fully owned `AIPerplex` service and concrete UCI from Tasks 3-4.
- Produces: `SearchPlayer final : public IPlayer` with `Board&`, by-value `AIPerplex`, immutable
  description, fixed type string, and `IsHuman() == false`.
- Produces: `PlayerCreationOptions { bool verbose_search_logging{false}; }` and
  `CreatePlayer(const Config::PlayerConfig&, Board&, PlayerCreationOptions = {})` returning
  `std::unique_ptr<IPlayer>`.

- [ ] **Step 1: Write failing adapter and factory tests**

Add behavior tests that:

```cpp
auto player = CreatePlayer(config, board, {.verbose_search_logging = false});
const SearchResult first = player->GetMove(SearchLimits{.depth = 1});
board.MakeMove(known_legal_move);
const SearchResult second = player->GetMove(SearchLimits{.depth = 1});
CHECK(first.best_move != Move{});
CHECK(second.best_move != Move{});
```

Use an actual legal board transition and assert terminal/move behavior that would fail if the
adapter searched a retained copy. Add factory cases for human and one legacy AI, plus an AIPerplex
case proving evaluator, tuning, threads, and logging options are applied before return through their
observable search behavior or existing friend fixture. Add a lifecycle case proving the initial
new-game notification occurs before the player is returned.

- [ ] **Step 2: Run factory, player, and game tests and verify RED**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[player],[search_player],[game_loop]"
```

Expected: compilation fails because `SearchPlayer`, `PlayerCreationOptions`, and the free factory do
not exist.

- [ ] **Step 3: Implement the adapter and remove AIPerplex inheritance**

Use this ownership shape:

```cpp
class SearchPlayer final : public IPlayer {
  public:
	SearchPlayer(Board& board, AIPerplexConfig config, std::string description);
	SearchResult GetMove(const SearchLimits& limits) override;
	const char* GetType() const override;
	std::string getDescription() const override;
	bool IsHuman() const override { return false; }

  private:
	Board& board_;
	AIPerplex search_;
	std::string description_;
};
```

`GetMove` delegates to `search_.Search(board_, limits)`. The inherited event remains inert as it is
for AIPerplex today. Remove `PlayerAiBase`/`PlayerBase`/`IPlayer` inheritance and compatibility
`GetMove` from AIPerplex. Delete all remaining inherited-player state use; the result cache was
removed when UCI stopped depending on it in Task 4.

- [ ] **Step 4: Implement the single player composition root and migrate callers**

The free factory maps `Config::PlayerConfig` and `PlayerCreationOptions` into concrete constructors,
applies the initial `StartNewGame()` while AI types are known, and never configures through
`IPlayer`. Delete `PlayerBase::Create`. Update the `Config::PlayerConfig::depth` comment to name the
new factory rather than the deleted static one.

Game passes verbose logging enabled, subscribes to the generic event after construction, and stores
the returned `unique_ptr<IPlayer>`. Tactical tooling constructs `AIPerplexConfig` directly with its
requested thread count. Human and legacy tests use the free factory.

Remove `GetBestScore()` from `IPlayer`; make `PlayerBase::GetBestScore()` non-virtual and retain it
only for the legacy aspiration seed. Leave `SetEvalEngine` only if a remaining legacy production
caller requires it; the new factory must not call it after type erasure.

- [ ] **Step 5: Run focused and fast suites and verify GREEN**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe "[player],[search_player],[game_loop],[search],[tactical]"
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PreCommit.ps1
```

Expected: all selected and non-slow tests pass; repository search finds no
`PlayerBase::Create`, `dynamic_cast<AIPerplex*>`, or `dynamic_cast<PlayerAiBase*>` in production.

- [ ] **Step 6: Update progress and commit**

Record the final ownership graph, any source-compatibility churn, and Task 6 validation/doc work as
next. Commit:

```powershell
git add StratEngine/SearchPlayer.h StratEngine/SearchPlayer.cpp StratEngine/PlayerFactory.h `
  StratEngine/PlayerFactory.cpp StratEngine/AIPerplex.h StratEngine/AIPerplex.cpp `
  StratEngine/IPlayer.h StratEngine/PlayerBase.h StratEngine/PlayerBase.cpp StratEngine/Game.h `
  StratEngine/Game.cpp StratEngine/Config.h StratEngine/Tests/TacticalTestRunner.cpp `
  StratChessTests/PlayerHumanTests.cpp StratChessTests/SearchTests.cpp `
  StratChessTests/GameLoopTests.cpp 256-progress.md
git commit -m "refactor: isolate search behind SearchPlayer (#256)"
```

---

### Task 6: Remove stale surfaces and update durable documentation

**Files:**
- Modify: `StratEngine/AIPerplex.h`
- Modify: `StratEngine/AIPerplex.cpp`
- Modify: `StratEngine/PlayerAI.h`
- Modify: `StratEngine/PlayerAI.cpp`
- Modify: `StratChessTests/UCITests.cpp`
- Modify: `StratChessTests/SearchTests.cpp`
- Modify: `Docs/Engine-Readme.md`
- Modify: `Docs/TestDesign.md`
- Modify: `Docs/Changelog.md`
- Modify: `CLAUDE.md`
- Modify: `256-progress.md`

**Interfaces:**
- Consumes: final ownership and factory boundaries from Tasks 1-5.
- Produces: no obsolete observer/result/player capability state, no production implementation
  downcasts, and durable documentation matching the implementation.

- [ ] **Step 1: Add or tighten behavior tests for every stale surface being removed**

Replace source-shape assertions with behavior assertions: returned `SearchResult` remains usable
after a later search, `StartNewGame()` clears TT/per-game heuristic state without a result cache,
legacy aspiration behavior still uses its local score, and UCI `eval` returns the same breakdown.
Run the relevant tests before deletion and confirm the new assertion fails where the authoritative
returned-value behavior is not yet wired.

- [ ] **Step 2: Delete transitional state and scan production**

Delete any remaining stored iteration observer, AIPerplex player metadata, duplicate default/control
fields, and unused compatibility forwarding. Verify `last_result_` stayed deleted after Task 4. Run:

```powershell
rg "dynamic_cast<(AIPerplex|PlayerAiBase|EvalComplex)\*|GetLastResult|SetIterationObserver" `
  StratEngine StratChessEvolved
```

Expected: no production matches. Test-only private-access helpers may name concrete types but must
not downcast from an interface.

- [ ] **Step 3: Update durable documentation**

Document the concrete search service, required by-value adapter, deferred interface, config-aware
factory, shared control, returned telemetry, Game-owned totals, and timed validation requirements.
Update `CLAUDE.md` Key Source Facts and the changelog without claiming Elo or nps improvement.

- [ ] **Step 4: Run the full test suite and verify GREEN**

Run:

```powershell
.\build.ps1 tests
.\build\windows-clang-cl\StratChessTests.exe
```

Expected: every test passes; do not gate on a fixed assertion or case count.

- [ ] **Step 5: Update progress and commit**

Record the clean stale-surface scan, full-suite evidence, and Task 7 validation commands as the only
remaining work. Commit:

```powershell
git add StratEngine/AIPerplex.h StratEngine/AIPerplex.cpp StratEngine/PlayerAI.h `
  StratEngine/PlayerAI.cpp StratChessTests/UCITests.cpp StratChessTests/SearchTests.cpp `
  Docs/Engine-Readme.md Docs/TestDesign.md Docs/Changelog.md CLAUDE.md 256-progress.md
git commit -m "docs: record production search boundary (#256)"
```

---

### Task 7: Prove behavior, timing, and operational neutrality

**Files:**
- Modify: `Docs/Changelog.md` only if measured evidence is recorded there
- Modify: `Docs/EloLog.md` through the smoke script's normal append behavior
- Modify: `256-progress.md`

**Interfaces:**
- Consumes: the complete implementation and the explicit merge-base reference build.
- Produces: fixed-depth equivalence, timed-UCI operational evidence, benchmark comparison, full
  validation, self-play, and search-focused review evidence.

- [ ] **Step 1: Run exact fixed-depth equivalence**

Build the shipping clang-cl Release executable, then run:

```powershell
.\StratChessEvolved\Scripts\Compare-SearchEquivalence.ps1 `
  -After .\build\windows-clang-cl\StratChessEvolved.exe -BaselineRef origin/main
```

Expected: zero differences for the built-in six positions at depth 12 and `Threads=1`.

- [ ] **Step 2: Run timed and node-budget UCI probes**

Against merge-base and candidate executables, exercise `go movetime`,
`go wtime/btime/winc/binc`, and `go nodes` on fixed positions. Each command must emit a completed
depth and `bestmove`, with no stall/time loss and only the documented 1024-entry polling overshoot.

- [ ] **Step 3: Run operational smoke and benchmark comparison**

Run `Run-EloMatch.ps1 -Smoke` with an explicit merge-base `-ReferenceExe` and SHA
`-ReferenceTag`; retain its automatic 20-game `10+0.1` row only as operational evidence. Run
`Run-Bench.ps1` repeatedly with the same clang-cl build, depth 12, and `Threads=1`; investigate any
tree difference or nps shift outside run-to-run noise rather than claiming a gain.

- [ ] **Step 4: Run repository validation and self-play gates**

Run:

```powershell
.\build.ps1 extended-tests
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1 -Force
```

Run the existing UCI race probe/TSan Lazy-SMP scenario. Run headless AIPerplex-vs-AIPerplex and
AIAgent-vs-AIAgent self-play from `StratChessEvolved/`, preserving the starting-position FEN and
confirming every move logs a complete result without crash or stall.

- [ ] **Step 5: Dispatch search review and broad branch review**

Dispatch the required `search-reviewer` against all changes to abort/limit access and AIPerplex
integration. Resolve Critical/Important findings through the task review loop. Then request a broad
whole-branch review against the approved design and this plan.

- [ ] **Step 6: Finalize the progress ledger and commit measured evidence**

Update `256-progress.md` with every command and result, remaining trade-offs, and the precise merge
or PR handoff. If validation scripts changed tracked evidence, commit only those files plus progress:

```powershell
git add Docs/Changelog.md Docs/EloLog.md 256-progress.md
git commit -m "test: validate production search boundary (#256)"
```

If `Docs/Changelog.md` or `Docs/EloLog.md` did not change, commit only `256-progress.md` with the same
message. The final park point must state that implementation and validation are complete but no push,
merge, or PR has been performed.
