# AIPerplex production-search boundary — Design

**Issue:** #256

## Goal

Stop the shipping search from inheriting the legacy player implementation. `AIPerplex` should expose
one production-search boundary, own its search control and evaluator explicitly, and return all
per-call output through `SearchResult`. The change is worthwhile only if it removes concrete
downcasts and hidden state from production callers while preserving the exact search tree and hot
path.

Issue #256 predates the August 19 `GameInfo` removal. Current `main` already carries the terminal
verdict in `SearchResult::game_state`; this design therefore does **not** preserve or replace a
mutable `GameInfo&` channel. It builds on the newer value-returning contract.

## Scope

**This change will:**

- Introduce a narrow production interface (`ISearchEngine`) whose synchronous search operation takes
  a root `Board`, per-call `SearchLimits`, and an optional per-iteration observer, and returns a
  `SearchResult`.
- Move the runtime capabilities current production owners actually use onto that boundary: stop,
  new-game reset, thread count, and hash configuration.
- Make `AIPerplex` implement that interface directly, with no `PlayerAiBase` or `PlayerBase`
  inheritance and no retained reference to a caller-owned board.
- Replace the inherited timing/abort/limit helpers with a composed `SearchControl` owned by
  `AIPerplex`; keep the recursive search functions and their `ThreadData`/TT inputs otherwise
  unchanged.
- Inject a non-null evaluator and immutable default depth/time limits at construction, rather than
  constructing an invalid search and requiring a later `SetEvalEngine()` call.
- Add a game-mode `SearchPlayer` adapter that implements `IPlayer`, holds the game board reference,
  and delegates one move request to an `ISearchEngine`.
- Let `UciHandler` own `std::unique_ptr<ISearchEngine>` directly. Remove its `PlayerAiBase` and
  `AIPerplex` downcasts; pass the iteration observer as part of the search call rather than installing
  mutable callback state before starting the thread.
- Move `IterationInfo` and `HashConfigurationResult` out of the legacy base/concrete header and into
  the production boundary.
- Remove `AIPerplex::last_result_`/`GetLastResult()`. The value returned by `Search()` is the sole
  authoritative completed result.
- Move production performance accounting out of the legacy process-global statics. The result will
  carry the elapsed duration needed by the game-mode adapter to maintain per-player statistics and
  perf logging.
- Make the UCI `eval` dependency honest by storing the complex diagnostic evaluator as the type the
  command requires, instead of storing `EvalManager` and downcasting on every command. This is a
  concrete composition-root choice, not a second speculative interface.
- Update focused tests and documentation for the new seam, including deterministic fake-search
  tests at the UCI boundary and real-search integration coverage.
- Migrate the tactical runner to the configured production-search/player factory, removing its
  `PlayerAiBase` downcast while preserving its existing thread-count option.

**This change will not:**

- Change `pvs()`, quiescence, evaluation, move ordering, TT replacement, pruning, tuning values,
  node-count semantics, or Lazy SMP's main-is-authoritative policy.
- Delete, archive, or behaviorally change `AIBasic`, `AIAgent`, or `ABIterative`; they remain on
  `PlayerAiBase` until a separately reviewed legacy/tooling decision.
- Add a second production search algorithm. The immediate second implementation of the interface is
  a deterministic test double; a strength-lab A/B implementation becomes possible but is not built
  here.
- Create a reusable compiled engine library or change CMake component ownership (#83/#251).
- Change the numeric player types or the `game_settings.json` schema.
- Change UCI commands, option names, output ordering, or measurement-contract version.
- Claim an Elo or nps gain. This is a behavior-preserving boundary change; its performance promise is
  no regression outside measurement noise.

## Decisions

### D1: Use an interface plus a game adapter, not another shared search base

The production seam is a small interface implemented by `AIPerplex`; game mode adapts it to the
existing player contract. A representative shape is:

```cpp
class ISearchEngine {
  public:
	virtual ~ISearchEngine() = default;
	virtual SearchResult Search(const Board& root, const SearchLimits& limits,
	                            IterationObserver observer = {}) = 0;
	virtual void Stop() noexcept = 0;
	virtual void StartNewGame() = 0;
	virtual void SetThreads(unsigned threads) noexcept = 0;
	virtual HashConfigurationResult SetHash(unsigned megabytes) noexcept = 0;
};
```

These methods are one coherent application boundary: search plus the lifecycle and UCI controls
that both production owners already exercise. `Stop()` is the only method callable concurrently
with `Search()`; configuration and `StartNewGame()` retain today's no-search-in-flight precondition.

The alternatives are real, but weaker:

| Approach | Upside | Cost / reason rejected |
|---|---|---|
| **Interface + `SearchPlayer` adapter (chosen)** | Removes production inheritance and downcasts; gives UCI a deterministic fake seam; permits future A/B search in one process | Adds an interface, adapter, and composition factory; configuration must be divided carefully between generic runtime controls and AIPerplex tuning |
| Split `PlayerAiBase` into modern and legacy bases | Smallest diff and least call-site churn | Keeps production coupled to a protected inheritance surface, leaves UCI/Game casts and invalid post-construction setup, and gives tests no substitutable search boundary |
| Use concrete `AIPerplex` directly everywhere | Honest with one shipping implementation and fewer types | Couples both front ends to the concrete search, makes fake UCI lifecycle tests impossible, and moves rather than removes the downcast problem |
| Retire legacy players now and make `IPlayer` the search interface | Cleanest final type graph | Changes supported game configuration and historical comparison tools; that product decision is not required to fix #256 |

The interface is not justified by a hypothetical second engine alone. It is used immediately by two
production adapters and by a deterministic test double, so it has a present consumer and a present
testability payoff.

### D2: Pass the root position per call; do not retain `Board&` in the search

`Search(const Board&, ...)` copies the supplied root into `td_.board` and helper `ThreadData` exactly
where `GetMove()` copies `m_Board` today. `SearchPlayer` owns the game-facing board reference because
that is player integration state; `UciHandler` supplies its board directly.

Rejected: keep `Board&` in the `AIPerplex` constructor. It preserves an unnecessary lifetime
constraint, makes one search object inseparable from one front end, and complicates isolated tests.
There is no additional board copy in the chosen design: the existing per-search copy merely takes
its source from the parameter instead of an inherited reference.

### D3: Compose search control; do not share it with legacy code through inheritance

`SearchControl` owns the `TimeManager`, resolved node limit, start time, and the operations now named
`ApplyLimits`, `StopRequested`, `NodeLimitReached`, and `IsAborted`. It is a value member of
`AIPerplex`; calls in `pvs()` and quiescence remain non-virtual and inlineable. The legacy
`PlayerAiBase` keeps its current implementation for the three old agents.

This deliberately tolerates a small amount of duplicated orchestration between modern and legacy
code. Extracting a new common base would recreate the coupling this issue removes. If a second
production search later needs identical control, both can compose the same `SearchControl` without
sharing object identity or protected state.

`SearchControl` does not own the TT, evaluator, `ThreadData`, tuning, or aggregate statistics. Those
have different lifetimes and concurrency rules and would turn a cohesive stop/limit component into a
new miscellaneous base by composition.

### D4: Construction produces a usable search object

An `AIPerplexConfig`/factory supplies the evaluator, default limits, thread count, hash budget, tuning,
and logging policy before the object is published. The evaluator is owned by `AIPerplex` and is never
null. Algorithm-specific tuning stays on the concrete config and does not enter `ISearchEngine`.

`SetThreads` and `SetHash` remain on the runtime boundary because they are advertised UCI options.
Rejected: put `SearchTuning& tuning()` on the interface. Other algorithms need not share AIPerplex's
heuristics, and doing so would rename concrete coupling as abstraction.

Creation code is the permitted place to know the concrete implementation. Game mode uses a player
factory that builds a configured `AIPerplex` and wraps it in `SearchPlayer`; UCI uses a production
search factory. Command handlers and the game loop do not include or cast to `AIPerplex`.

### D5: Returned values and per-call observers replace result and callback side channels

`SearchResult` is already sufficient for the move, score, completed depth, terminal verdict, node
split, and stability. It gains elapsed duration so game-mode perf logging does not need
`PlayerAiBase::StopTimerAndAdjustVars()` or its process-global totals. `SearchPlayer` may retain the
last score only to satisfy the legacy `IPlayer::GetBestScore()` compatibility method; no production
caller may use that cache as search telemetry.

The iteration observer is an argument whose lifetime is the synchronous `Search()` call. It is
invoked only by the authoritative main thread at the same accepted-iteration points as today. Empty
observers retain the current one-boolean-check cost per accepted iteration. This removes the
`SetIterationObserver(callback)` → start thread → clear callback temporal protocol and prevents a
stale observer from surviving an exceptional or early-return path.

Rejected: keep `GetLastResult()` as a convenience. It duplicates the authoritative return value,
requires reset semantics at `StartNewGame()`, and exists only because consumers previously lost
information behind the player abstraction.

### D6: The adapter is the only production object that is both a player and a search consumer

`SearchPlayer final : public PlayerBase` contains `std::unique_ptr<ISearchEngine>` and the game board
reference. Its `GetMove(limits)` calls `Search(board_, limits)`. It forwards new-game configuration
before play and preserves `IPlayer` description/event compatibility without forcing search code to
inherit those concerns.

Legacy players continue to be constructed as today. The numeric `AI_PERPLEX` selection now chooses
the adapter plus production search rather than a concrete subclass of `PlayerAiBase`.

This wrapper is an intentional extra hop: game mode is expressed in players, while UCI is expressed
in search commands. Treating those as the same abstraction is what made the old base accumulate two
generations of state.

### D7: Remove search-related downcasts; keep evaluation diagnostics concrete and explicit

After the change, production code contains no `dynamic_cast<PlayerAiBase*>` or
`dynamic_cast<AIPerplex*>`. UCI invokes search capabilities through `ISearchEngine`; game mode passes
configuration into its factory before type erasure.

`cmd_eval` is separate from search and always constructs the complex evaluator today. It will store
that concrete diagnostic type and call `Breakdown()` directly, deleting the cast and unreachable
fallback. Rejected: add an evaluator-capability interface for one fixed diagnostic command. That is
the same speculative-generalization problem #256's issue text warns against. If UCI later offers a
runtime evaluator choice, that new requirement is the trigger for a capability boundary.

Test-only casts used to inspect AIPerplex internals are replaced where they were compensating for the
missing public seam (threads, hash results, new-game identity, returned result). Fine-grained search
algorithm tests may continue to use the existing conditional friend; this design does not pretend
private `pvs()` helpers are a production interface.

### D8: Performance neutrality is the contract; maintainability and testability are the upside

The virtual search call occurs once per move. Game mode adds one adapter call, also once per move.
Neither lies in `pvs()`, quiescence, move generation, evaluation, or TT access. `SearchControl` is a
value member and preserves the existing atomic abort fast path and 1024-entry clock/node polling.

Expected effects:

| Dimension | Expected upside | Trade-off / limit |
|---|---|---|
| Performance | No extra board copy or production process-global stats mutation; unused legacy fields leave the search object; clearer ownership makes later profiling changes safer | Object size is immaterial beside the TT/`ThreadData`; inheritance removal itself does not make the tree faster; one extra game-adapter call is negligible but still measured; no nps gain is claimed |
| Maintainability | AIPerplex exposes only supported capabilities, is valid at construction, and cannot touch legacy fields; front ends no longer know its concrete type | More named types and factory wiring; legacy and modern control code coexist until legacy retirement |
| Testability | UCI lifecycle, stop, options, observer, terminal result, and error cases can use a deterministic fake; `SearchControl` can be tested without allocating a TT or searching | Real-search integration tests remain necessary because a fake cannot prove helper joins, node accounting, or search equivalence |
| Extensibility | A second search can be selected at the composition root for in-process A/B experiments | Interface evolution becomes deliberate work; algorithm-specific tuning must not leak onto it |

Any measurable nps regression is a defect to investigate, not an accepted price for cleaner types.

## Assumptions I cannot verify from the code

- **No external source consumer depends on `PlayerBase::Create(AI_PERPLEX, ...)`, `GetLastResult()`, or
  the concrete `AIPerplex` inheritance shape.** The repository is built as executables rather than a
  supported library, and all in-tree uses can be migrated, but GitHub cannot prove private forks do
  not compile these headers. Not verified; the release/README policy would need to declare a stable
  source API before this became a compatibility requirement.
- **The interface/adapter calls have no measurable cost.** The call frequency and hot-path placement
  are verified from code, but the optimizer and object-layout effect are not. Verify with repeated
  clang-cl `Run-Bench.ps1` runs at `Threads=1`, identical position/depth inputs, and first establish
  identical node/PV output.
- **Legacy player types remain useful enough to preserve.** Current config, tests, and factory still
  expose them, but no usage telemetry exists. This design chooses compatibility; deleting or moving
  them requires a separate product/tooling decision.

## Invariants

- At fixed depth and `Threads=1`, every accepted-iteration depth, score, node count, PV, final
  `SearchResult`, and best move is identical to `origin/main`.
- `SearchResult::game_state` remains the only terminal-verdict channel. Game mode consumes mate,
  stalemate, and human-exit results exactly as it does now; no search writes game outcome to `Board`.
- The main thread remains authoritative under Lazy SMP. All helpers observe the same root position,
  are stopped and joined before `Search()` returns, and only their node counts are aggregated.
- `Stop()` is safe from another thread. After the first stop/clock/node-limit latch, recursive stacks
  still collapse through the cheap atomic abort check.
- Node-limit and clock polling cadence and units do not change. Main and quiescence counts remain
  separate and sum to UCI `nodes`; measurement contract stays at 1.
- `ISearchEngine::StartNewGame()` clears the TT and main/helper per-game heuristic state without
  changing configured threads, hash allocation, evaluator, tuning, or defaults. A newly constructed
  `SearchPlayer` starts with an empty `IPlayer::GetBestScore()` compatibility cache.
- UCI emits the same option lines, accepted-iteration lines, final info lines, and `bestmove` ordering.
  The observer is cleared by lifetime, not by a later mutation.
- A configured search cannot exist with a null evaluator or a dangling board reference.
- Legacy AI fixed-depth best moves, terminal results, and limit behavior remain unchanged.
- Production code has no search/player implementation downcasts and no `AIPerplex` access to
  `PlayerAiBase` state.

## Validation

The implementation is an **Engine-tier, behavior-preserving search refactor**. Required evidence:

- Build and run the fast suite after each migration checkpoint; all **476 current test cases** must
  remain green. Do not gate on the exact assertion count: the randomized run can vary it by a small
  amount between seeds.
- Add interface-contract tests with a fake search for UCI: request translation, iteration callback,
  stop, thread/hash configuration, new-game lifecycle, terminal result, and final output ordering.
  Keep end-to-end tests using real `AIPerplex` for at least one fixed-depth search, node accounting,
  TT reset, and threaded stop/join.
- Add adapter tests proving it supplies the current board, returns the exact `SearchResult`, preserves
  terminal state, and keeps per-player stats isolated between two adapters.
- Add focused `SearchControl` tests for limit resolution, explicit stop, hard/soft clock behavior,
  node-limit latching, and per-call reset. Existing `TimeManager` and `resolve_limits` tests remain.
- Run `Compare-SearchEquivalence.ps1 -After .\\build\\windows-clang-cl\\StratChessEvolved.exe -BaselineRef origin/main`
  with the shipping clang-cl Release build, built-in six-position set, depth 12, `Threads=1`:
  **zero differences**.
- Run `Run-Bench.ps1` repeatedly before and after with the same clang-cl toolchain, built-in position
  set, depth 12, `Threads=1`. Compare wall time and total/main/qsearch nps only after equivalence is
  established; result must be within run-to-run noise.
- Run `build.ps1 extended-tests`, the full `Validate-PrePR.ps1 -Force` gate, and the existing UCI race
  probe/TSan Lazy-SMP scenario because stop, observer lifetime, and search ownership cross threads.
- Exercise game mode explicitly with `AI_PERPLEX` versus `AI_PERPLEX` and at least one preserved
  legacy AI configuration. UCI-only automation does not reach the `SearchPlayer` adapter.
- Dispatch `search-reviewer` after implementation because `AIPerplex` control flow and abort access
  change even though `pvs()`/quiescence semantics must not.

**No Elo match is needed** if the fixed-depth equivalence check is exact: the engine searches the
same trees and returns the same moves. Any equivalence difference invalidates that claim; stop the
refactor, explain the behavioral change, and apply the full search-change validation tier rather than
averaging it away with an Elo result.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Search is synchronous; only `Stop()` is concurrent; configuration/new-game require no in-flight search | `ISearchEngine` contract comments and UCI ownership comments |
| The board is supplied per call and copied once into each `ThreadData` root | `ISearchEngine::Search`, `AIPerplex::Search`, and `ThreadData::board` comments |
| `SearchControl` owns only limit/clock/abort state and stays a value member for the hot path | `SearchControl.h` class comment and `AIPerplex` member comment |
| `SearchResult` is authoritative; no `GetLastResult()` side channel | `SearchResult.h`, `CLAUDE.md` Key Source Facts, and migrated test names |
| Main-is-authoritative and helper-join-before-return remain unchanged | existing `AIPerplex::Search`/helper comments, updated for the new method name |
| Per-iteration observer lifetime is exactly one search call | `IterationObserver` and `ISearchEngine::Search` comments |
| AIPerplex tuning is construction-time concrete configuration, not a generic capability | `AIPerplexConfig` comment and `Config.h` mapping comment |
| Game mode uses a player adapter; UCI consumes search directly | `SearchPlayer.h`, `UCIHandler.h`, and `Docs/Engine-Readme.md` architecture/example |
| Production stats are per adapter/result, never process-global | stats owner comment, `Docs/Changelog.md`, and tests with two adapters |
| Measured equivalence and before/after benchmark | `Docs/Changelog.md` and PR body |
| New fake/real boundary coverage and game-adapter coverage | `Docs/TestDesign.md` |

Any approved decision that changes during implementation is added to this table with the reason and
its durable destination before the PR is opened.
