# Lock-free transposition table — Design

**Issue:** #747 (decision record: #250)

## Goal

Every `TranspositionTable::probe()` and `store()` takes a per-bucket `std::shared_mutex` from a
separate array. At `Threads=1`, where nothing ever contends, that costs **9.4%** of CPU on the
shipping clang-cl build and **32.9%** on Linux/GCC 15, the strength lab's build (#719 profiles, bench
set, depth 13). The Linux share is mostly the cache miss of touching a 56-byte `pthread_rwlock_t` in
a second array on every probe. Those figures cap the gain at roughly 1.10× nps on Windows and 1.49×
on Linux. The lock array also takes 16 MiB (Windows) or 112 MiB (Linux) beside 128 MiB of entries at
`Hash=192`. This change removes the locks and keeps the table's single-thread behaviour bit-for-bit.

## Review focus

- **D1's memory-model argument and its residual risk.** A torn read can be accepted as another key
  with probability about 2⁻⁽⁶⁴⁻ⁱⁿᵈᵉˣ ᵇⁱᵗˢ⁾, which is 2⁻⁴³ at `Hash=192`. Check that the bound holds,
  that it stays below the collision rate the table already accepts, and that an accepted wrong
  payload cannot do worse than a hash collision (D5).
- **D3, removing the counters.** Check that nothing outside the tests reads `count_entries()` or
  `count_pv_nodes()`, and that the replacement for `clear()`'s early-out keeps its contract.
- **Assumption A1**, that relaxed `std::atomic<uint64_t>` loads and stores compile to plain moves on
  both toolchains. If it is false, the nps gain shrinks.

## Scope

**This change will:**

- store each entry as two `std::atomic<uint64_t>` words, XOR-validated (D1, D2);
- delete `bucket_locks`, `make_bucket_locks()`, `lock_bytes()` and the per-bucket lock acquisitions
  in `probe()`, `store()`, `hashfull()` and `clear()`;
- delete `entry_count`, `pv_count`, `count_entries()` and `count_pv_nodes()` (D3);
- add a torn-pair unit test and a concurrent probe/store stress test (Validation);
- update the comments that describe the locks: the `HugePageAllocator` and Windows storage comments
  and the class comment in `TranspositionTable.h`, the `lock_bytes()` comment in `UCIHandler.cpp`,
  and the TT rows in `Docs/Architecture.md`.

`tt_mutex` becomes the class's only lock. It serialises `clear()` calls that the single controlling
thread already serialises. That is existing debt, and this change neither adds to it nor removes it.

**This change will not:**

- change `PackedEntry`'s layout, `Bucket`'s 64-byte size, the bucket count, replacement scoring,
  `sameKeyStoreWins()`, mate normalisation, or what a probe returns at `Threads=1`;
- change `tt_mutex`, `newSearch()` or the ageing scheme;
- unify the Linux and Windows table storage types (#685) or add Windows large pages (#684);
- add a Catch2 run to the `tsan-linux` job (`Docs/CI.md` explains why it has none).

## Decisions

### D1: Two relaxed atomic words per entry, the first holding `key ^ payload`

`PackedEntry` is already exactly an 8-byte key at offset 0 plus an 8-byte payload (value, depth,
move, metadata, age) at offset 8. Each storage slot becomes:

```cpp
struct Slot {
	std::atomic<std::uint64_t> key_xor_data;
	std::atomic<std::uint64_t> data;
};
```

`store()` writes `data` and `key_xor_data = key ^ data`; nothing relies on the order of the two
stores. `probe()` loads both and accepts the
slot only if `key_xor_data ^ data == key`. Every access is `memory_order_relaxed`.

**Memory-model argument.** Both words are atomic, so concurrent access is not a data race and has no
UB. A relaxed load returns some value from that word's modification order, so each word read is
whole. Nothing else is published through an entry: the payload is self-contained and no reader
dereferences anything it names. So no ordering beyond per-word coherence is needed, and none is
bought. The only inconsistent outcome is a pair whose two words come from different stores. It
decodes as `key_old ^ data_old ^ data_new`. For that to equal the probed key `K`, `K ^ key_old` must
equal `data_old ^ data_new`. `K` and `key_old` index the same bucket, so their low `index_bits` agree
and the remaining `64 − index_bits` bits are independent hash bits. The chance of a match is
therefore about 2⁻⁽⁶⁴⁻ⁱⁿᵈᵉˣ ᵇⁱᵗˢ⁾ per torn read: 2⁻⁴³ at `Hash=192` and 2⁻⁴⁰ at the `Hash=1536` cap.

**Why that risk is acceptable.** The locked table already accepts a false match whenever two
positions share all 64 key bits. At `Hash=192` that is about 4 × 2⁻⁴³ per probe (four slots, 43 free
bits), on every probe. The torn-read risk is smaller per event and applies only to the rare read that
overlaps a store, which never happens at `Threads=1`. D5 bounds what an accepted wrong payload can do.

**Rejected:**

- *Plain (non-atomic) racy reads, Stockfish-style.* A data race is UB, and the `tsan-linux` job would
  fail. D1 gets the same machine code legally (A1).
- *One 16-byte atomic.* It is not lock-free on the MSVC STL, GCC routes it through `libatomic`, and a
  16-byte atomic load on x64 is a `cmpxchg16b`, which writes the cache line.
- *A seqlock with a side array of versions.* It fits the 64-byte bucket, but the side array restores
  the second cache line per probe, which is most of the Linux cost (64% of `rdlock` samples are that
  first touch).
- *Striped or 1-byte locks.* These make the lock array cache-resident, which fixes most of the Linux
  cost. Every probe still pays an atomic read-modify-write, though, which is likely most of the
  Windows 9.4%, and stripes add false contention at higher thread counts.
- *Skipping locks at `Threads=1`.* Cheap, but it leaves two code paths and allocates locks it never
  uses. It does nothing under Lazy SMP, and correctness would depend on a mode flag tracking the
  thread count.
- *`std::atomic_ref` over the existing plain `PackedEntry` storage.* It needs no type change, but it
  relies on discipline at every access. A stray plain read would compile and race. Atomic members
  make the type enforce the rule.

### D2: `PackedEntry` stays the value type; a slot converts with `std::bit_cast`

`PackedEntry` keeps its layout, its `static_assert`s and its accessors. Two private helpers do the
conversion:

```cpp
// bit_cast to two words, then word[0] ^= word[1]
static constexpr std::array<std::uint64_t, 2> encode(const PackedEntry&) noexcept;
// two relaxed loads, word[0] ^= word[1], bit_cast back
static PackedEntry load(const Slot&) noexcept;
// encode, then two relaxed stores
static void publish(Slot&, const PackedEntry&) noexcept;
```

Word 0 is the key and word 1 the payload on either endianness, because the key is a whole
`uint64_t` at offset 0. New `static_assert`s:

- `std::is_trivially_copyable_v<PackedEntry>`, which `std::bit_cast` requires (`Move` is trivially
  copyable today);
- `std::atomic<std::uint64_t>::is_always_lock_free`;
- `sizeof(Slot) == sizeof(PackedEntry)`, with `sizeof(Bucket) == 64` kept.

`Slot` default-initialises both words to `encode(PackedEntry{})`, so an untouched or cleared slot
decodes to exactly today's `PackedEntry{}`: key 0, move `0xFFFF`, metadata `0x10`. Construction
writes the same bytes the current `std::vector<Bucket>` constructor writes, so the Linux zero-fill
reasoning in the allocator comment is unchanged. `encode(PackedEntry{})` is `constexpr`.

### D3: Remove `entry_count` and `pv_count`; `clear()`'s early-out reads a written flag

Without the bucket lock, the read-decide-adjust sequence in `store()` cannot keep the counters exact:
two stores into one bucket can both see an empty slot and both increment. Only `TTTests.cpp` reads
`count_entries()` and `count_pv_nodes()`. No engine, UCI or script consumer exists (checked by grep
at `79c3217`). They are removed, which also removes a `fetch_add` on a shared cache line from the
store path under Lazy SMP.

`clear()` used `entry_count == 0` to skip zeroing a table that holds nothing. It now reads
`std::atomic<bool> written_since_clear`. `store()` sets it with a relaxed load and then a store only
when it is false, so after the first store the flag's cache line is only read. `clear()` resets it.
`clear()` already requires that no search stores concurrently, so the flag is exact whenever
`clear()` reads it, and the return value keeps its meaning: whether entries were removed. A store
that is declined on the same-key path does not set the flag. That matches today, where a declined
store leaves `entry_count` unchanged.

**Rejected:** approximate counters. They would keep a shared-line RMW per store for a diagnostic
nobody reads.

### D4: `store()`'s decision runs on per-slot snapshots; races cost a write, never a wrong pairing

`store()` loads the four slots once each (D2's `load`), runs the existing replacement and
`sameKeyStoreWins()` logic on those snapshots unchanged, and publishes one slot. Under concurrency
there are three outcomes:

- two stores pick the same slot and one is lost;
- a decision rests on a slot that changed after it was read;
- the same-key path inherits `best_move` from a snapshot whose key matched (subject to D1's bound).

Each of these leaves a valid entry or a miss. None pairs a key with another key's payload beyond
D1's bound. This is the trade every lock-free table makes, and Validation covers it at the
multi-thread level. At `Threads=1` the snapshots are exact, so decisions are identical to today's.

### D5: An accepted wrong payload can only mis-score, never mis-move

`best_move` from a probe is used only to rank generated moves (`ScoreMovesBestFirst`) and is compared
by equality against them (`move == hash_move`, `AIPerplex.cpp:1005`). It is never played unverified.
Quiescence does not read `best_move` at all. So D1's residual case has the same effect as an ordinary
hash collision: a wrong score or bound for one node. This must stay true. The source comment on
`probe()` will say that a returned move is a hint that may be wrong.

### D6: `hashfull()` and `clear()` take no per-bucket synchronisation

`hashfull()` decodes its 250-bucket sample with `load()`. Under concurrent stores it may count a
slot that is mid-store either way, which is within the sample's own error. `clear()` publishes
`PackedEntry{}` into every slot. Its caller contract ("no search can store concurrently") is
unchanged, and `tt_mutex` still serialises concurrent `clear()` calls.

## Assumptions I cannot verify from the code

- **A1: relaxed `std::atomic<uint64_t>` loads and stores compile to plain `mov` on clang-cl and
  GCC 15, with no fence or `lock` prefix.** This is the documented codegen on x86-64 for both, but it
  is not verified on these builds. If it is false, the nps gain shrinks. It is settled by
  disassembling `probe` (inlined into `pvs` on clang-cl) in the Release build and finding no
  `lock`-prefixed instruction or `mfence` from the TT. Not yet done; implementation step.
- **A2: the D1 probability treats the non-index key bits as independent.** Zobrist keys are XORs of
  random 64-bit constants, so the bits beyond the index are as random as the constants. This is
  taken from the construction, not measured; the stress test bounds it empirically only at a far
  coarser level.
- **A3: the 9.4% Windows ceiling is roughly what the change recovers.** The profile attributes SRWLock
  self time. Some of that cost may move to the XOR or the second word's load. This is settled by
  Validation's Windows `Compare-Bench`, and nothing ships on the profile number.

## Invariants

1. At `Threads=1`, search is node-identical with identical best moves, because every probe and store
   decision is unchanged.
2. `probe()` returns a payload only for a slot whose words decode to the probed key.
3. An empty slot decodes to `PackedEntry{}`, both after construction and after `clear()`.
4. `clear()` returns whether stored entries were removed.
5. `sizeof(Bucket) == 64`, `alignof(Bucket) == 64` and `sizeof(PackedEntry) == 16`, unchanged.
6. No TT access is a non-atomic concurrent access: the table has no plain member that a search thread
   writes.
7. A probe's `best_move` is only ever a ranking hint (D5).

## Validation

Engine tier, search-change validation: it changes synchronisation on the hottest shared structure.

- **Invariant 1:** `Compare-SearchEquivalence.ps1 -After <clang-cl Release exe>` reports identical
  node counts and best moves at `Threads=1`. This is the single-thread correctness gate, and it is
  why no single-thread Elo match is needed.
- **Invariants 2 and 3:** unit tests.
  - Torn pair: the fixture writes the `key_xor_data` word from one entry and the `data` word from
    another, and `probe()` misses for both keys.
  - Defaults: the existing defaults/clear test, with the fixture now returning a decoded
    `PackedEntry` by value.
- **Invariant 6 and D4:** a `[tt][smp]` stress test.
  - Setup: `TranspositionTable(0)`, a single bucket, so the full 64-bit key is checked. Four threads
    store and probe random non-zero keys for a fixed number of iterations. Each payload is a pure
    function of its key.
  - Pass: every hit's payload equals that function of the probed key.
  - Positive control: temporarily drop the XOR, so word 0 holds the raw key and `probe()` compares
    word 0 alone. The test must fail; record this in the PR.
  - TSan: the `tsan-linux` job stays green over its six multi-threaded UCI scenarios at `Threads=4`,
    `8` and `16`.
- **Invariant 4:** the existing clear tests ("no work for a fresh table", "no work after the table is
  empty"), plus one new test: after a declined same-key store into an empty table, `clear()` reports
  no work.
- **Invariant 5:** the existing and new `static_assert`s.
- **A1:** a disassembly check as described there; result in the PR.
- **Speed:**
  - `Compare-Bench.ps1` on the clang-cl Release build against `origin/main`, with the 9.4% ceiling
    stated next to the result.
  - The Linux nps is reported for contrast, not as the shipping number.
  - Shipping condition: the Windows interval shows no slowdown. A Windows gain far below the ceiling
    is reported, not blocked: the Linux and Lazy SMP gains stand on their own.
  - If the paired delta is uniformly negative on node-identical builds, relink both builds with a
    shared `/ORDER` before reading it (#555's guidance in `Docs/Workflow.md` → Speed and nps).
- **Multi-thread strength (slice 3, owner-approved budget):** a local `Run-EloMatch.ps1`
  non-regression run with `option.Threads=4` on both sides. The strength lab cannot cover this
  because it pins `Threads=1`. At `Threads=1`, a lab run would measure only speed, inflated about 3×
  by the GCC lock; it is optional and not a gate.

## Cost

- **Size:** 50–200 lines, mostly in `TranspositionTable.h` and `TTTests.cpp`, about half of them
  deletions. Also touched: `UCIHandler.cpp` (one comment) and `Docs/Architecture.md` (three rows).
- **Blast radius:** Engine tier. No gate, skill or script changes.
- **Review:** one code review run, plus the search-reviewer agent, since the change touches TT
  semantics.
- **Optional parts:**
  - The A1 disassembly check, about 15 minutes. It explains a disappointing bench, and can be
    dropped if the bench lands near the ceiling.
  - The multi-thread match, slice 3. Its budget is the owner's call; it covers Lazy SMP behaviour,
    which nothing else tests for strength.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 publication scheme, memory-model argument, residual bound | class comment on `TranspositionTable` and the `Slot` comment |
| D3 why there are no entry counters; the `written_since_clear` contract | comment on the flag and on `clear()` |
| D4 races cost a write, never a pairing | comment in `store()` |
| D5 a probe's move is a hint that may be wrong | comment on `probe()` |
| lock array gone; TT synchronisation model | `Docs/Architecture.md` TT rows (lines 67, 131, 177) |
| measured Windows and Linux nps, equivalence result | `Docs/Changelog.md` and the PR body |
