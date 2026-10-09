# TT prefetch in DoMove — Design

**Issue:** #776 (spike #775, triage [comment](https://github.com/theEscape2207/StratChess/issues/776#issuecomment-6079027167))

## Goal

Every child node opens with a TT probe whose bucket is usually not in cache, and the search stalls
on that miss. Spike #775 variant B removed most of the stall by prefetching the child's bucket
inside `Board::DoMove`: **+4.18% [+3.95%, +4.40%] nps** on clang-cl, ordered pair, A/A +0.13%,
node-identical. It did so through a process-wide static on `Board`, which is not shippable. This
change ships B with an instance-owned hook. The search is unchanged, so any gain is pure speed.

## What variant C taught (measured 2026-10-09)

C kept `Board` free of the TT. The search predicted the child's key with an exact
`Board::KeyAfter(move)` and prefetched before `DoMove`. On an ordered pair (spacers, #785),
`Compare-Bench -Control -Rounds 60 -Affinity 4` measured **−1.43% [−1.54%, −1.31%]**, A/A 0.00%
[−0.24%, +0.25%], with all 8 positions negative. The disassembly of that binary shows two facts,
both plausible explanations rather than measured causes:

1. This clang-cl build compiled `_mm_prefetch(p, _MM_HINT_T0)` to **`prefetcht2`**, not
   `prefetcht0`. The spike's `__builtin_prefetch` emitted `prefetcht0`.
2. `KeyAfter` was not inlined: an out-of-line call into 176 static instructions with 7 register
   pushes. Author estimates, not measured: about 60–80 instructions executed per move against B's
   about 5, and a per-move budget under about 15 cycles to reach B, which only an approximate key
   could meet.

C's patches and data are in `StratChessSupport\CpuProfiles\2026-10-09-776\`. Each lesson becomes a
decision below: (1) is D4, (2) is D2's budget, and the measurement gap it exposed is D5.

## Review focus

- **D7, the hook's lifetime.** A stale prefetch need not fault, and ASan does not see it, so a
  search's results cannot show a lifetime mistake. Target-state assertions (Validation) catch a
  missing or wrong binding or reset; the declaration and join order stays a review item.
- **D5, measuring with `DoMove` held in place.** The spike's pair controlled only `pvs` and
  `quiescence`, so B's resized `DoMove` floated between the two images. Does adding `DoMove` and
  `DoNullMove` to the ordered pair's hot list close that gap?

## Scope

**This change will:**

- add an opaque prefetch target to `Board`, set by `AIPerplex` on the boards it owns and reset when
  the search ends (D1, D7);
- prefetch the child's bucket in `DoMove` and `DoNullMove` from the exact child key (D2, D3);
- have `TranspositionTable` supply the target from its own bucket-index rule (D6);
- extend `New-OrderedBuildPair.ps1`'s hot list with `Board::DoMove` and `Board::DoNullMove` (D5);
- measure and gate by D5 and D8.

**This change will not:**

- change the search: `pvs()` and `quiescence()` are untouched;
- change real hashes, make/unmake state, TT geometry, replacement, probe or store;
- change `LinkerMap.ps1`'s shared hot list, which `Test-CodeAlignment.ps1` also reads;
- run the strength lab or claim Elo. That is the owner's call afterwards (see Validation).

## Decisions

### D1: Variant B — `Board` prefetches, through an opaque instance-owned target

`Board` gains a private `{const char* base; uint64_t mask}` pair. It defaults to a static
64-byte-aligned dummy bucket with mask 0, so `DoMove` stays branch-free on any board no search has
bound. It is set through `SetPrefetchTarget(PrefetchTarget)`, where `PrefetchTarget` is a small
struct in its own header. `Board` never includes or names `TranspositionTable`.

Rejected:
- **C**, measured above. Fixing it needs an approximate key, which gives up the exact-key contract
  that made C attractive; the author's unmeasured estimate was about a 40% chance of matching B.
- **A process-wide static (the spike).** It breaks with two engines in one process, as in the tests.
- **A pointer to the TT on `Board`.** It couples the types and adds a dependent load on every move.

### D2: The key is exact at no cost

The prefetch sits where the spike put it: after `current_ply_++`, before the `InCheck()` legality
check. By then `DoMove` has applied the piece, castling-rights and en-passant updates to
`zobrist_hash_`. Only `change_player()` remains, so `zobrist_hash_ ^ zobrist::side_key` is exactly
the child's key. Nothing about the hash update is duplicated, so nothing needs a contract test.

Budget: two loads from `this`, plus xor, and, shift, add and prefetch, all inline. The hook adds
no call, no indirection and no branch.

Pseudo-legal moves that `InCheck()` then rejects still prefetch. That wastes a prefetch and nothing
else, and the spike measured it.

### D3: `DoNullMove` prefetches too

The prefetch goes after `DoNullMove` removes the en-passant key and before `change_player()`, so
`zobrist_hash_ ^ side_key` is exact there as well. The spike missed this site, and A-pvs covered it
from the search. Doing it in `Board` keeps `pvs()` unchanged.

### D4: The prefetch instruction is `prefetcht0` on every compiler

```cpp
#if defined(__clang__) || defined(__GNUC__)
	__builtin_prefetch(address);                    // prefetcht0 (read, locality 3)
#else
	_mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
#endif
```

It is wrapped in one inline helper in `Compat.h`. clang-cl compiles `_mm_prefetch(_MM_HINT_T0)` to
`prefetcht2` (C's post-mortem), so the single-path form that C used is rejected. Before any quiet
window, the bench binary's disassembly must show `prefetcht0` in `DoMove` and `DoNullMove`
(Validation).

### D5: Measurement holds `DoMove` and `DoNullMove` in place

`New-OrderedBuildPair.ps1` gets a script-local hot list: `LinkerMap.ps1`'s `pvs` and `quiescence`,
plus `?DoMove@Board@@` and `?DoNullMove@Board@@`. The shared list stays as it is, because
`Test-CodeAlignment.ps1`'s fixtures assume two hot functions. B resizes both `Board` functions;
#785's spacers align each resized function after the first, and the placement check covers all
four. If no exact cold spacer set exists, stop before any quiet window and report. Do not fall back
to an uncontrolled comparison.

Then run one `Compare-Bench.ps1 -Control -Rounds 60 -Affinity 4`, about 40 minutes quiet. The
optional GCC trend (`Compare-BenchLinux.ps1`, about 20 minutes) decides nothing.

### D6: The TT supplies the target from its own index rule

`TranspositionTable::prefetch_target()` returns `{table.data(), index_mask}`, and its private
`bucket_index(key)` (`key & index_mask`) serves `probe()` and `store()`. `Board` forms
`base + (key & mask) * 64`, with `static_assert(sizeof(Bucket) == 64)` beside `prefetch_target()`.
That pins the one fact the two sides share.

### D7: Lifetime — set after the copy, reset after the joins

- **Set.** `init_search` sets the target on `td_.board` after `td_.board = root`. Each helper sets
  it on `htd.board` after its copy from `root`. A copy of the caller's board carries the dummy, so
  the order is copy then set.
- **Reset.** On every exit, exceptional ones included, the order is: stop, join the helpers, reset
  the targets, finish the launch. In `Search()`'s RAII order (`launch_guard`, helper vector,
  `stop_guard`) the reset guard is declared after `launch_guard` and before the helper vector, so
  reverse destruction runs it after the joins. It skips helpers that were never created.
- **TT replacement.** `SetHash` replaces the table only while no search runs, and the targets are
  reset by then. A target never names a table that is not live.
- **Non-search callers** (game loop, UCI replay, perft, `IsLegalMove`) keep the dummy. They pay one
  prefetch per move to the same cached 64 bytes.

### D8: Gate — a Speedup verdict

Accept if the run from D5 issues **Speedup**: the 95% lower bound of candidate vs
mean(baseline, control) is above 0, with the whole A/A interval inside ±0.5%. The issue's +3.5% was
C's bar for matching B, not a gate on B. The spike's +3.95% lower bound sets the expectation, not
the gate. An invalid run (A/A out of band, a pair failure, a time loss) resolves nothing. A valid
non-Speedup parks #776, and the spike's number is never shipped.

## Assumptions I cannot verify from the code

- **A1: the instance hook keeps the spike's gain.** The spike read a static base and mask; the hook
  reads two members of `this`, which `DoMove` already has in cache. Not verified; D5 settles it.
- **A2: an exact cold spacer set exists for two resized `Board` functions.** C needed one 64-byte
  spacer. Not verified; building the pair settles it before any quiet window.
- **A3: LTO leaves `pvs` and `quiescence` the same size.** `DoMove` is called, not inlined, in C's
  disassembly, and B does not change its signature. If they change size, D5's spacers still apply.
  Building the pair settles it.
- **A4: `__builtin_prefetch` emits `prefetcht0` on clang-cl and GCC.** The spike's binary showed it
  on clang-cl. The disassembly check in Validation verifies it per build.

## Invariants

- The search is node-identical. `Compare-SearchEquivalence.ps1` against the merge base reports
  IDENTICAL at `Threads=1`.
- `Board` does not include or name `TranspositionTable`.
- A target is bound only between the search's copy of a board and the joins of its helpers. Outside
  that window every board holds the dummy.

## Validation

**Engine tier**, plus the Tooling change to `New-OrderedBuildPair.ps1` (self-test cases for the
extended hot list).

- **Unit tests.**
  - A default-constructed and a FEN-constructed `Board` carry the dummy target.
  - Binding and reset, through a `Board::prefetch_target()` getter and the search fixture: during an
    iteration observation the main and helper boards hold their engine's own TT target; after a
    normal return, an early stop and a throwing observer (two threads) they hold the dummy again.
  - Two engines with different `Hash` sizes search in one process without interference. The
    existing search tests cover this once the static is gone.
- **Disassembly.** `prefetcht0`, and no `prefetcht1`/`prefetcht2`, in `Board::DoMove` and
  `DoNullMove` of the Release clang-cl exe (`llvm-objdump -d`). This runs before the quiet window.
- **Equivalence.** `Compare-SearchEquivalence.ps1` reports IDENTICAL against the merge base.
- **Speed.** D5 and D8.
- **Elo.** Not needed to accept: the tree is identical, so the gain is pure speed. B's Elo stays
  unmeasured; the lab is GCC, whose spike trend was +3.23% nps. A lab run is the owner's call.

## Cost

- **Size:** 50–200 lines across `Board.h`, `Board.cpp`, a `PrefetchTarget` header,
  `TranspositionTable.h`, `AIPerplex.cpp`, `Compat.h`, `New-OrderedBuildPair.ps1`, a test file and
  the changelog.
- **Blast radius:** Engine tier, plus one benchmark tool.
- **Review:** one code review (170–270k tokens, 3–5 min), plus `search-reviewer` for
  `AIPerplex.cpp`.
- **Quiet machine time:** about 40 minutes, once. Optional GCC trend: about 20 minutes.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1/D2 exact key at the prefetch point; why `Board` holds an opaque target | source comment at the prefetch in `DoMove` and on `PrefetchTarget` |
| D4 clang-cl `_MM_HINT_T0` → `prefetcht2` trap | source comment on the `Compat.h` helper |
| D7 lifetime and reset order | source comment at the reset guard in `Search()`; `Docs/EngineContracts.md` if it has a search-lifecycle section |
| D5 extended hot list | `New-OrderedBuildPair.ps1` help |
| C's result and post-mortem; B's measured result | `Docs/Changelog.md`, the PR body, #776 |
