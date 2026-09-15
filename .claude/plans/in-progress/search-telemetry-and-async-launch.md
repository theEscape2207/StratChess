# Search telemetry regroup and async search launch — Design

**Issue:** none yet — came out of an architecture review (2026-09-14). One issue per PR once approved.

## Goal

Two refactorings in the most-edited area of the engine (`AIPerplex`, `UciHandler`), plus a deletion
that precedes them. None is meant to change a node, a move or measurable nps.

1. **Search telemetry is spread across five places.** A counter such as `singular_eligible` is
   declared in `ThreadData`, cleared in `ThreadData` and again for every helper in `Search()`, mirrored
   in `SearchResult`, hand-summed across threads in `Search()` (`AIPerplex.cpp:346-403`), and formatted
   inline in the `go` thread lambda (`UCIHandler.cpp:504-580`). Adding one counter means editing all
   five. `TTStats` already solved this for its own counters with `add()`/`record()`; the others never
   followed.
2. **The async search lifecycle is split across the `UciHandler`/`AIPerplex` seam.** UCI owns the
   thread and a `searching_` flag; `AIPerplex` owns `search_launch_active_`/`stop_pending_` and
   exposes the private `arm_uci_search_launch()`/`finish_search_launch()` to UCI through
   `friend class UciHandler`. The #245 fix (clear "searching" before `bestmove`) and the go-then-
   immediate-stop race are each only correct because of ordering on both sides. The thread reads
   `board_` by reference, safe only because `position` is refused mid-search and because of member
   declaration order in `UCIHandler.h`. Threads is stored twice (`configured_threads_`, `threads_`).
3. **The futility cost probe (#498) has done its job.** #498 and #87 are closed and both futility
   guards shipped; its counters are the largest telemetry block (7 fields, 5 arrays, ~45 lines of UCI
   formatting). Deleting it is cheaper than regrouping it.

## Scope

**This change will:**

- PR 1: delete the futility probe — `STRAT_FUTILITY_PROBE`, `kFutilityProbeCompiled`,
  `kFutilityProbeEvaluates`, every counter, the aggregation, the `info string futilityprobe` line and
  its CMake option.
- PR 2: introduce `SearchTelemetry` (D1–D4) and route every reset, cross-thread sum and `info string`
  line through it. Extend `Compare-SearchEquivalence.ps1` to compare every `info string` line from
  `go`, first, so the output invariant has a gate that can fail.
- PR 3: move the async launch into `AIPerplex` (D5–D8) and construct the service eagerly in
  `UciHandler`.

**This change will not:**

- Touch node counters (`nodes_searched`, `qnodes_searched`) — they are the measurement contract
  (`MEASUREMENT_CONTRACT`), not telemetry, and feed `assess_iteration_quality()`.
- Change any `++` site inside `pvs()`/`quiescence()` beyond the member path it names.
- Change any `info string` text or print condition.
- Give `SearchPlayer`/`Game` an async path. They keep synchronous `Search()`.
- Unify the two `AIPerplexConfig` construction routes (a separate review candidate).
- Change singular extensions' compile-time gating (#95 still open).

## Decisions

### D1: One struct per feature, held by one `SearchTelemetry` aggregate

`SingularStats` (4 counters), `FrontierFutilityStats` (1), `LateMovePruningStats` (1), and the
existing `TTStats` (11), each with `add(const T&)`. `SearchTelemetry { SingularStats singular;
FrontierFutilityStats frontier; LateMovePruningStats lmp; TTStats tt; }` offers `reset()`, `add(const SearchTelemetry&)` and the formatting of D3. `ThreadData` and
`SearchResult` each hold one `SearchTelemetry telemetry`.

`init_search()`, the helper setup loop, `reset_for_new_game()` and the post-join sum each become one
call. Both the helper reset and the post-join sum stay bounded by the per-search `threads` snapshot
(`i + 1 < threads`), never `helper_tds_.size()`: `helper_tds_` is not shrunk when Threads drops, so a
range-for would add stale helpers' frontier and LMP skips. Rejected: per-feature structs used directly (still
four call sites per struct), and one flat struct (no `compiled` flag per feature, see D2).

`frontier_futility_skips` and `late_move_pruning_skips` (#552) move in although both are live in the
shipping build: they are trigger counts (frontier's is read by `Run-Bench.ps1`), not part of the node
contract. Their resets stay per search and they still survive an abort.

**Member order is a layout requirement:** singular, frontier, lmp, tt. With singular (32 B) first, both
live counters keep today's offsets in `ThreadData` exactly (see Performance).

### D2: Each struct carries `static constexpr bool compiled`

`SingularStats::compiled = kSingularExtensionsCompiled`, `TTStats::compiled = kTTStatsCompiled`,
`FrontierFutilityStats::compiled = LateMovePruningStats::compiled = true` (LMP is live and always
counted; #552 removed its compile gate before merge). The aggregate's `reset`/`add`/format do `if constexpr
(T::compiled)` per member, so the gate appears once per feature instead of at every reset, sum and
print site. Write sites in `pvs()`/`quiescence()` keep their existing gates unchanged.

Consequence: the shipping build no longer clears the four singular counters per search (it cleared
them unconditionally before). Every singular write sits inside `if constexpr
(kSingularExtensionsCompiled)`, so the observable output is identical.

### D3: Each struct formats its own `info string` payload

Each struct appends zero or one payload (the text after `info string `) to a caller-supplied sink;
`UciHandler` adds the prefix and calls `send`. The counter names are an external contract —
`Run-Bench.ps1:205` parses `frontier skips`, `Scripts/measure_tt_capacity.py` parses `ttstats` — so
they live beside the counters they name. Print conditions move with them: singular prints when
`eligible != 0`, frontier when `skips != 0`, lmp when `skips != 0`, TT stats whenever compiled. Order stays singular,
frontier, lmp, ttstats.

Rejected: formatting kept in `UciHandler` as one function per struct — a new counter would still
touch two files, and the script-facing names would sit away from the counters.

### D4: Telemetry lives in the cold tail of `ThreadData`

`telemetry` replaces the singular, frontier, lmp and `tt_stats` members exactly where they are
today: after `excluded_move[MAX_PLY]`, after every hot member. No hot member's offset changes, and
with D1's order neither do the frontier and LMP counters'.

### D5: `AIPerplex` owns the async search thread

```cpp
using CompletionHandler = std::function<void(const SearchResult&)>;
void StartAsync(const Board& root, const SearchLimits& limits, IterationObserver observer,
                CompletionHandler on_done);
void Stop() noexcept;        // unchanged: non-blocking, callable from any thread
void StopAndWait();          // Stop(), then join the launch thread
void Wait();                 // join only
bool IsSearching() const noexcept;
```

`StartAsync` runs, in this **required** order: stop, join any previous launch, arm the handshake,
copy `root`, start a `std::jthread` running `Search()`. Join-before-arm is load-bearing: the handshake
flags are not per launch, and a previous `Search()` still unwinding would clear a fresh arm through its
`launch_guard`, losing a `stop` and misreporting `IsSearching()`. If anything after the arm throws
(`Board` copy, `std::function` move, `jthread` construction), a scope guard calls
`finish_search_launch()` so the engine does not refuse commands for the rest of the session.

`stop`, `quit`, `ucinewgame` and `perft` call `StopAndWait()` (all rely on the join today: tests assert
`bestmove` is out when `stop`/`quit` returns, `StartNewGame()` must not overlap a search, and perft
writes `std::cout` without `send_mutex`). The test fixture's `join_search()` becomes `Wait()`.
`~UciHandler` keeps an explicit `ai_->StopAndWait()` in its body, so no `UciHandler` member is
destroyed while a launch can still run; UCI's `on_done` captures nothing (`send` is static).

`IsSearching()` reads `search_launch_active_` under `stop_mutex_`. It is therefore also true during a
direct synchronous `Search()`, which is the accurate answer, and step 2 of D6 needs no new state.

In `AIPerplex`, the `std::jthread` and any other launch members are declared **after `helper_tds_`**,
so `td_`'s offset is unchanged and they are destroyed first; the destructor body still calls
`StopAndWait()` explicitly.

`arm_uci_search_launch`/`finish_search_launch` become internal, `friend class UciHandler` is deleted,
and `UciHandler` loses `search_thread_` and `searching_`.

Rejected: UCI keeps its thread and a public launch token replaces the friend. That renames the
coupling without removing it — the ordering still spans both modules. Rejected: a launch generation
tag instead of join-before-arm — more state for an ordering that is simple to state and test.

### D6: Completion order on the search thread

1. `Search()` returns (helpers joined, then `launch_guard` clears `search_launch_active_`).
2. `IsSearching()` is therefore false.
3. `on_done(result)` runs; UCI's handler sends the final `info` lines, then `bestmove`.

Step 2 before step 3 is the #245 invariant: by the time a client can read `bestmove`, the engine
accepts `position`. **One change from today:** `searching_` currently clears after the final `info`
lines, just before `bestmove`; here it clears before them. A non-compliant client that sends
`setoption Hash` before `bestmove` could see the `info string hash` reply land among the final lines.
Compliant clients never send it then. `SetHash` during `on_done` is safe because `hashfull` is taken
inside `Search()`.

**`on_done` uses only its argument and no `AIPerplex` member.** It must not call `StartAsync`, `Wait`,
`StopAndWait`, `SetHash`, `SetThreads`, `StartNewGame` or destroy the service: a join from the launch
thread would throw `resource_deadlock_would_occur`, and a member read would race `SetHash`. Checked in
Debug by a `thread_local bool` set around the `on_done` call and asserted in each of those entry
points — not by comparing against the `jthread`'s id, which would itself race a move-assignment in
`StartAsync`.

An exception escaping `Search()` on the launch thread terminates the process, as it does today. The
launch lambda must not catch and continue: that would leave a session with no `bestmove`.

`Stop()` stops a launch only when called after `StartAsync` returns; a `Stop()` before `StartAsync` is
reset by the arm.

### D7: The root is copied into the launch

One `Board` copy per `go` (≈6.5 KiB plus one heap allocation for `position_history_`). Removes the
by-reference read of `board_` and the declaration-order comment that protects it.

### D8: `UciHandler` constructs `AIPerplex` in its constructor

Deletes `configured_threads_`, `init_ai()`'s lazy path and every `if (!ai_)`. `run()` already builds
the service before the command loop, so a real engine process is unaffected. `UciHandler` gains an
optional `AIPerplexConfig` constructor parameter; the test fixture passes `hash_mb = 1`, so the ~30
fixture constructions that never built `ai_` do not each value-initialise a 192 MiB table (a cost that
lands hardest on the Debug `sanitize-linux` leg).

Test fallout, all in PR 3:
- `UCITests.cpp:681-706` asserts `ai_identity() == nullptr` before `go` — deleted.
- `configured_threads()` (`UCITests.cpp:628`, `UCITestFixture.h:48`) — replaced by the service's
  `threads_`.
- Refusal tests faking `searching_` via `set_searching` switch to a real `go infinite` … `StopAndWait`.
- "Both commands work normally once the search is over" (`UCITests.cpp:631-650`) pins flag vs
  `joinable()`. A `stop` would join and void it, so it becomes `go depth 1`, `wait_for("bestmove")`
  with no stop and no wait, then a `position` that must be accepted.
- Lazy-construction comments: `UCITests.cpp:187-223`, `UCIHandler.cpp:196-200,437-442,715-717`;
  `Docs/TestDesign.md:524-530`.

Rejected: a test-only setter on `AIPerplex` for the searching state — it would test a state the engine
can never reach by itself.

## Performance

The question for review: does any of this move nps, and if so by how much? Expected answer: no
measurable change. Grounds, per PR, verified in review against `origin/main` `30a5d46`:

**PR 1 — probe deletion.** The engine target defaults every probe, singular and TT-stats switch off
(`CMakeLists.txt:380,397,407`), and every such site in `pvs()`/`quiescence()` is a discarded `if
constexpr` or a constant-false conjunction, so the probe emits no code today. What changes is data:
`ThreadData` and `SearchResult` each lose 27 `int64_t` (216 B), all after `frontier_futility_skips`.
No member `pvs()`/`quiescence()` touches changes offset. Expected: `pvs`/`quiescence` codegen
identical apart from addresses.

**PR 2 — regroup.**
- *Per node:* the only telemetry writes live in the shipping build are `td.frontier_futility_skips++`
  and `td.late_move_pruning_skips++` (`AIPerplex.cpp:877,886`). They become
  `++td.telemetry.frontier.skips` / `++td.telemetry.lmp.skips` — same base-plus-offset access,
  **same offsets** given D1's order. clang-cl builds without strict aliasing, so the nested path gives
  the optimiser nothing new. Expected: codegen identical.
- *Per search:* reset and cross-thread sum are O(threads) and gated per feature; the shipping build
  does slightly less than today (D2). Formatting builds the same strings, once, after the clock stops.
  `SearchResult` is returned once per search and is not part of `IterationInfo`.

**PR 3 — async launch.**
- *Per node:* none. `pvs()`/`quiescence()` read only `control_.IsAborted()`, unchanged. `td_`'s offset
  in `AIPerplex` is unchanged (D5 member placement).
- *Per `go`:* microseconds — one ≈6.5 KiB `Board` copy plus one allocation, a `std::function` capturing
  nothing (fits the small-buffer optimisation), `jthread` stop state. All of it runs before the
  engine's clock starts (`AIPerplex.cpp:284`) and inside the GUI's clock, as today's thread start
  does. For scale, `init_search()` already copies the `Board` and a 64 KiB `PVTable` before the clock.
- *`stop`:* `Stop()` is unchanged (mutex, then atomic write); the join moves from
  `UciHandler::stop_and_join` into `StopAndWait()`, same work.
- *Test suite:* neutralised by D8's small-hash fixture config.

Because the expected per-`go` cost is microseconds, the latency checks in Validation only catch gross
mistakes (a lost `stop`, a ms-scale regression); they are not expected to resolve the change itself.

**Noise floor.** Code-layout shifts can move nps with no source-level cause. Equal node counts prove
search equivalence; identical disassembly (where obtainable) proves no codegen change; nps is judged by
the acceptance rule in Validation.

## Assumptions I cannot verify from the code

- **`llvm-objdump` can locate `pvs`/`quiescence` in the LTO-linked shipping exe.** Not tried. If it
  cannot, the nps acceptance rule alone decides PR 1 and PR 2.
- **Catch2 launch tests run under a local TSan build.** `sanitize-linux` (ASan+UBSan) detects no races
  and `tsan-linux` runs neither Catch2 nor `stop` (#243). Not tried; attempted once via the WSL method
  and the outcome, including "could not run", recorded in PR 3.
- **`Measure-UciLatency.ps1` can drive `go infinite` in `-Setup` and time `stop` → `bestmove`.**
  Checked against the script by review: mechanically yes, with no control-subtracted delta. Confirmed
  on first run.

## Invariants

- Identical node counts and best moves at `Threads=1` before and after each PR.
- Identical `go` output, excluding `time` fields, in the shipping build: every `info string` line
  present, absent and worded as before.
- A `stop` sent after `StartAsync` returns, before the launch thread runs, stops that search.
- `IsSearching()` is false before `on_done` runs, and so before `bestmove` is written (#245).
- `td_`'s offset within `AIPerplex` and every `ThreadData` member offset up to and including the
  frontier and LMP counters are unchanged.
- Precondition, restated: no `Search()` or `StartAsync()` overlaps another on the same service.

## Validation

Engine tier for all three PRs; no Elo match, because none changes a search decision and the
equivalence gate proves that directly.

**nps acceptance rule.** Baseline exe from `Compare-SearchEquivalence.ps1 -BaselineRef origin/main`'s
cache (same compiler and flags, clean tree). At least 5 `Run-Bench.ps1` passes per build
(`-Depth 12`, Threads=1), alternating baseline and candidate, idle machine. **Pass** when the change in
median aggregate nps is no larger than the larger of the two builds' own max−min spread (aggregate nps
is valid because trees are identical). Otherwise the per-position table goes to the owner before
merge.

| Evidence | Closes | PR |
|---|---|---|
| `Compare-SearchEquivalence.ps1 -After <exe>`, extended to keep every `info string` line | no node/move change; shipping-build output unchanged | all |
| Disassembly diff of `pvs`/`quiescence` (`llvm-objdump --no-show-raw-insn --no-leading-addr`) | no codegen change | 1, 2 |
| nps acceptance rule above | nps within noise | all |
| Catch2 output pins for the stats-enabled test build | text of `singular`/`frontier`/`lmp`/`ttstats` lines | 2 |
| Catch2: `StartAsync` at depth 1 with `on_done` recording `IsSearching()`, `REQUIRE` false; falsified once by swapping D6 steps 2 and 3 | #245 ordering | 3 |
| Catch2: stop-before-initialisation on `AIPerplex` directly (see below) | lost stop fails within a deadline instead of hanging | 3 |
| Catch2: search at Threads=3, then Threads=1; frontier and LMP counts equal the main thread's | stale-helper over-count (D1 bound) | 2 |
| Existing Catch2 UCI tests + reasoning from source; one local TSan attempt | handshake races (no automated race detector covers `stop`) | 3 |
| `Measure-UciLatency.ps1 -Command 'go depth 1' -CompletionMarker bestmove -Repetitions 200` | per-`go` fixed overhead (≥ ~0.1 ms visible) | 3 |
| `go movetime 200` (compliance) and `stop` probe with `-TimeoutMs 2000` | no lost stop, no ms-scale regression | 3 |
| `sanitize-linux` "Run fast tests" step duration before/after | D8 test cost stays neutral | 3 |

**Lost-stop test, bounded including teardown.** A `stop` dispatched through UCI joins, so a wait placed
after it is never reached when the stop is lost, and every later join (fixture, service destructor)
hangs too. The case therefore drives `AIPerplex` directly:
- A launch barrier — a hook the launch thread calls before `Search()`, a member that exists only under
  `STRAT_ENABLE_TEST_ACCESS` (zero shipping cost) — holds the worker; the test calls `StartAsync` with
  `go infinite` limits, then the non-blocking `Stop()`, then releases the barrier. This forces
  stop-before-initialisation instead of relying on scheduling.
- `on_done` fulfils a promise; the test waits with a 5 s deadline. On expiry it reports the failure and
  calls `std::_Exit(1)` before any assertion can unwind into a joining destructor.
- The CI job's `timeout-minutes` remains the outer bound.
- Falsified once by deleting the pending-stop delivery in `Search()` (`AIPerplex.cpp:286-289`): the
  test binary must exit non-zero within the deadline.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Telemetry vs node counters: which is which and why | `CONTEXT.md` → *Search telemetry* |
| D1 loop bound and member order as a layout requirement | comments on `SearchTelemetry` and its aggregation |
| D2 per-feature `compiled` flag | comment on `SearchTelemetry` |
| D3 counter names are parsed by scripts | comment on each formatter |
| D4 telemetry stays in the cold tail | existing "deliberately LAST" comment in `ThreadData.h` |
| D5/D6 launch order, completion order, `on_done` contract, precondition | `Docs/EngineContracts.md` → The search service, and the `StartAsync` declaration |
| D5 member placement after `helper_tds_` | comment at the launch members in `AIPerplex.h` |
| D8 small-hash fixture and replaced tests | `Docs/TestDesign.md:524-530` |
| Probe deletion, measured results per PR, rejected review findings | `Docs/Changelog.md` and PR bodies |
