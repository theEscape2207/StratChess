# Compact transposition-table storage — Design

**Issue:** [#442](https://github.com/theEscape2207/StratChess/issues/442)
**Status:** Approved; prototype in progress. Adoption evidence outstanding.
**Baseline:** `0d9ae52`; branch updated through `b53d457` (#524/#525, workflow-only).
Prototype implemented; equal-capacity search and eviction evidence recorded on 2026-09-11 in the
[experiment protocol](compact-transposition-table-experiments.md). Timing and adoption remain open.

## Goal and scope

Reduce the current four-way TT bucket from 96 to 64 bytes without losing stored information.
Investigate memory and search performance before adoption. This covers packing, alignment and
capacity consequences; it excludes replacement-policy changes, lock redesign, rule-50 cutoff
policy (#347), new dependencies and runtime layout switches.

## Decisions

### D1: Private packed storage; unchanged public probe result

Keep public `TTEntry` and `probe()`/`store()` signatures unchanged. A private `PackedEntry` holds:

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 8 | full key |
| 8 | 2 | signed score |
| 10 | 2 | signed depth/quiescence budget |
| 12 | 2 | complete Move, including flags |
| 14 | 1 | metadata |
| 15 | 1 | unsigned age |

Metadata: phase in bit 0, bound in bits 1–2, node type in bits 3–4; bits 5–7 reserved and zero.
Pin enum encodings, use explicit masks and typed inline accessors, and initialize MAIN/EXACT/
ALL_NODE as `0x10`. Setters preserve unrelated bits; full initialization and clear reset them.
Keep all eight age bits, including modular replacement arithmetic. Bitfields, packed pragmas,
narrowed fields and public proxy members add unnecessary layout or compatibility risk.

Decode a hit into `TTEntry` under the shared lock, then apply existing mate denormalization.
Store and replacement inspect packed fields under the exclusive lock. Reuse the existing ranking
and tie-breaking logic through shared helpers; do not create separate packed/unpacked policies.
This confines representation changes to the table and preserves search readers and logical tests.
The rejected public-accessor migration would spread storage knowledge into `AIPerplex.cpp` for no
required API benefit. Decoding cost remains a measurement question, not a claimed free operation.

### D2: Align the bucket and retain locking

Use `alignas(64) Bucket` containing four packed entries in the existing vector. Permanent assertions
pin packed size/alignment to 16/8, standard-layout eligibility and offsets, and bucket size/alignment
to 64/64. Move the old `sizeof(TTEntry)==24` capacity tripwire and rationale to `PackedEntry==16`;
the unpacked result no longer determines capacity. Add a constructor Debug assertion that
`reinterpret_cast<uintptr_t>(table.data()) % 64 == 0`; the 64-byte stride aligns later buckets too.

Keep the separate per-bucket mutex array, copied probe result and clear's quiescent-writer contract.
Do not align each entry to 64 or change the baseline's 96-byte stride. The hypothesis is fewer entry
cache lines per lookup: a full 96-byte bucket spans at least two 64-byte lines (possibly three with
only natural alignment), while the candidate fits one. Early probe exits need not touch the full
bucket; locks still cause separate memory traffic. This is not a guarantee of fewer misses.

### D3: Retain natural sizing; distinguish unchanged and increased capacity by request

Use `bucket_count_for(H) = floor_pow2(max(1, floor(H * 1048576 / sizeof(Bucket))))` as a pure
`constexpr` helper. Hash remains an entry-byte budget rounded down, with locks additional.
Reject a permanent legacy 96-byte sizing unit: it unnecessarily couples storage to old geometry.

The engine/UCI default is **192 MiB** (`AIPerplex.h:45`); 256 is only the TT constructor default.
Keep these requested defaults and UCI bounds unchanged in this change. At 192, both layouts have
2,097,152 buckets and 8,388,608 entries. Capacity is also preserved at H = 3·2^k (k >= 0),
including 3, 6, 384, 768 and 1536. At other requests it may double; Hash=1 and 256 are examples.
Thus one candidate suffices: equivalence at matching capacity, search-change validation elsewhere.

Calculated MiB payloads, excluding allocator overhead:

| Hash request | Layout | Buckets | Entries MiB | Total: 8-byte locks | Total: 56-byte locks |
|---|---|---:|---:|---:|---:|
| 192 (engine default) | Current | 2,097,152 | 192 | 208 | 304 |
| 192 | Packed | 2,097,152 | 128 | 144 | 240 |
| 256 (capacity example) | Current | 2,097,152 | 192 | 208 | 304 |
| 256 | Packed | 4,194,304 | 256 | 288 | 480 |

Confirm actual mutex sizes on tested toolchains. `allocated_bytes()` is entry plus lock payload,
not resident memory. Update UCI exact-fit guidance to 128/256/512/1024; the unchanged 192 default
is no longer exact-fit. Its 64 MiB saving is verified arithmetic, not a measured speed gain.

### D4: Three bits available to #347; no clock policy here

Eight states could encode LOW, exact 94–99 and >=100, but that is only an example. Low-clock
ancestors can inherit high-clock dependence. #347 owns cutoff compatibility and same-key context
mismatch replacement, including move hints. Wider storage, a sidecar or a TT-only derived key
remain alternatives if more states are required. Do not change the repetition hash or combine a
clock-policy experiment with packing equivalence.

## Invariants and unresolved assumptions

Preserve all logical fields, four-way scan order, replacement ties, mate normalization, key-zero
convention, accepted same-key move inheritance, counters, empty-clear fast path and abort guards.
At matching capacity, preserve indexing and search results. At doubled capacity, expect collisions
to differ. Permanent tests must exercise the same logical behavior through packed storage.

Cross-compiler layout/over-alignment, generated decoding cost, target cache behavior, actual mutex
sizes and speed/strength effects remain unverified. The [experiment protocol](compact-transposition-table-experiments.md)
names the checks. Fewer bytes alone cannot prove faster construction or clear.

## Validation and adoption

Drafting is Docs tier and needs no Elo run. Implementation is Engine tier: TT/search contracts,
full validation, Linux Debug sanitizers, shipping clang-cl and MSVC compatibility. Apply required
TT/search diff review even though search call sites remain unchanged. Require exact Threads=1
equivalence at Hash=192 and eviction-forcing Hash=3, plus measured timing and total-memory evidence.
Capacity-changing settings require search-change/strength assessment before adoption; default-only
equivalence does not cover them. A strength-gain claim uses the CI lab with an owner-approved budget.
Retain 24-byte storage or defer if costs/uncertainty do not justify adoption; do not call an
inconclusive result neutral. No paid experiment is authorized by this document.

## Harvest

| Durable result | Destination |
|---|---|
| Encoding, layout, default metadata and locks | `TranspositionTable.h`, `TTTests.cpp`, `Docs/EngineContracts.md` |
| Capacity and actual/requested Hash semantics | Constructor/UCI comments, `UCITests.cpp`, `Docs/Engine-Readme.md` |
| Measured adoption or rejection evidence | `Measurements/` convention and `Docs/Changelog.md` |
| Three-bit budget and later clock choice | #347's eventual design/contract |

Review changed D1 to private storage and D3 to natural sizing; prototype implementation has started.
Retain the designs until results are harvested and reference/spec lifecycle checks permit deletion.
