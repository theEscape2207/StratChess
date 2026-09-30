# Transparent huge pages for the TT — Design

**Issue:** #676 (option 1)

## Goal

On a Linux host whose transparent-huge-page mode is `madvise` (ubuntu-26.04 runners, WSL, most
current desktop distributions), the TT is backed by 4 KiB pages. Building it zero-fills through
tens of thousands of page faults, and every probe pays a TLB miss over a 240 MiB working set (128 MiB
of entries plus 112 MiB of `std::shared_mutex` on libstdc++). ubuntu-24.04 hid this by running THP
`always`. The strength lab (#476 step 4) and Linux users run the full-size table, so both costs reach
them.

Spike on WSL Ubuntu-26.04 (THP `madvise`, GCC 15.2, default 192 MB request), one binary, madvise
toggled by an environment variable, rounds alternated:

| | 4 KiB pages | huge pages |
|---|---|---|
| engine construction (process start to `readyok`) | 85–88 ms | 18–23 ms |
| bench nps, depth 12, 10 rounds | 1.920 M (1.906–1.936) | 1.987 M (1.965–2.002), +3.5% |
| bench nps, depth 14, 4 rounds | 1.945 M (1.921–1.948) | 1.987 M (1.967–1.992), +2.1% |
| `tactical stability 10`, wall clock, two runs each | 36.8 / 36.5 s | 8.0 / 7.9 s |

Node counts and best moves are identical in both modes. `AnonHugePages` read 240 MiB with madvise
and 0 without.

## Review focus

- D3: the 2 MiB threshold. Below it, test engines must keep their current allocation. A 1 MiB table
  rounded up to a huge page would double each test engine's zero-fill, and the fast tier builds
  thousands of them.
- Assumption A2: `defrag=madvise` makes a fault in an advised region compact memory synchronously.
  On a fragmented host, construction could stall instead of speeding up.

## Scope

**This change will:**

- back the TT entries and the per-bucket lock array with 2 MiB-aligned memory advised
  `MADV_HUGEPAGE` on Linux, when the allocation is at least 2 MiB;
- add one test that a Linux allocation at or above the threshold is 2 MiB-aligned.

**This change will not:**

- use Windows large pages. They need `SeLockMemoryPrivilege`, which a normal user account lacks, and
  Windows has no transparent equivalent. The shipping Windows build keeps its current allocation;
  #684 tracks an opt-in.
- use `MAP_HUGETLB` / hugetlbfs. It needs a reserved pool that no default host has.
- add a UCI option or environment switch for it. The spike's toggle is removed.
- move `strength.yml` to 26.04. That is #476 step 4, next.

## Decisions

### D1: `aligned_alloc(2 MiB)` plus `madvise(MADV_HUGEPAGE)`, through a stateless allocator

`TranspositionTable` gets `HugePageAllocator<T>`. Both `table` (`std::vector<Bucket, …>`) and
`bucket_locks` use it. `allocate()` and `deallocate()` call two non-template functions defined in
`TranspositionTable.cpp`, so `<sys/mman.h>` stays out of the header that `AIPerplex.h` includes.

The spike measured entries and locks together. The locks are covered because on libstdc++ they are
nearly half the footprint and are touched on every probe.

Rejected:

- **`mmap` directly.** It is more code for the same effect: the size must be tracked for `munmap`,
  and the zero pages it returns are not useful, because `PackedEntry{}` is not all-zero (`metadata`
  is `0x10`).
- **`GLIBC_TUNABLES=glibc.malloc.hugetlb=1`.** It works (7.9 s fast tier in the #675 probe), but it
  is per-process configuration that every lab runner and user would have to set.

### D2: the lock array becomes `mutable std::vector<std::shared_mutex, HugePageAllocator<…>>`

Today it is `std::unique_ptr<std::shared_mutex[]>`, whose `operator[]` is shallow-const, which is
what lets `probe() const` lock. A vector is deep-const, so it is `mutable`, the standard idiom for a
mutex member.

Rejected: keeping the `unique_ptr` with a custom deleter and placement-new of each mutex. That
duplicates what the vector already does.

### D3: huge pages only for allocations of at least 2 MiB

Below 2 MiB the allocator uses the plain aligned path: `operator new` with `align_val_t`, as the
vector does today. The size is rounded up to a 2 MiB multiple only on the huge-page path.

Test engines use a 1 MiB table (#679). Rounded up and advised, the first touch would fault a whole
2 MiB page and zero it: twice today's work for every test engine.

### D4: failures

- `aligned_alloc` returning null throws `std::bad_alloc`. `SetHash()` already catches it and keeps
  the old table.
- A failing `madvise` is ignored, because the advice is advisory. The table is then 4 KiB-backed,
  as it is today.

### D5: non-Linux platforms unchanged in behaviour

On Windows (and any other non-Linux build) `allocate()` is aligned `operator new`, exactly what
`std::vector<Bucket>` does for the over-aligned `Bucket` today. The only shipping-binary change is
the lock container type (D2).

## Assumptions I cannot verify from the code

- **A1: the gain holds on GitHub's ubuntu-26.04 runners, not only WSL.** To be verified by a
  dispatched Nightly: `tactical-stability` should fall from 470–477 s to the 24.04 range
  (93–169 s), with search time unchanged.
- **A2: synchronous compaction under `defrag=madvise` does not stall construction on a fresh
  runner.** WSL shows 18–23 ms. The Nightly run above measures it on a runner. A fragmented
  long-running desktop could see a slower first build of the table; that is a one-off cost at
  construction or `setoption Hash`, never during search.
- **A3: glibc `aligned_alloc` with 2 MiB alignment returns a 2 MiB-aligned block.** POSIX guarantees
  it. The new test checks it.
- **A4: sanitizer runtimes accept `aligned_alloc` plus `madvise`.** ASan and TSan intercept
  `aligned_alloc`. `sanitize-linux` and `tsan-linux` in the PR run cover it.

## Invariants

- Identical node counts and best moves at `Threads=1` (the TT's contents and indexing are
  untouched).
- `SetHash()` failure still returns `{false, …}` and keeps the old table.
- Allocations below 2 MiB are not rounded up or advised.
- Windows nps within noise of `main`.

## Validation

Engine tier: `Validate-PrePR.ps1`, plus:

- `Compare-SearchEquivalence.ps1` on the Windows build, which must be identical;
- `Run-Bench.ps1` on Windows, where nps must be within noise;
- the WSL 26.04 A/B bench repeated on the final code against `origin/main`: at least 10 alternating
  rounds at depth 12, where nps must gain with non-overlapping ranges;
- a dispatched Nightly on the branch for `tactical-stability` (A1, A2).

No Elo match. The search is node-identical, so the only effect is speed, and +2–3.5% nps on Linux
is below what the lab resolves. The lab runs on Linux, so its own before/after comes free with #476
step 4.

## Cost

- **Size:** about 60 lines in `TranspositionTable.h/.cpp`, plus one test.
- **Blast radius:** Engine tier. `Docs/CI.md`'s THP paragraph changes. No script or skill changes.
- **Review:** code-review (170–270k tokens), plus `search-reviewer`, which is not triggered by
  `TranspositionTable.h`; state that in the PR.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| why madvise, why the locks too, the 2 MiB threshold (D1, D3) | source comment on `HugePageAllocator` |
| `mutable` lock vector (D2) | self-evident idiom, no comment |
| spike and final bench numbers, tactical-stability before/after | `Docs/Changelog.md`, PR body, #676 |
| engine now advises huge pages, so THP mode no longer matters for the full-size table | `Docs/CI.md` THP paragraph |
| **Changed in implementation: D5.** The shared storage types cost 1.6% Windows nps with identical nodes (disjoint ranges, 6 rounds). Reverting only the lock array recovered half. So non-Linux builds keep `main`'s `std::vector<Bucket>` and `std::unique_ptr<std::shared_mutex[]>` behind `#if defined(__linux__)`, and the allocator is Linux-only; Windows then benched +0.2%. The cause is open in #685. | source comment on the `#else` members, PR body, #685 |
