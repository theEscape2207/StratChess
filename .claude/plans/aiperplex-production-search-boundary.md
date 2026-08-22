# AIPerplex production-search boundary — Design

**Issue:** #256

## Goal

Stop the shipping search from being a player or inheriting the legacy player implementation.
`AIPerplex` becomes a concrete search service with explicit position, configuration, control, and
result boundaries. Game mode keeps its `IPlayer` model through a thin `SearchPlayer` adapter; UCI
uses `AIPerplex` directly. The change is worthwhile only if it removes production downcasts, invalid
two-phase setup, hidden result state, and process-global statistics while preserving the exact search
tree, time-control behavior, and measurement output.

Issue #256 predates the August 19 `GameInfo` removal. Current `main` already carries the terminal
verdict in `SearchResult::game_state`; the issue's proposed step 1 is therefore much smaller now.
There is no mutable result back-channel to preserve or replace.

## Scope

**This change will:**

- Make `AIPerplex` a concrete, final search service with no `IPlayer`, `PlayerBase`, or
  `PlayerAiBase` inheritance. Its synchronous `Search()` takes a root `Board`, per-call
  `SearchLimits`, and an optional per-iteration observer, and returns a `SearchResult`.
- Add `SearchPlayer final : public IPlayer`. It holds the game board reference and an `AIPerplex` by
  value; `GetMove(limits)` delegates to `search_.Search(board_, limits)`.
- Keep the current `IPlayer::GetMove(const SearchLimits&)` signature. `SearchPlayer` is load-bearing:
  it is what permits board-per-call search without changing every player or retaining a board
  reference inside `AIPerplex`.
- Replace the inherited timing/abort/limit state with a composed `SearchControl`. Both `AIPerplex`
  and legacy `PlayerAiBase` hold it as a value member, so limit resolution and timer arming have one
  implementation without a new shared base.
- Inject a non-null evaluator, default depth/time limits, hash budget, threads, tuning, and logging
  policy through `AIPerplexConfig` at construction. A published search object is immediately usable.
- Replace `PlayerBase::Create(type, depth, board)` with one config-aware free factory,
  `CreatePlayer(const Config::PlayerConfig&, Board&, PlayerCreationOptions) ->
  std::unique_ptr<IPlayer>`. `PlayerCreationOptions` carries front-end policy that is not JSON player
  data, initially only verbose search logging. For `AI_PERPLEX` the factory builds the complete
  `AIPerplexConfig`, invokes the initial new-game reset, and returns a configured `SearchPlayer`; no
  configuration occurs after type erasure. Human and legacy creation move through the same factory.
- Migrate the in-tree direct-construction sites in `PlayerHumanTests.cpp`, both fixtures in
  `SearchTests.cpp`, `TacticalTestHelpers.h`, and `TacticalTestRunner.cpp`. The tactical runner passes
  threads at construction, deleting its `PlayerAiBase` downcast.
- Let `UciHandler` own `std::unique_ptr<AIPerplex>` directly. Its one-time `init_ai()` construction,
  persistence across `ucinewgame`, and `StartNewGame()` reset lifecycle remain unchanged.
- Move the iteration observer into the `Search()` call. Remove the register-before-spawn / clear-in-
  lambda protocol and `SetIterationObserver()` state.
- Remove `AIPerplex::last_result_`/`GetLastResult()`. The value returned by `Search()` is the sole
  authoritative completed result.
- Remove `GetBestScore()` from `IPlayer`. Keep a non-virtual `PlayerBase::GetBestScore()` for the
  legacy algorithms that use `_bestScore` internally as an aspiration seed; `SearchPlayer` carries
  no compatibility score cache.
- Move completed-search elapsed time into `SearchResult`. Legacy agents populate their unsplit node
  count in `nodes_searched` with `qnodes_searched == 0`; AIPerplex retains its existing main/q split.
- Move cumulative perf accounting to `Game`, which already owns the only logger that writes
  `SimplePerfStats.txt`. Preserve all six columns and the last three columns' combined-both-players
  semantics; totals reset with a new `Game` instead of living for the process lifetime.
- Make the UCI `eval` dependency honest by storing `EvalComplex`, the diagnostic evaluator the
  command always constructs and requires, rather than storing `EvalManager` and downcasting.
- Remove every production `dynamic_cast<PlayerAiBase*>`, `dynamic_cast<AIPerplex*>`, and
  `dynamic_cast<EvalComplex*>` in `Game.cpp`, `UCIHandler.cpp`, and `TacticalTestRunner.cpp`.

**This change will not:**

- Introduce `ISearchEngine`, a generic search factory, or fake-search UCI tests. Those form an optional,
  independently reviewable follow-up whose justification must be demonstrated by the tests it adds.
- Change `IPlayer::GetMove` to accept a board.
- Change `pvs()`, quiescence, evaluation, move ordering, TT replacement, pruning, tuning values,
  node-count semantics for AIPerplex, or Lazy SMP's main-is-authoritative policy.
- Delete, archive, or behaviorally redesign `AIBasic`, `AIAgent`, or `ABIterative`.
- Add a second search algorithm or an in-process A/B strength harness. The existing strength lab
  compares two binaries, and nothing in #256 requires two algorithms in one build.
- Create a reusable compiled engine library or change CMake component ownership (#83/#251).
- Change numeric player types, `game_settings.json`, UCI commands/options/output ordering,
  `SimplePerfStats.txt` columns, or the UCI measurement-contract version.
- Claim an Elo or nps gain. This is a behavior-preserving boundary change.

## Decisions

### D1: A concrete search service plus a load-bearing player adapter; no search interface

The first and possibly final architecture is:

```cpp
class AIPerplex final {
  public:
	SearchResult Search(const Board& root, const SearchLimits& limits,
	                    IterationObserver observer = {});
	void Stop() noexcept;
	void StartNewGame();
	void SetThreads(unsigned threads) noexcept;
	HashConfigurationResult SetHash(unsigned megabytes) noexcept;
};

class SearchPlayer final : public IPlayer {
	Board& board_;
	AIPerplex search_;
	std::string description_;

	SearchResult GetMove(const SearchLimits& limits) override
	{
		return search_.Search(board_, limits);
	}
	const char* GetType() const noexcept override;
	std::string getDescription() const override { return description_; }
	bool IsHuman() const noexcept override { return false; }
};
```

`SearchPlayer` exists because game mode calls players without passing a board, while a search should
receive the position for each call. It is not justified by a hypothetical second search and is not
optional while D2 and the current `IPlayer` contract both hold.

It holds `AIPerplex` by value. The outer `SearchPlayer` is already heap-allocated by the player
factory, `AIPerplex` is non-movable, and valid-at-construction removes any nullable state. A
`unique_ptr<AIPerplex>` member would add an allocation, pointer hop, and null state without adding a
lifetime or substitution capability.

The adapter also owns the remaining player-only presentation contract. It returns the existing
`GetType()` text, builds the existing depth/evaluator description once from creation config, and
reports `IsHuman() == false`. The inherited player event remains inert, matching current
`AIPerplex`, which never fires it. None of that metadata or event surface leaks back onto the search
service.

Rejected alternatives:

| Approach | Why rejected for this change |
|---|---|
| `AIPerplex : IPlayer` with board per call | `IPlayer::GetMove` has no board parameter. It requires changing every player/test double or retaining `Board&` and undoing D2. |
| `ISearchEngine` in this PR | Two concrete callers do not require an interface; the existing strength lab needs two binaries, not two in-process implementations. Deterministic fake UCI tests are a real possible justification, but should pay for the interface in their own diff. |
| Split modern and legacy search bases | Keeps production coupled to protected inheritance and encourages another miscellaneous base as responsibilities grow. |
| Use concrete `AIPerplex` in Game | Game stores `IPlayer`; special-casing each move through a cast would recreate the defect this issue removes. |

If a later PR demonstrates otherwise-unreachable deterministic UCI cases, extracting
`ISearchEngine` from this already-fixed concrete API is mechanical: `SearchPlayer::search_` widens
from `AIPerplex` to `std::unique_ptr<ISearchEngine>`, and UCI can do the same. Waiting does not make
that extraction more expensive; if it never happens, the concrete design loses nothing.

### D2: Pass the root position per call; do not retain `Board&` in the search

`Search(const Board&, ...)` copies the supplied root into `td_.board` and helper `ThreadData` exactly
where `GetMove()` copies `m_Board` today. `SearchPlayer` owns the game-facing board reference;
`UciHandler` supplies its board directly.

Rejected: keep `Board&` in the `AIPerplex` constructor. It preserves an unnecessary lifetime
constraint, makes one search object inseparable from one front end, and complicates isolated tests.
There is no additional board copy in the chosen design: the existing per-search copy merely takes
its source from the parameter instead of an inherited reference.

### D3: One composed `SearchControl` serves modern and legacy searches

`SearchControl` owns the `TimeManager`, default depth/time limits, resolved node limit, start time,
and the operations now named `ApplyLimits`, `StopRequested`, `NodeLimitReached`, and `IsAborted`.
It is a value member in both `AIPerplex` and `PlayerAiBase`.

Legacy methods may forward temporarily for a buildable migration, but final code contains one
implementation of limit resolution, timer arming, node-limit storage, elapsed-time capture, and stop
latching. Sharing a value component is composition, not the shared inheritance D1 rejects.

The component does not own TT, evaluator, `ThreadData`, tuning, iteration observers, or cumulative
statistics. Those have different lifetimes and concurrency rules.

The `threads_` snapshot at `AIPerplex::Search()` entry remains even though current UCI refuses
`setoption` during a search and the public precondition forbids concurrent configuration. It is a
cheap local consistency defense against a future or non-UCI caller violating that precondition and
prevents an out-of-bounds `helper_tds_` access if the configured count changes mid-call.

### D4: Configuration is complete before type erasure

`AIPerplexConfig` contains the evaluator choice, default depth/time, initial threads/hash, tuning, and
logging policy. Construction creates the evaluator and TT, clamps configuration, and leaves the
object ready to search; failures are reported during construction except `SetHash`, which retains its
current failure result and old-table guarantee for runtime replacement. Verbose logging becomes an
instance policy instead of the current static `AIPerplex::SetVerboseLogging(true)` side effect.

The free `CreatePlayer(config, board, options)` function is the composition root. It maps every
explicit player field plus default hash policy and the front-end logging option before constructing
`SearchPlayer`; it never constructs `IPlayer` and then asks what concrete type it got. Before return,
it performs the initial `StartNewGame()` notification for AI implementations while their concrete
type is still known. This deletes all three current construct-then-configure branches: verbose
logging, tuning, and threads/new-game setup. Game passes verbose logging enabled; tests default it
off. UCI does not use this player factory and builds its concrete `AIPerplexConfig` directly.

The change is deliberately visible in scope because it drives most call-site churn. Test fixtures
that need search internals construct `AIPerplex(AIPerplexConfig)` directly. Tactical tooling does the
same. Human and legacy tests migrate to `CreatePlayer(config, board)`. The old static factory is
deleted rather than retained as a second, under-specified construction path.

Algorithm-specific tuning stays on `AIPerplexConfig`; it does not leak onto `IPlayer` or a generic
runtime capability surface.

### D5: Returned values replace score, result, timing, and statistics side channels

`SearchResult` remains authoritative for move, score, completed depth, terminal verdict, main/q
nodes, and stability, and gains elapsed duration. `GetLastResult()` is deleted rather than retained
as a duplicate cache with new-game reset semantics.

`IPlayer::GetBestScore()` is deleted. No production caller reads it; `Game` already consumes
`SearchResult::best_score`, and `GameLoopTests` pins that the old side channel may be stale. The
non-virtual `PlayerBase::GetBestScore()` and `_bestScore` remain private to the legacy hierarchy,
where `AIAgent` uses them as an aspiration seed.

`Game` updates a per-game accumulator from each AI `SearchResult` and writes the existing six perf
columns. AIPerplex supplies `nodes_searched + qnodes_searched`; legacy agents supply their existing
combined `m_SearchCount` as an unsplit `nodes_searched` value. The last three columns therefore stay
combined across both players. Moving lifetime from process-global to one `Game` changes nothing in
the current executable, which constructs one game and explicitly has no play-again loop.

Rejected: per-`SearchPlayer` cumulative totals. That silently changes `SimplePerfStats.txt` from
combined-both-players totals to per-player totals and breaks historical interpretation of its last
three columns.

### D6: The iteration observer belongs to one synchronous search call

The observer is an argument whose lifetime is the `Search()` call. It is invoked only by the
authoritative main thread at the same accepted-iteration points as today. Empty observers retain the
current one-boolean-check cost per accepted iteration.

This deletes `SetIterationObserver(callback)` -> start thread -> clear callback inside the thread
lambda. A stale observer cannot survive an exceptional or early-return path because it is never
stored as cross-call configuration.

### D7: UCI owns the concrete search for its complete lifecycle

`UciHandler::ai_` becomes `std::unique_ptr<AIPerplex>`. `init_ai()` constructs it exactly once;
`cmd_ucinewgame()` continues to stop/join, call `StartNewGame()` on the same object, and reset the
board. Preserving identity matters because `StartNewGame()` clears TT and heuristic state while
retaining configured threads, hash allocation, evaluator, tuning, and defaults.

Search, stop, thread count, hash replacement, new-game reset, and iteration observation all use the
concrete API. The factory-to-`PlayerAiBase` cast and both `AIPerplex` casts disappear. UCI tests can
inspect the correctly typed member without runtime casts; this does not require a fake-search seam.

`cmd_eval` is separate from search and always constructs the complex evaluator today. It stores
`EvalComplex` and calls `Breakdown()` directly, deleting the cast and unreachable fallback. A generic
evaluator-diagnostics interface for one fixed command would be the same speculative layer rejected
in D1.

### D8: Performance neutrality is the contract; ownership and testability are the upside

UCI calls concrete `AIPerplex` directly. Game mode makes one virtual `IPlayer` call and one ordinary
adapter call per move; both are outside `pvs()`, quiescence, move generation, evaluation, and TT
access. `SearchControl` is a value member and preserves the atomic abort fast path and 1024-entry
clock/node polling.

| Dimension | Expected upside | Trade-off / limit |
|---|---|---|
| Performance | No extra board copy, no nullable search pointer in game mode, and no process-global stats mutation | Inheritance removal does not make the tree faster; the adapter call is once per move; no nps gain is claimed |
| Maintainability | AIPerplex has only search responsibilities, is valid at construction, and cannot touch legacy/player fields; all limit wiring has one composed implementation | `SearchPlayer` and `SearchControl` are new types; legacy and modern search algorithms still coexist |
| Testability | Search accepts an explicit board/evaluator/config; UCI's search member is correctly typed; timing/limit logic has focused component tests | Real-search integration remains necessary for clock expiry, helper joins, node accounting, and output ordering; a fake interface is deferred |
| Extensibility | The concrete API can be widened behind an interface later without changing search internals | No in-process algorithm substitution is promised or currently needed |

Any measurable nps regression or changed fixed-depth tree is a defect to investigate, not an
accepted price for cleaner types.

## Assumptions I cannot verify from the code

- **No external source consumer depends on `PlayerBase::Create(AI_PERPLEX, ...)`, `GetLastResult()`,
  `IPlayer::GetBestScore()`, or the concrete inheritance shape.** The repository builds executables,
  not a supported library, and all in-tree uses can be migrated. Private forks cannot be inspected;
  a stable source API would need an explicit project policy before this became a compatibility bar.
- **The adapter and object-layout changes have no measurable cost.** Call frequency and hot-path
  placement are verified from code; optimizer effects are not. Verify with matched clang-cl
  benchmarks only after fixed-depth equivalence is established.
- **The time-control wiring remains behaviorally neutral.** Unit tests can prove `SearchControl`, but
  not that every caller arms and observes it correctly. Timed UCI probes and a fastchess smoke run
  are required integration evidence.
- **Legacy player types remain useful enough to preserve.** Current config, tests, and factory expose
  them, but no usage telemetry exists. Deleting or moving them needs a separate product decision.

## Invariants

- At fixed depth and `Threads=1`, every accepted-iteration depth, score, node count, PV, final result,
  and best move is identical to `origin/main`.
- `SearchResult::game_state` remains the only terminal-verdict channel; no search writes an outcome
  to `Board`.
- `IPlayer::GetMove(const SearchLimits&)` is unchanged. Only `SearchPlayer` owns the game-facing
  board reference; `AIPerplex` owns none.
- The main thread remains authoritative under Lazy SMP. Every helper starts from the same supplied
  root, is stopped and joined before `Search()` returns, and contributes only node counts/TT work.
- `Stop()` is safe from another thread. After stop, clock, or node-limit latching, recursive stacks
  collapse through the same cheap atomic abort check.
- Limit precedence, default resolution, soft/hard clock behavior, node-limit polling cadence, and
  polling units are identical for production and legacy searches.
- The `threads_` snapshot is retained regardless of the no-concurrent-configuration precondition.
- `StartNewGame()` acts on the same UCI search object across games and preserves configured threads,
  hash allocation, evaluator, tuning, and defaults while clearing TT/per-game heuristic state.
- UCI emits the same options, iteration lines, final info lines, tree-node split, and `bestmove`
  ordering. Measurement contract stays at 1.
- `SimplePerfStats.txt` retains six columns and combined-both-players cumulative totals.
- A configured `AIPerplex` cannot exist with a null evaluator and retains no board reference.
  `SearchPlayer` retains Game's board under the same player-before-board lifetime contract as today.
- Legacy fixed-depth moves, terminal results, limits, and perf counts remain unchanged.
- Production code contains no player/search/evaluator implementation downcasts.

## Validation

The implementation is an **Engine-tier, behavior-preserving search and time-control refactor**.
Required evidence:

- Build and run the current fast suite after each migration checkpoint. All tests must pass; do not
  gate on an exact test-case or assertion count, both of which can change before implementation and
  the latter of which varies slightly with the randomized seed.
- Add focused `SearchControl` tests for default/explicit limit resolution, per-call reset, explicit
  stop, hard/soft clock behavior, node-limit latching, elapsed capture, and repeated calls. Exercise
  the same component through at least one legacy player and AIPerplex to prove wiring, not only the
  value type in isolation.
- Add construction/factory tests proving every `Config::PlayerConfig` AIPerplex field and each
  `PlayerCreationOptions` policy reaches the by-value search before type erasure, the initial
  new-game notification occurs before return, and no type/depth/board-only production construction
  path remains.
- Add adapter tests proving `SearchPlayer` supplies the current board, returns the exact result,
  propagates terminal state, and contains a non-nullable by-value search.
- Add UCI lifecycle tests against the concrete `ai_`: it is constructed once, survives
  `ucinewgame`, clears TT/per-game state, retains threads/hash, accepts a per-call observer, and
  returns authoritative telemetry without `GetLastResult()`.
- Run `Compare-SearchEquivalence.ps1 -After .\build\windows-clang-cl\StratChessEvolved.exe -BaselineRef origin/main`
  with the shipping clang-cl Release build, built-in six-position set, depth 12, `Threads=1`:
  **zero differences**.
- Run baseline and candidate UCI probes on fixed positions for `go movetime`, clock-based
  `go wtime/btime/winc/binc`, and `go nodes`. Each must return `bestmove`, report a completed depth,
  and stay within the same documented budget/polling-overshoot envelope. Wall-clock searches are not
  required to reach identical depths.
- Run `Run-EloMatch.ps1 -Smoke` with the candidate and an explicit merge-base `-ReferenceExe`, naming
  that build's commit SHA through `-ReferenceTag`. The 20-game default `10+0.1` run must have no
  illegal move, disconnect, stall, or time loss. Its automatic `Docs/EloLog.md` row is retained as
  operational evidence; its Elo estimate carries no strength conclusion.
- Run `Run-Bench.ps1` repeatedly before and after with the same clang-cl toolchain, built-in position
  set, depth 12, and `Threads=1`. Compare wall time and total/main/qsearch nps only after equivalence
  is established; results must stay within run-to-run noise.
- Run `build.ps1 extended-tests`, `Validate-PrePR.ps1 -Force`, and the existing UCI race probe/TSan
  Lazy-SMP scenario because stop, configuration, and search ownership cross threads.
- Exercise game mode with `AI_PERPLEX` versus `AI_PERPLEX` and at least one preserved legacy AI
  configuration. Verify the six perf columns and combined cumulative totals explicitly.
- Dispatch `search-reviewer` after implementation because abort/limit access changes even though
  recursive search semantics must not.

**No statistical Elo match or SPRT is needed** if fixed-depth equivalence is exact, timed probes obey
the same contracts, and the smoke run completes without time-control or protocol failures. The smoke
run is an integration gate, not strength evidence. Any tree difference or timed failure invalidates
the neutral-refactor claim and must be investigated before choosing a stronger validation tier.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| `SearchPlayer` is required by board-per-call plus the unchanged `IPlayer` signature | `SearchPlayer.h` class comment and `CreatePlayer` |
| `SearchPlayer` owns a non-nullable `AIPerplex` by value | member comment and construction test |
| AIPerplex is a search service, not a player; no `ISearchEngine` yet | `AIPerplex.h`, `Docs/Engine-Readme.md`, and `CLAUDE.md` Key Source Facts |
| The board is supplied per call and copied once into each `ThreadData` root | `AIPerplex::Search` and `ThreadData::board` comments |
| `SearchControl` is composed by modern and legacy searches; limit wiring has one implementation | `SearchControl.h` and `PlayerAI.h` member comments |
| `threads_` remains snapshotted despite the caller precondition | comment at `AIPerplex::Search()` entry |
| AIPerplex configuration is complete before type erasure | `AIPerplexConfig`, `PlayerCreationOptions`, `CreatePlayer`, and factory tests |
| UCI constructs one concrete search and resets that same object across `ucinewgame` | `UCIHandler::init_ai`, `cmd_ucinewgame`, and lifecycle tests |
| Result, elapsed, and score travel only through `SearchResult`; no result/score cache | `SearchResult.h`, `IPlayer.h`, and migrated test names |
| Per-iteration observer lifetime is exactly one search call | observer alias and `AIPerplex::Search` comments |
| Perf totals are Game-owned and retain combined-both-players column semantics | `Game.h/.cpp`, `Docs/TestDesign.md`, and `Docs/Changelog.md` |
| Timed UCI probe and smoke gates close the fixed-depth validation blind spot | `Docs/Workflow.md` or the PR test plan, if the commands are not automated |
| Measured equivalence and before/after benchmark | `Docs/Changelog.md` and PR body |
| Interface extraction is deferred until fake UCI tests demonstrate the need | follow-up issue only if that trigger occurs; otherwise this rationale dies with the plan |

Any approved decision that changes during implementation is added to this table with the reason and
its durable destination before the PR is opened.
