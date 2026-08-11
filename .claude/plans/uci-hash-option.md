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
- Preserve the configured table across `ucinewgame`, which clears it through the existing
  `StartNewGame()` lifecycle without rebuilding the AI.
- Record the exact-fit allocation ladder `192 / 384 / 768 / 1536` where the option contract is
  defined, while continuing to accept intermediate budgets.

**This change will not:**

- Add `Ponder`, `MultiPV`, `UCI_Chess960`, occupancy counters, or hash-size measurement tooling.
- Change the effective default table size, replacement policy, bucket geometry, or search logic.
- Add an in-place `TranspositionTable::resize()` operation.
- Decide whether a larger shipping default is stronger; that remains #260 and requires measurement.

## Decisions

### D1: Advertise the effective exact-fit default

The option is `option name Hash type spin default 192 min 1 max 1536`. The former constructor
request of 256 MiB already rounds down to 192 MiB because a bucket is 96 bytes and the bucket count
must be a power of two. Requesting and advertising 192 therefore makes the contract honest without
changing the table, search tree, or memory occupied by entries.

The maximum is the largest exact-fit value identified in #254's discussion. The minimum remains 1
MiB, matching the conventional UCI shape and the table's existing budget semantics; values that are
not exact fits continue to round down rather than exceed the requested budget.

### D2: Replace the table object instead of resizing it in place

`AIPerplex::SetHash(unsigned)` constructs a new `TranspositionTable` and replaces `_tt` only after
construction succeeds. This naturally recomputes the index mask, creates the correct lock array,
and resets entries and counters. It also gives replacement strong exception safety: allocation
failure leaves the old table alive.

An in-place `TranspositionTable::resize()` was rejected because the only caller is lifecycle
configuration outside search. It would add whole-table synchronization, partial-allocation cleanup,
and tests for an API that the engine does not otherwise need. Rebuilding the whole AI was rejected
because it would undo #266 and reintroduce per-option restoration state.

### D3: Configure through the existing player-AI interface

`PlayerAiBase` gains a default no-op `SetHash(unsigned)` beside `SetThreads(unsigned)`, and
`AIPerplex` overrides it. `UciHandler` can therefore remain independent of the concrete search class
when applying an option. The `AIPerplex` override clamps to the advertised range before allocating.

### D4: Do not add `configured_hash_`

After #266, `run()` constructs the AI before processing commands and `cmd_ucinewgame()` keeps that
same instance. A successfully applied `Hash` value therefore survives new-game lifecycle calls
without a shadow copy. Direct unit tests must construct the AI before setting `Hash`, matching the
real command loop rather than creating a test-only pre-initialization contract.

### D5: Reuse the existing in-search refusal

`cmd_setoption()` already refuses before parsing or mutating state while `searching_` is true. The
Hash branch stays behind that guard. Replacing `_tt` is consequently a lifecycle operation with the
same no-concurrent-search precondition as `StartNewGame()`; no new table synchronization is needed.

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
- `ucinewgame` clears but does not resize or replace the configured table.
- A requested budget is never exceeded; intermediate values keep the existing round-down rule.
- Unknown or malformed options remain ignored according to the existing parser contract.

## Validation

This is an Engine-tier change because it modifies UCI configuration and `AIPerplex`; the full
pre-PR gate applies.

- `[uci]` tests pin the advertised line, immediate application, malformed input behaviour,
  lower-bound clamping, TT reset, persistence across `ucinewgame`, and refusal during search.
- Existing and focused `[tt]` tests pin exact-fit sizing, power-of-two buckets, correct masks through
  store/probe behaviour, counter reset, and the never-exceed-budget rule.
- Compare `origin/main` and the candidate with `Run-Bench.ps1` at `Threads=1`; all fixed-depth node
  counts and best moves must be identical at the default.
- Run `Validate-PrePR.ps1`; its change-tier selection supplies the complete repository gate.
- Dispatch the required `search-reviewer` because `AIPerplex.h` changes.

No Elo match is needed: the effective default table and search decisions must be equivalent. A node
or best-move difference is a correctness failure, not a strength result.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Hash bounds and exact-fit default | Named constants and option-contract comments in `AIPerplex.h` / `UCIHandler.cpp` |
| Replacement requires no active search | Source comment on `AIPerplex::SetHash()` and the existing `cmd_setoption()` guard |
| Actual allocation can be below an intermediate request | Existing `TranspositionTable` constructor comment and allocation tests |
| Default equivalence and validation evidence | PR body |
| Approved decision changed during implementation | This table before the design file is removed |

The design file is a working specification for #254 and will be deleted after implementation and
review once every durable item above has landed.
