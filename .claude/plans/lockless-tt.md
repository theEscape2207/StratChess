# Lock-free transposition table — Design

**Issue:** #747 (decision record: #250)

## Goal

Every `TranspositionTable::probe()` and `store()` takes a per-bucket `std::shared_mutex` from a
separate array. At `Threads=1`, where nothing ever contends, that costs **9.4%** of CPU on the
shipping clang-cl build and **32.9%** on Linux/GCC 15, the strength lab's build (#719 profiles, bench
set, depth 13, commit `cbfce87`; Linux includes about 1.7 points of `clear()`, and the Windows share is
approximate because sampling slowed the workload). Taken as workload-specific estimates, those shares
cap the gain at roughly 1.10× nps on Windows and 1.49× on Linux. The Linux share is mostly the cache
miss of touching a 56-byte `pthread_rwlock_t` in a second array. The lock array also takes 16 MiB
(Windows) or 112 MiB (Linux) beside 128 MiB of entries at `Hash=192`. This change removes the locks
and keeps the table's single-thread behaviour bit-for-bit.

## Review focus

- **D1's residual-risk model.** A slot can hold two words from different stores, transiently or
  persistently. The claim is that this changes which key a slot claims, not how many slots can claim
  a probed key, so under the random-key model the per-probe false-match rate is unchanged. Check the
  model and its stated limits.
- **D3, removing the counters,** and the replacement for `clear()`'s early-out.
- **Assumption A1**, that relaxed 64-bit atomics compile to plain moves on both toolchains. If it is
  false, the nps gain shrinks.

## Scope

**This change will:**

- store each entry as two `std::atomic<uint64_t>` words, XOR-validated (D1, D2);
- delete `bucket_locks`, `make_bucket_locks()`, `lock_bytes()` and the per-bucket lock acquisitions
  in `probe()`, `store()`, `hashfull()` and `clear()`;
- delete `entry_count`, `pv_count`, `count_entries()` and `count_pv_nodes()` (D3);
- add deterministic mixed-pair tests and a concurrent probe/store stress test (Validation);
- update the comments that describe the locks: the `HugePageAllocator` and Windows storage comments
  and the class comment in `TranspositionTable.h`, the `lock_bytes()` comment in `UCIHandler.cpp`,
  and the TT rows in `Docs/Architecture.md`.

`tt_mutex` becomes the class's only lock. It serialises `clear()` calls that the single controlling
thread already serialises. That is existing debt, and this change neither adds to it nor removes it.

**This change will not:**

- change `PackedEntry`'s layout, `Bucket`'s 64-byte size, the bucket count, replacement scoring,
  `sameKeyStoreWins()`, mate normalisation, or what a probe returns at `Threads=1`;
- change `tt_mutex`, `newSearch()` or the ageing scheme;
- make key 0 a storable key. It is the empty sentinel today and stays one; a Zobrist key of 0 has
  probability 2⁻⁶⁴;
- unify the Linux and Windows table storage types (#685) or add Windows large pages (#684);
- add a Catch2 run to the `tsan-linux` job (`Docs/CI.md` explains why it has none).

## Decisions

Number them so a review can cite them individually.

### D1: Two relaxed atomic words per entry, the first holding `key ^ payload`

`PackedEntry` is already exactly an 8-byte key at offset 0 plus an 8-byte payload (value, depth,
move, metadata, age) at offset 8. Each storage slot becomes:

```cpp
struct Slot {
	std::atomic<std::uint64_t> key_xor_data;
	std::atomic<std::uint64_t> data;
};
```

`store()` writes `data` and `key_xor_data = key ^ data`, and nothing relies on the order of the two
stores. `probe()` loads both words and accepts the slot only if `key_xor_data ^ data == key`. Every
access is `memory_order_relaxed`.

**Memory-model argument.** Both words are atomic, so concurrent access is not a data race and has no
UB. A relaxed load returns a value from that word's modification order, so each word is read whole.
Nothing else is published through an entry: the payload is self-contained and no reader dereferences
anything it names. Per-word coherence is therefore all that is needed, and no stronger ordering is
bought. The model guarantees per-word atomicity, not a snapshot of the pair.

**Mixed pairs.** A slot's two words can come from different stores in two ways:

- *Transiently:* a probe's two loads straddle a store.
- *Persistently:* two stores into one slot interleave. If A writes `data_A`, then B writes `data_B`
  and `key_B ^ data_B`, then A writes `key_A ^ data_A`, the slot holds
  `(key_A ^ data_A, data_B)`. That state lasts until the slot is overwritten or cleared, and is
  visible to every later reader. No memory order makes two stores one transaction. D1 accepts this
  state rather than preventing it.

Either way the slot decodes to a pseudo-key `key_X ^ Δ`, where `Δ = data_X ^ data_Y`, carrying
`data_Y`. Probes for `key_X` and `key_Y` miss, which costs only a lost entry. A probe for `K`
false-matches the slot only if `K` equals the pseudo-key. The review's arithmetic probe constructed
such a `K`, so the state is reachable.

**Residual-risk model.** The locked table already false-matches whenever a slot holds another
position's key that equals the probed key in all 64 bits. The low `index_bits` agree by construction,
so under the random-Zobrist model each occupied slot matches with probability about
2⁻⁽⁶⁴⁻ⁱⁿᵈᵉˣ ᵇⁱᵗˢ⁾. Per probe, the rate is at most 4 × 2⁻⁴³ at `Hash=192` and 4 × 2⁻⁴⁰ at the
`Hash=1536` cap (2²⁴ buckets).

Under the same model a pseudo-key matches with the same per-slot probability. A mixed pair changes
*which* key a slot claims, not *how many* slots can claim the probed key, so the per-probe bound is
unchanged and the added rate is about zero. That is how #747's criterion — an added rate below the
existing collision rate — is met.

This is a **model estimate, not a demonstrated bound**. Its limits:

- **Independence is assumed (A2).** `Δ` is structured: it is non-zero only in the payload fields
  that differ. So a pseudo-key is a real key with a few bits flipped. It equals another real
  position's key only if an XOR of Zobrist constants equals `Δ`. The model treats that as no likelier
  than any fixed 64-bit value. That is not measured.
- **Exposure is unmeasured.** How many slots hold mixed pairs at any time is not known. The model
  needs no exposure figure because a mixed slot replaces an occupied one, but this is why the claim
  stays a model.
- **Repeated probes are correlated.** A persistent pseudo-key that does match a probed position can
  match it again until overwritten. An ordinary collision behaves the same way.

The owner accepts this residual at approval. That acceptance is what #747's restated criterion
records.

**Rejected:**

- *Plain (non-atomic) racy reads, Stockfish-style.* A data race is UB, and the `tsan-linux` job would
  fail. D1 should get the same machine code legally; A1 checks that.
- *One 16-byte atomic.* Lock-freedom and codegen depend on the target and library. By default the
  MSVC STL does not report `std::atomic` of a 16-byte type as lock-free, and GCC routes it through
  `libatomic` out of line. Either way it is not the plain inline access D1 gets. This is not probed
  on these exact builds; the rejection rests on portability and predictability, not on a measured
  cost.
- *A seqlock with a side array of versions.* It fits the 64-byte bucket, but every probe would touch
  a second array again. The profile places most of the Linux cost on the first touch of the separate
  lock array (64% of `rdlock` samples). That locates the cost; it does not measure what a smaller
  version array would save.
- *Striped or 1-byte locks.* A small lock array could become cache-resident and recover much of the
  Linux cost, but that is an expectation, not measured. Every probe would still pay an atomic
  read-modify-write, and stripes add false contention at higher thread counts.
- *Skipping locks at `Threads=1`.* Cheap, but it leaves two code paths and allocates locks it never
  uses. It does nothing under Lazy SMP, and correctness would depend on a mode flag tracking the
  thread count.
- *`std::atomic_ref` over the existing plain `PackedEntry` storage.* It needs no type change, but it
  relies on discipline at every access: a stray plain read compiles and races. Atomic members make
  the type enforce the rule.

### D2: `PackedEntry` stays the value type; a slot converts with `std::bit_cast`

`PackedEntry` keeps its layout, its `static_assert`s and its accessors. Three private helpers:

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

- `std::is_trivially_copyable_v<PackedEntry>`, which `std::bit_cast` requires (`Move` is a pure
  two-byte value);
- `std::atomic<std::uint64_t>::is_always_lock_free`;
- `sizeof(Slot) == sizeof(PackedEntry)`, with `sizeof(Bucket) == 64` kept.

`Slot` default-initialises both words to `encode(PackedEntry{})`, so an untouched or cleared slot
decodes to exactly today's `PackedEntry{}`: key 0, move `0xFFFF`, metadata `0x10`. The stored bytes
differ from today's: the key word now holds the non-zero encoded payload. Construction still
initialises and touches the whole allocation, so the Linux huge-page reasoning in the allocator
comment is unchanged.

### D3: Remove `entry_count` and `pv_count`; `clear()`'s early-out reads a written flag

Without the bucket lock, the read-decide-adjust sequence in `store()` cannot keep the counters exact:
two stores into one bucket can both see an empty slot and both increment. Only `TTTests.cpp` reads
`count_entries()` and `count_pv_nodes()`. No engine, UCI or script consumer exists (grep at
`79c3217`, confirmed by review). So they are removed, which also removes a `fetch_add` on a shared
cache line from the store path under Lazy SMP.

`clear()` used `entry_count == 0` to skip zeroing a table that holds nothing. It now reads
`std::atomic<bool> written_since_clear`:

- `store()` sets it after a completed publication, never on a declined store. It does a relaxed load
  first and stores only when the flag is false, so after the first store the flag's cache line is
  only read.
- `clear()` resets it.
- `clear()` already requires that no search stores concurrently, so the flag is exact whenever
  `clear()` reads it. The return value keeps its meaning: whether entries were removed.

**Rejected:** approximate counters. They would keep a shared-line read-modify-write per store for a
diagnostic nobody reads.

### D4: `store()` decides on per-slot snapshots; races can lose an entry or leave a mixed pair

`store()` loads the four slots once each (D2's `load`), runs the existing replacement and
`sameKeyStoreWins()` logic on those snapshots unchanged, and publishes one slot. Under concurrency
there are four outcomes:

- two stores pick the same slot and one entry is lost;
- the two stores interleave word by word and leave a persistent mixed pair (D1);
- a decision rests on a slot that changed after it was read;
- the same-key path inherits `best_move` from a snapshot whose key matched.

A mixed pair is treated by later stores as an ordinary occupied slot holding its pseudo-key, and is
ranked and evicted normally. Its residual risk is D1's. At `Threads=1` every snapshot is exact, so
every decision is identical to today's.

### D5: A wrong payload can corrupt search, never produce an illegal move

`best_move` from a probe is a search hint. It is only matched against moves the engine generated
itself: it ranks them (`ScoreMovesBestFirst`), gates singular-extension eligibility and exclusion,
and exempts a move from some pruning (`AIPerplex.cpp:703-731`, `:858-862`, `:886`, `:947-962`). The
move actually played always comes from the generated list and passes the legality check, never
straight from the TT. Quiescence does not read `best_move` at all.

So D1's residual case has an ordinary hash collision's effect: a wrong score, bound or hint. That can
change a subtree and propagate into ancestor results, but it cannot make the engine play an illegal
move. That legality guarantee must stay true. The source comment on `probe()` will say that a
returned entry may belong to another position.

### D6: `hashfull()` and `clear()` take no per-bucket synchronisation

`hashfull()` decodes its 250-bucket sample with `load()`. Under concurrent stores it may count a slot
that is mid-store either way, which is within the sample's own error. `clear()` publishes
`PackedEntry{}` into every slot. Its caller contract — no search can store concurrently — is
unchanged, and `tt_mutex` still serialises concurrent `clear()` calls.

## Assumptions I cannot verify from the code

- **A1: relaxed `std::atomic<uint64_t>` loads and stores compile to plain `mov` on clang-cl and
  GCC 15, with no fence or `lock` prefix.** This is the expected x86-64 codegen for both, but it is
  not verified on these builds. If it is false, the nps gain shrinks. It is settled by a **required**
  disassembly check of the Release builds: no `lock`-prefixed instruction or `mfence` comes from the
  TT. On clang-cl, `probe` is inlined into `pvs` and `quiescence`. A timing result is not evidence
  for this.
- **A2: the non-index key bits behave as independent uniform bits with respect to payload
  differences.** Zobrist keys are XORs of fixed-seed random 64-bit constants (`Board.cpp:25-42`), so
  this is plausible, but it is not proven. Probes also depend on search history. D1's rate rests on
  it, and the stress test cannot measure a 2⁻⁴³ event. It stays an accepted model assumption.
- **A3: the 9.4% Windows share is roughly what the change recovers.** The XOR and the second word's
  load add work, and removing the counters' read-modify-write saves some. This is settled only by
  Validation's paired Windows comparison, and nothing ships on the profile number.

## Invariants

1. At `Threads=1`, search is node-identical with identical best moves, because every probe and store
   decision is unchanged.
2. `probe()` returns a payload only for a slot whose words decode to the probed key.
3. An empty slot decodes to `PackedEntry{}`, both after construction and after `clear()`.
4. `clear()` returns whether stored entries were removed.
5. `sizeof(Bucket) == 64`, `alignof(Bucket) == 64` and `sizeof(PackedEntry) == 16`, unchanged.
6. No TT access is a non-atomic concurrent access: the table has no plain member that a search thread
   writes.
7. A probe's `best_move` is only ever matched against generated moves, never executed directly (D5).

## Validation

Engine tier, search-change validation: it changes synchronisation on the hottest shared structure.
**PR readiness and merge readiness are separate.** The implementation PR opens once the first five
items pass. Merging waits for the two multi-thread items as well, unless the owner explicitly waives
them.

`<mb>` below is the PR branch's merge-base with `origin/main` at measurement time, recorded in the PR.
`origin/main` moves, so a fixed `79c3217` would not be the matched baseline.

1. **Single-thread equivalence (Invariant 1).** `Compare-SearchEquivalence.ps1 -After
   <clang-cl Release exe> -BaselineRef <mb>` reports identical node counts and best moves at
   `Threads=1`. This is the single-thread correctness gate, and it is why no single-thread Elo match
   is needed.
2. **Deterministic unit tests (Invariants 2–4).** The fixture writes slot words directly:
   - a mixed pair `(key_A ^ data_A, data_B)` misses for `key_A` and for `key_B`;
   - the synthesized key `key_A ^ data_A ^ data_B` hits and returns `data_B`, which pins D1's
     documented probabilistic semantics;
   - the completed two-writer state from D1 gives those same three results;
   - the existing defaults/clear test passes, with the fixture returning a decoded `PackedEntry` by
     value;
   - the existing "fresh table" and "empty after clear" tests cover Invariant 4. A declined store
     needs a key already in the table, so it cannot reach an empty one. No declined-store clear test
     is added.
3. **Concurrent stress test (Invariant 6, D4).** A `[tt][smp]` test:
   - Setup: `TranspositionTable(0)` is a single bucket, so the full 64-bit key is checked. Four
     threads store and probe a shared pool of 64 non-zero keys for a fixed iteration count. Each
     iteration probes the key it just stored or a random pool key. Each payload is a pure function
     of its key, outside the mate range.
   - Pass: every hit's payload equals that function of the probed key, and at least 10% of probes
     hit, so the test is proved to exercise hits.
   - Positive control: drop the XOR so word 0 holds the raw key and `probe()` compares word 0 alone.
     The deterministic mixed-pair tests in item 2 must then fail, whatever the scheduling; the stress
     test is expected to fail too. Record both in the PR.
   - The stress test complements the deterministic tests. It does not prove D1's production rate.
4. **Static checks and codegen (Invariant 5, A1).** The new and existing `static_assert`s compile,
   and the A1 disassembly check is recorded in the PR.
5. **Speed**, following `measure-strength` → `reference/regression-check.md`:
   - Build the baseline from a detached worktree of `<mb>` through its own `build.ps1 main`, and the
     candidate the same way.
   - Run `Compare-Bench.ps1` on the clang-cl Release pair. **No slowdown** is the ship condition.
   - Claiming a **Speedup** needs that verdict twice: once from a `-Control` series, and once on the
     `New-OrderedBuildPair.ps1` placement-equalised pair. Report the result against the 9.4% ceiling.
   - Report Linux GCC Release nps, measured the same way, before claiming any Linux benefit. It is
     context, not the shipping number.
   - No multi-thread speed claim is made, so none is measured.
6. **TSan.** The `tsan-linux` job stays green over its six multi-threaded UCI scenarios at
   `Threads=4`, `8` and `16`. That covers data races only, not strength.
7. **Multi-thread tactical stability (merge gate).** Run `StratChessEvolved.exe tactical stability
   10 tactical_test_cases.json 4` on the baseline and candidate exes. Pass: the candidate clears the
   suite's 90% threshold, and no position the baseline solves in 10 of 10 runs falls below 8 of 10.
8. **Multi-thread strength (merge gate; owner approves the budget before it starts).**
   - Command: `Run-EloMatch.ps1 -Sprt NonRegression -ReferenceExe <mb exe> -ReferenceTag <mb>
     -CandidateOptions 'option.Threads=2' -ReferenceOptions 'option.Threads=2' -Concurrency 3
     -Games 4000 -Tc 10+0.1`.
   - Bounds: −5/0 Elo, α = β = 0.05.
   - Concurrency: two 2-thread engines per game × 3 games = 12 threads, the dev machine's 12
     physical cores. The script's default of six games assumes single-threaded engines.
     `Threads=4` would allow only one game at a time, so a capped run would take days.
   - At the 4000-game cap an undecided SPRT is **inconclusive, not a pass**. It goes back to the
     owner, who extends it, waives the gate or parks the change.
   - The strength lab cannot cover this because it pins `Threads=1`. A `Threads=1` lab run would
     measure only speed, inflated by the GCC lock; it is optional and not a gate.

## Cost

- **Size:** author's estimate is 50–200 lines, about half of them deletions, in `TranspositionTable.h`
  and `TTTests.cpp`, plus `UCIHandler.cpp` (one comment) and `Docs/Architecture.md` (three rows).
- **Blast radius:** Engine tier. No gate, skill or script changes.
- **Review:** one code review run, plus the search-reviewer agent.
- **Measurement:**
  - Items 5 and 7: about 1–2 hours of local machine time.
  - Item 8: the dominant cost and the owner's call. At 3 concurrent games of about 25 s each, the
    4000-game cap is about 9 h; a decisive SPRT stops earlier.
- **Optional parts:** none left. A1 is now a required check, and items 7–8 are merge gates the owner
  can waive.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 publication scheme, memory-model argument, mixed pairs and the residual-risk model | class comment on `TranspositionTable` and the `Slot` comment |
| D3 why there are no entry counters; the `written_since_clear` contract | comment on the flag and on `clear()` |
| D4 races can lose an entry or leave a mixed pair | comment in `store()` |
| D5 a probe's entry may belong to another position; its move is only a hint | comment on `probe()` |
| lock array gone; TT synchronisation model | `Docs/Architecture.md` TT rows (lines 67, 131, 177) |
| measured Windows and Linux nps, equivalence, tactical and SPRT results | `Docs/Changelog.md` and the PR body |
| review findings and their dispositions | PR body |
