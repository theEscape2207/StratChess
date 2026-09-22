# One writer for the UCI protocol channel — Design

**Issue:** #605

## Goal

Every UCI protocol line leaves the engine through `UciHandler::send()`, a **static** function that
writes `std::cout` under a function-local static mutex (`StratEngine/UCIHandler.cpp:86-103`). Being
static is what makes the output path untestable from the outside: a test cannot supply a destination,
so it mutates the process-global `std::cout` instead. The two UCI test files do that at **85 sites**
— 51 `capture_cout(...)` calls and 34 direct `CoutRedirect` constructions. `capture_cout` centralises
51 *implementations*, but it is not an injection seam: every site still swaps `std::cout`'s
`rdbuf`, and production still hard-codes the destination.

That has two costs beyond ergonomics:

- **A test-only data race.** `CoutRedirect` swaps a global while another thread may be writing it.
  The fixture's `start_silent_search()` carries the scar — it exists to run "a real infinite search
  on the handler's board that **prints nothing**, so a test ... never swaps `std::cout`'s buffer
  under a writing thread" (`StratChessTests/UCITestFixture.h:55-56`). The test is shaped around the
  hazard because it cannot be removed.
- **A hole in the lock.** `Testing::Perft::divide` writes `std::cout` directly
  (`StratEngine/Tests/Perft.cpp:313,330,335`), bypassing `send()`'s mutex entirely. `cmd_perft` works
  around it by stopping and joining the search first (`StratEngine/UCIHandler.cpp:512-514`).

The fix is one writer object, owned by the handler and injectable at construction, through which all
protocol text passes.

## Scope

This lands as **two PRs**. PR1 introduces the seam and changes no test; PR2 spends it.

**PR1 will:**

- Add `StratEngine/UciWriter.h`: a `UciWriter` owning a line sink and the serialising mutex.
- Give `UciHandler` a `writer_` member, injectable through a new constructor overload, defaulting to
  a stdout writer. `send()` becomes a non-static member forwarding to it.
- Capture the writer **by value as a `shared_ptr`** into the `go` observer and completion callbacks.
- Add a sink parameter to `Testing::Perft::divide` and route `cmd_perft` through the writer, closing
  the lock bypass.
- Keep `cmd_perft`'s `StopAndWait()` and correct its comment (D4).

**PR2 will:**

- Add a synchronised capture sink to the test fixture, carrying `SynchronizedStringBuf`'s existing
  `wait_for(needle, timeout)` behaviour.
- Migrate the 85 redirection sites to it and delete `CoutRedirect` and `capture_cout`.
- Remove the "prints nothing" constraint from `start_silent_search()` if it is no longer load-bearing.

**Neither PR will:**

- Change one byte of emitted protocol text. `Run-Bench.ps1`, `Compare-SearchEquivalence.ps1`,
  `Measure-UciLatency.ps1` and fastchess all parse this stream.
- Give the writer a batch or transaction guarantee, or remove `cmd_perft`'s `StopAndWait()` (D4).
- Touch the non-UCI `std::cout` users: `TacticalTestRunner.cpp` (32 sites), the rest of `Perft.cpp`
  (28 sites outside `divide`), `Game.cpp` (3). Those are separate entry points, not the protocol
  channel.
- Touch search, evaluation or anything on the per-node path.
- Take on Candidates 1, 3 or 4 of the same review.

## Decisions

### D1: A `UciWriter` object holding a `std::function` line sink, not a virtual `Sink` hierarchy

The review proposes `UciWriter` plus `StdoutSink`/`CaptureSink` classes. Rejected the class
hierarchy: the tree already has the lighter idiom for exactly this shape —
`result.telemetry.append_info(sink)` hands lines to a supplied callable
(`StratEngine/UCIHandler.cpp:482`). Two sinks do not earn an inheritance tree.

```cpp
class UciWriter {
  public:
	// One complete protocol line, WITHOUT a trailing newline. The sink owns framing.
	using LineSink = std::function<void(std::string_view)>;

	UciWriter();                       // writes std::cout, flushing each line
	explicit UciWriter(LineSink sink);

	void send(std::string_view line);  // serialised; one line, indivisibly

	UciWriter(const UciWriter&) = delete;
	UciWriter& operator=(const UciWriter&) = delete;

  private:
	LineSink sink_;
	std::mutex mutex_;
};
```

The sink receives the line **without** its newline and appends its own, so the stdout sink emits
`line << '\n' << std::flush` — byte-identical to today (`UCIHandler.cpp:101-102`) — and a capture
sink accumulates exactly what a test reads today.

### D2: The mutex moves from a function-local static to a `UciWriter` member

Today one static mutex serialises every `send()` process-wide. Per-writer locking is equivalent in
production, where exactly one writer exists, and *more* correct in tests, where each fixture's writer
has its own sink and has no reason to contend with another fixture's. Rejected keeping a global
mutex: it would reintroduce cross-fixture coupling the seam exists to remove, and it makes parallel
test execution serialise on nothing.

### D3: The writer is a `std::shared_ptr<UciWriter>`, captured by value into the async callbacks

`send()` being static is precisely why the `go` observer and completion handler "capture nothing"
today, as the comment at `UCIHandler.cpp:459` states. An injected member sink replaces that with a
lifetime contract: the callbacks run on the search thread and must not touch a destroyed writer or a
dead capture sink.

`~UciHandler` already calls `StopAndWait()` (`UCIHandler.cpp:145`), so in-tree the callbacks are
joined before destruction. The `shared_ptr` copy is taken anyway, because it makes the guarantee
local and verifiable rather than dependent on a destructor ordering three call sites away, and it
costs one refcount bump per `go` — not per node, not per `info` line. The capture sink is owned *by*
the writer, so holding the writer alive holds the sink alive; that is the whole lifetime contract.

Rejected a raw `UciWriter&` or a bare pointer: both make the callback's validity a property of
someone else's destructor.

### D4: `cmd_perft` keeps `StopAndWait()`; only its comment changes

The review states that routing `Perft::divide` through the writer means the stop "is no longer an
output-serialisation device". That is half right. The comment at `UCIHandler.cpp:510-511` names
**two** reasons for the stop: `divide` writes without `send()`'s lock, *and* "a search's completion
handler is still printing until the join." PR1 removes the first. The second is transcript-level and
per-line atomicity does not address it — with the search still running, `info` and `bestmove` lines
interleave between divide lines even when every individual line is indivisible.

Rejected adding a batch/transaction API to the writer to make the stop removable: it would hold the
writer's lock across an unbounded loop, blocking the search thread instead of interleaving with it,
which is a worse property than the stop it replaces. `StopAndWait()` stays, and the comment is
reduced to the one reason that survives.

This is the design's single most important constraint, because the review's text invites the opposite
conclusion.

### D5: Constructor injection with a stdout default, not a setter

```cpp
explicit UciHandler(const AIPerplexConfig& config, std::shared_ptr<UciWriter> writer = nullptr);
```

`nullptr` means "make a stdout writer". `StratChessEvolved/StratChessEvolved.cpp:353` uses the
default constructor and is unchanged. Rejected a `SetWriter()`: a writer swappable after a search has
started is a lifetime hazard with no use case.

### D6: Only `Perft::divide` gains a sink parameter

`divide` becomes `static void divide(Board&, int depth, const UciWriter::LineSink& out)`. Its other
callers (`Perft::run` at `StratEngine/Tests/Perft.cpp:284` and the `perft` entry point at
`StratChessEvolved/StratChessEvolved.cpp:206`) pass a stdout sink. The `"\nTotal nodes: "` write
becomes two lines, `""` then `"Total nodes: N"`, so the blank line survives (I1).
Rejected converting `Perft.cpp`'s other 28 `std::cout` sites and `TacticalTestRunner.cpp`'s 32: they
belong to the `perft` and `tactical` entry points, which are not the protocol channel and have no
interleaving problem to solve.

### D7: PR1 changes no existing test

The default sink writes `std::cout`, so all 85 existing redirection sites keep working untouched.
That keeps PR1 a small, reviewable diff whose correctness is demonstrated by the *existing* suite
passing unchanged, and confines the 85-site churn to PR2 where it is mechanical. Rejected doing both
at once: it produces one large diff in which a behavioural regression and a migration typo look alike.

PR1's two *new* tests (Validation, I4 and I5) capture through an injected writer, never
`CoutRedirect`: the I4 test runs `perft` over a printing search, which is exactly the rdbuf-swap race
the seam removes. The fixture gains a constructor taking a `std::shared_ptr<UciWriter>` for them.

## Contract PR2 depends on

Durable across any reordering of the work:

| Name | Shape |
|---|---|
| `UciWriter::LineSink` | `std::function<void(std::string_view)>`; receives one complete protocol line **without** a trailing newline |
| `UciWriter::UciWriter(LineSink)` | takes ownership of the sink; `send()` is serialised per writer |
| `UciHandler(const AIPerplexConfig&, std::shared_ptr<UciWriter>)` | `nullptr` ⇒ stdout writer; `writer_` is never null afterwards |
| `Testing::Perft::divide(Board&, int, const UciWriter::LineSink&)` | every divide line goes to `out`, none to `std::cout` |

PR2 relies on exactly one behavioural guarantee: **after PR1, no code reachable from a UCI command
writes `std::cout` except through the injected writer.** That is what makes replacing the global
redirect with an injected capture sink a no-op for every assertion.

## Assumptions I cannot verify from the code

- **`std::function` indirection on the `info` path is not measurable.** Not verified. `info` lines
  are emitted once per completed iteration, so the call count is single-digit per search against a
  flush syscall per line — the indirection is far below the noise floor, and below what
  `Measure-UciLatency.ps1` can resolve, so it is argued rather than measured.
- **No external consumer depends on the flush *timing* rather than the content.** Not verified beyond
  the wire format being unchanged. The stdout sink flushes per line exactly as `send()` does today,
  so this is preserved by construction; there is no scenario in which it could differ.

## Invariants

- **I1** — The emitted protocol stream is byte-identical to `origin/main`'s for the same command
  sequence: same lines, same order, same per-line flush.
- **I2** — One protocol line is still written indivisibly with respect to every other line from any
  thread. This is the #237 stage 0 requirement; a torn line forfeits a game.
- **I3** — After PR1, the only `std::cout` write reachable from a UCI command is the stdout sink's.
  `Perft::divide`'s lock bypass is gone.
- **I4** — `cmd_perft` still emits its divide transcript with no search output interleaved into it.
- **I5** — No callback invoked on the search thread touches a destroyed writer or a dead sink, for
  any interleaving of search completion and handler destruction.
- **I6** — Search behaviour is unchanged: identical nodes and best moves at `Threads=1`.

## Validation

Engine tier. No per-node work is added — `send()` is called per protocol line, not per node — so no
bench pass and no Elo match. The risk here is concurrency and wire format, not strength.

| Risk | Evidence that closes it |
|---|---|
| I5 (lifetime) — **the primary risk** | A new test that starts a real search and destroys the handler the moment `bestmove` is observed, run under `sanitize-linux` (ASan: a callback touching a dead writer is a use-after-free). `tsan-linux` does not run the Catch2 tier; it drives real `go` commands through the engine binary, which exercises the capturing callbacks for races as-is. |
| I1 (wire format) | Capture the full transcript of a fixed command script (`uci`, `isready`, `position`, `go depth 8`, `perft 4`, `eval`, `quit`) on `origin/main` and on the branch; diff must be empty apart from node/time figures. Drive it from a scratchpad script — and per `reference_uci_stdin_pipe_needs_handshake`, read until `bestmove` rather than piping blind. |
| I2 (line atomicity) | The existing #237 interleaving tests, unchanged in PR1 — that they still pass against a rebuilt writer is the point of D7. |
| I4 (transcript order) | The existing `cmd_perft` divide tests (`UCIReportingTests.cpp:360,397,414,490`) plus one that issues `perft` while a search is running and asserts no `info` line lands inside the divide block. |
| I6 (search) | `Compare-SearchEquivalence.ps1 -After <worktree exe>`. |
| Everything else | `Run-Tests.ps1` full fast tier, then `Validate-PrePR.ps1`, per PR. |

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D4 — the one reason `cmd_perft` still stops and joins | rewritten comment at `UCIHandler.cpp:510-511`; this is the fact most likely to be "cleaned up" later by someone reading the review |
| D3 — why the callbacks hold a `shared_ptr` by value | source comment at the `StartAsync` call site, replacing the current "captures nothing: send() is static" note |
| D1 — the sink receives a line without its newline | source comment on `UciWriter::LineSink` |
| I2 — one-line atomicity and the #237 forfeiture consequence | moves with the mutex, from `send()` onto `UciWriter::send()` |
| D2, D6, D7 and their rejected alternatives | PR bodies |
| The test-only race `CoutRedirect` created | `Docs/TestDesign.md`, when PR2 deletes it — it explains why the new capture sink is the only sanctioned way to read UCI output |
| Any decision that changed during implementation | back into this table, before each PR |
