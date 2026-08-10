# TT New-Game Clear Design

**Issue:** #259 — TT is zeroed twice per game, and the second pass runs on the arbiter's clock

## Goal

Remove transposition-table clearing from `AIPerplex::GetMove()` while preserving an explicit,
correct new-game reset for both UCI and non-UCI game mode. A freshly constructed table must skip the
redundant bucket walk, and search behavior must otherwise remain unchanged.

## Scope

This change will:

- make `TranspositionTable::clear()` detect and report the already-empty case without walking its
  buckets;
- add an explicit AI new-game lifecycle hook;
- invoke that hook from both `UciHandler::cmd_ucinewgame()` and non-UCI `Game` player setup;
- remove the `fullMoveCount == 1` clear from the timed `AIPerplex::GetMove()` path;
- preserve TT contents when a client analyzes or searches a FEN whose fullmove field is one;
- add unit coverage for the empty fast path, concrete lifecycle override, and move-metadata
  regression, with runtime validation of both lifecycle owners; and
- measure the first-search latency and verify deterministic search equivalence at `Threads=1`.

This change will not implement lazy TT generations, change TT entry layout or replacement policy,
add a UCI `Hash` option, or alter search tuning.

## Design Decisions

### D1: Use the existing entry counter as the known-empty state

`TranspositionTable` already maintains an atomic `entry_count`. A zero value proves that no entry
has been stored since construction or the last completed clear. `clear()` will take its existing
whole-table mutex, return immediately when this counter is zero, and otherwise perform the current
bucket-by-bucket reset.

`clear()` will return `true` when it removed stored entries and `false` when the table was already
empty. This makes the performance-relevant behavior deterministic and directly testable without a
timing assertion. The existing callers do not require the result and may ignore it.

The method will still reset the table's age and diagnostic counters after a populated clear. The
empty fast path does not need to reset age because no stored entry can observe it; the next stored
entry uses the current age consistently. No entry representation or probe behavior changes.

The zero-count check is safe for the intended lifecycle use because both runtime owners call it only
when no search can be storing entries. That precondition will be stated beside `clear()` in the
source: the whole-table mutex serializes clear calls, but stores take only bucket locks and are not
excluded by it. This change deliberately does not claim to make clear concurrent with search.

### D2: Put new-game behavior on the AI lifecycle, not move metadata

`PlayerAiBase` will gain a virtual `StartNewGame()` default no-op so lifecycle owners can notify any
AI without knowing its concrete type. `AIPerplex` will override it and call `_tt->clear()`.

This hook belongs on `PlayerAiBase`, rather than `IPlayer`, because humans have no search state and
all runtime call sites already distinguish AI players. Legacy AI implementations inherit the no-op,
so their behavior is unchanged.

`Game::SetPlayerParams()` returns `std::unique_ptr<IPlayer>`, so its concrete call will use
`dynamic_cast<PlayerAiBase*>` beside the existing `SetThreads` block at the end of the function. This
adds one downcast while #256 is considering cleaner search-interface boundaries, but the lifecycle
method itself belongs on that future interface and carries forward rather than becoming discarded
work.

### D3: Both runtime owners call the hook before any move clock starts

`UciHandler::cmd_ucinewgame()` will stop any search, rebuild the AI, restore its configured options,
call `StartNewGame()`, and reset the board. The rebuilt `AIPerplex` owns a fresh TT, so its call takes
the constant-time empty path.

Non-UCI `Game` mode will call `StartNewGame()` while configuring each AI player, before `Game::Run()`
requests a move. This explicitly covers the gameplay path even though players currently receive a
fresh TT during construction. If player ownership is later reused across games, the lifecycle hook
remains the correct reset point.

An instrumented 20-game `Run-EloMatch.ps1 -Smoke` probe on 2026-08-10 recorded exactly 20
`ucinewgame` calls in each side's isolated working directory: one per game per engine. Fastchess
therefore retains fresh-table match isolation after this change. A UCI client that omits
`ucinewgame` will intentionally keep TT contents for its whole engine session; entries remain
position-keyed and age normally. The engine will not infer resource lifetime from FEN move metadata
to compensate for a missing lifecycle command.

`AIPerplex::GetMove()` will no longer inspect `GameInfo::fullMoveCount` to manage TT lifetime. That
field describes the board position, not ownership of engine resources, and custom/FEN positions can
legitimately report move one without representing a new engine game.

## Data Flow

UCI mode:

`ucinewgame` → `stop_and_join()` → `init_ai()` → `ai_->StartNewGame()` → board reset → later `go`

Game mode:

`Game::Init()` → configuration → player factory → AI settings → `StartNewGame()` → later `Run()`

In both paths, `GetMove()` begins with per-search initialization and limit arming only; it performs
no whole-table reset.

## Error and Concurrency Behavior

The lifecycle owners call `StartNewGame()` only when no search is running: UCI joins its search
thread first, while game mode calls it before gameplay begins. This preserves the current practical
concurrency contract for `clear()` and avoids expanding #259 into a concurrent-clear redesign.

The fast-path decision is made while holding the existing whole-table mutex, serializing concurrent
clear calls. Stores continue to use per-bucket locks as before; no hot probe/store path gains a new
lock or branch.

## Tests

1. `[tt]`: a newly constructed TT reports that `clear()` removed nothing.
2. `[tt]`: after a store, `clear()` reports work, removes the entry, and resets counters.
3. `[tt]`: a second clear reports the table was already empty.
4. `[search]`: populate an `AIPerplex` TT through test access, call `StartNewGame()`, and verify the
   entry is removed; this proves the concrete lifecycle override.
5. `[search]`: populate the TT, search a position whose FEN fullmove field is one, and verify the
   marker entry survives. This catches any future reintroduction of position-metadata-driven TT
   lifetime and covers a second existing defect in analysis/custom-position sessions.

The existing `[uci][smp]` tests continue to exercise `cmd_ucinewgame()` rebuilding the AI and
restoring configured options. A new "replacement AI starts empty" assertion would already pass
before this change because reconstruction alone creates a fresh TT, so it would not test the new
lifecycle hook; the direct `[search][tt]` test above is the regression for that behavior.

Non-UCI game setup receives no artificial counter merely to observe a side-effect-free hook call on
a freshly constructed table. Its production insertion point is explicit above; headless AIPerplex
self-play validates the complete game-mode path but is reported as runtime validation, not as proof
that this particular call occurred.

Tests will be written and observed failing before each production change.

## Validation

- Build the clang-cl Release test target and run the focused `[tt]`, `[search]`, and `[uci]` tests.
- Run the complete fast test tier.
- Compare `ucinewgame` + `position startpos` + `go depth 1` against the same probe from a position
  past move one, recording wall-clock time to first `bestmove`.
- Verify identical best moves and node counts at `Threads=1` for fixed deterministic probes.
- Run the repository's search self-play validation and required `search-reviewer` review because
  `AIPerplex.cpp/.h` changes.
- Run `Validate-PrePR.ps1`; its change-tier classification is authoritative.
- No Elo match: the design removes redundant setup work without changing retained entries or search
  decisions.

## Invariants

- A real new-game notification empties a populated AIPerplex TT before search begins.
- Fresh TTs do not walk every bucket merely to establish that they are empty.
- Neither UCI nor non-UCI game mode clears the TT from inside `GetMove()`.
- TT entry layout, replacement scoring, probing, and storage semantics remain unchanged.
- `Threads=1` deterministic node counts and best moves remain identical.
- No per-node work is added.
