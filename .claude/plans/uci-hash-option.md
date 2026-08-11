# UCI Hash Option — Design

**Issue:** #254

## Goal

Let UCI clients select the transposition-table memory budget without recompiling the engine, while
preserving the effective 192 MiB table and identical search behaviour at the default setting.

## Scope

**This change will:**

- Advertise `Hash` as a UCI spin option with default `192`, minimum `1`, and maximum `1536` MiB.
- Apply `setoption name Hash value N` immediately when no search is running.
- Replace the live transposition table with a newly constructed, empty table for the clamped budget.
- Report the actual entry allocation and bucket count through `info string` after success, and keep
  the previous table in service with an `info string` diagnostic if allocation fails.
- Preserve the configured table across `ucinewgame`, which clears it through the existing
  `StartNewGame()` lifecycle without rebuilding the AI.
- Record the exact-fit allocation ladder `192 / 384 / 768 / 1536` where the option contract is
  defined, while continuing to accept intermediate budgets.

**This change will not:**

- Add `Ponder`, `MultiPV`, `UCI_Chess960`, occupancy counters, or hash-size measurement tooling.
- Change the effective default table size, replacement policy, bucket geometry, or search logic.
- Add an in-place `TranspositionTable::resize()` operation.
- Decide whether a larger shipping default is stronger; that remains #260 and requires measurement.
- Add a `game_settings.json` Hash field; game mode and the tactical runner retain the constructor
  default, while this issue adds the client-facing UCI control only.

## Decisions

### D1: Advertise the effective exact-fit default

The option is `option name Hash type spin default 192 min 1 max 1536`. The former constructor
request of 256 MiB already rounds down to 192 MiB because a bucket is 96 bytes and the bucket count
must be a power of two. Requesting and advertising 192 therefore makes the contract honest without
changing the table, search tree, or memory occupied by entries.

The maximum is a policy cap, not the largest mathematical exact fit: 3072 MiB and larger values can
also produce power-of-two bucket counts. The 1536 MiB cap exposes an eightfold experimental range
over today's table while bounding steady-state allocation to 1664 MiB on Windows and 2432 MiB on
Linux, including the platform-dependent lock array. Construct-before-replace can temporarily hold
both tables, so failure at the cap must be recoverable rather than process-fatal.

The minimum remains 1 MiB, matching the conventional UCI shape and the table's existing budget
semantics. It allocates 8192 buckets (768 KiB of entries), so integer `memory_mb()` deliberately
reports `0`; the bucket count in the UCI diagnostic makes that sub-MiB result explicit. Values that
are not exact fits continue to round down rather than exceed the requested *entry-byte* budget.
Locks are additional memory and are reported separately by `lock_bytes()`; Hash is not a cap on
whole-process resident memory.

### D2: Replace the table object instead of resizing it in place

`AIPerplex::SetHash(unsigned)` constructs a new `TranspositionTable` and replaces `_tt` only after
construction succeeds. This naturally recomputes the index mask, creates the correct lock array,
and resets entries and counters. It catches `std::bad_alloc`, returns a failed result without
replacing `_tt`, and lets `UciHandler` report the failure through `info string`; the engine remains
responsive with the previous table in service.

An in-place `TranspositionTable::resize()` was rejected because the only caller is lifecycle
configuration outside search. The table contains atomics and a `shared_mutex`, so it is neither
copyable nor movable; in-place resizing would require hand-written whole-table swap machinery,
partial-allocation cleanup, synchronization, and tests for an API the engine does not otherwise
need. Rebuilding the whole AI was rejected because it would undo #266 and reintroduce per-option
restoration state.

### D3: Configure through the existing player-AI interface

`PlayerAiBase` gains a small Hash-result type and a default unsupported `SetHash(unsigned)` beside
`SetThreads(unsigned)`; `AIPerplex` overrides it. The result carries success, actual entry MiB, and
bucket count, so `UciHandler` can report allocation without reaching through the player interface to
the concrete table. The `AIPerplex` override clamps to the advertised range before allocating and is
`noexcept` because allocation failure is part of the option's result contract.

### D4: Do not add `configured_hash_`

After #266, `run()` constructs the AI before processing commands and `cmd_ucinewgame()` keeps that
same instance. A successfully applied `Hash` value therefore survives new-game lifecycle calls
without a shadow copy. Direct unit tests must construct the AI before setting `Hash`, matching the
real command loop rather than creating a test-only pre-initialization contract.

This dependency on construct-before-dispatch ordering is recorded at the Hash branch, and a UCI
test pins that a Hash request applies to the live AI and survives `ucinewgame`. If `init_ai()` ever
becomes lazy again, the comment and test make the deliberate asymmetry with `configured_threads_`
visible rather than silently dropping a client setting.

### D5: Reuse the existing in-search refusal

`cmd_setoption()` already refuses before parsing or mutating state while `searching_` is true. The
Hash branch stays behind that guard. Replacing `_tt` is consequently a lifecycle operation with the
same no-concurrent-search precondition as `StartNewGame()`; no new table synchronization is needed.

The early-clear window introduced for #245 does not invalidate that conclusion. `searching_` becomes
false after `GetMove()` returns but before `bestmove` is emitted. By then all helper `jthread`s have
joined inside `GetMove()`, and the remainder of the UCI search lambda does not access `_tt`, so a
Hash command accepted in that narrow window still races with no table reader.

### D6: Make rounding and allocation failure visible to the client

After success, UCI emits `info string hash <actual MiB> MiB (<bucket count> buckets)` from the result
returned by `SetHash()`. This closes #254's user-visible failure mode: a 512 MiB request reports the
actual 384 MiB entry allocation rather than failing silently. At the 1 MiB minimum the intentional
message is `hash 0 MiB (8192 buckets)` because `memory_mb()` reports whole entry MiB.

If construction reports allocation failure, UCI emits an `info string` stating that the requested
Hash allocation failed and the previous table was retained. Unknown and malformed options remain
silent under the parser's existing convention.

### D7: Keep the remaining common UCI options unadvertised

`Ponder` is not advertised because the engine has no ponder-search / `ponderhit` lifecycle.
`MultiPV` is not advertised because the root search and result model produce exactly one PV.
`UCI_Chess960` is not advertised because the engine has no Chess960 castling protocol support.
Advertising any of the three without its underlying behaviour would make the option contract false;
each should be implemented under its own issue if wanted.

### D8: Accept and measure size-dependent new-game latency

#259 removed the redundant clear from the first search, and #266 moved the remaining reset into the
persisting AI's `StartNewGame()` lifecycle. A populated table is still cleared once per game, so
`ucinewgame` latency scales with Hash. It runs before `go`, so it is client-visible setup latency,
not time charged to the engine's search clock; the default cost remains unchanged.

Measure one populated-table `ucinewgame` sequence at 192 and 1536 MiB and record the result. The
existing latency probe's normal repeated-`ucinewgame` setup hits `clear()`'s empty fast path after
the first reset, so this measurement must explicitly complete a search to populate the table before
timing the following `ucinewgame`; no production script change is part of this issue.

## Assumptions I cannot verify from the code

- UCI clients interpret the conventional `Hash` spin option and respect its advertised bounds. This
  is the protocol convention explicitly requested by #254, but GUI behaviour is outside this
  repository. An end-to-end check with a supported GUI would verify presentation; it is not needed
  to verify engine protocol output or allocation behaviour.

## Invariants

- The default still allocates 2,097,152 buckets and 192 MiB of TT entries.
- At `Threads=1`, the default produces identical fixed-depth node counts and best moves to
  `origin/main`.
- A Hash change never occurs while a search is active.
- A successful Hash change leaves a correctly sized, empty table with reset counters and mask.
- A failed Hash allocation leaves the previous table in service and the engine responsive.
- `ucinewgame` clears but does not resize or replace the configured table.
- A requested entry-byte budget is never exceeded; intermediate values keep the existing round-down
  rule, while the separately reported lock array remains additional platform-dependent memory.
- Unknown or malformed options remain ignored according to the existing parser contract.

## Validation

This is an Engine-tier change because it modifies UCI configuration and `AIPerplex`; the full
pre-PR gate applies.

- `[uci]` tests pin the advertised line, immediate application and success diagnostic, malformed
  input behaviour, lower-bound clamping to 8192 buckets / 0 whole entry MiB, TT reset, persistence
  across `ucinewgame`, and refusal during search.
- Existing and focused `[tt]` tests pin exact-fit sizing at small allocations, power-of-two buckets,
  correct masks through store/probe behaviour, counter reset, and the never-exceed-entry-budget
  rule. No normal test allocates the 1536 MiB maximum; any direct maximum-size exercise must be
  `[slow]` and justify its multi-GiB Linux/sanitizer cost.
- Verify the allocation-failure branch by source-level construct-before-replace and catch-path
  inspection unless an existing low-cost allocation-failure seam is available; do not add a global
  allocator hook or multi-GiB exhaustion test solely to force `bad_alloc`.
- Compare `origin/main` and the candidate with `Run-Bench.ps1` at `Threads=1`; all fixed-depth node
  counts and best moves must be identical at the default.
- Measure populated-table `ucinewgame` latency at Hash 192 and 1536 using an explicit
  search-to-`bestmove` before each timed reset; report it as setup latency, not Elo or search time.
- Run `Validate-PrePR.ps1`; its change-tier selection supplies the complete repository gate.
- Dispatch the required `search-reviewer` because `AIPerplex.h` changes.

No Elo match is needed: the effective default table and search decisions must be equivalent. A node
or best-move difference is a correctness failure, not a strength result.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Hash bounds and exact-fit default | Named constants and option-contract comments in `AIPerplex.h` / `UCIHandler.cpp` |
| Replacement requires no active search, including #245's early-clear window | Source comment on `AIPerplex::SetHash()` and the existing `cmd_setoption()` guard |
| Failed allocation retains the old table | `SetHash()` result contract and UCI `info string` failure path |
| Actual allocation can be below an intermediate request | Existing `TranspositionTable` constructor comment and allocation tests |
| Hash covers entry bytes; locks are additional | UCI option-contract comment pointing to `lock_bytes()` |
| No `configured_hash_`; depends on construct-before-dispatch ordering | Hash branch comment and live-AI / `ucinewgame` UCI test |
| Ponder / MultiPV / UCI_Chess960 remain unsupported | #254 issue comment after revised-design approval |
| Size-dependent populated-table new-game latency | PR body and `Docs/Changelog.md` if the measurement is durable |
| Default equivalence and validation evidence | PR body |
| Approved decision changed during implementation | This table before the design file is removed |

The design file is a working specification for #254 and will be deleted after implementation and
review once every durable item above has landed.
