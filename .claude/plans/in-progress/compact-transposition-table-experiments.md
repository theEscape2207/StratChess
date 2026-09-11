# Compact TT storage — Experiment protocol

**Issue:** [#442](https://github.com/theEscape2207/StratChess/issues/442)
**Design:** [representation decisions](compact-transposition-table.md)
**Status:** E1-E4 complete. Layout/equal-capacity evidence supports adoption; Hash=256/1 capacity growth
is documented but unassessed for strength. No strength (Elo) evidence exists; adoption is not decided.
**Source baseline:** `0d9ae52`; updates through `b53d457` change workflow only.

## Controls

Use two binaries from the same source base: A = current storage; P = private 16-byte storage,
aligned 64-byte buckets and natural sizing. At Hash=192 and 3 they have equal bucket counts;
at Hash=256 and 1 P has twice the capacity. No unaligned packed variant or legacy sizing mode.
Pair identical compilers/configurations/STLs/ISA flags. Time clang-cl Release; verify MSVC and
Linux GCC compatibility separately. Record source/diff and binary hashes, machine, OS, commands,
corpus hash, Hash, threads and actual allocation with every result.

## E1: Permanent layout and unit checks

Keep size/offset/alignment assertions and constructor alignment checks in production code (D2),
covered by supported builds and Linux Debug sanitizers. Test all 18 valid enum combinations,
all 256 ages including wraparound, metadata-bit isolation, defaults/clear and full Move flags.
Preserve `[tt]` and `[search][tt]` logical assertions, including evictions and same-key tie branches.

Test pure `constexpr bucket_count_for(H)` for H=0..1536 without allocations against independently
computed old/new formulas. Assert equality where counts match and the deliberate doubling elsewhere,
not equality over the entire domain. Pin 0's one-bucket minimum, power-of-two counts and entry-budget
bounds. A design-time arithmetic enumeration found 513 equal and 1024 doubled cases, including H=0;
this is arithmetic evidence only, not an executed candidate unit test.

Deliberately update geometry expectations (line numbers at baseline):

| Test/source | Intended packed expectation |
|---|---|
| `TTTests.cpp:198–202`, request 256 | 4,194,304 buckets / 256 MiB; retain a non-exact-fit case such as 192 -> 128 MiB so the diagnostic test stays meaningful |
| `TTTests.cpp:224`, bytes per bucket | 64 instead of 96 |
| `UCITests.cpp:244–259`, default | Still advertises 192; allocation 128 MiB, same 2,097,152 buckets; rename exact-fit test wording |
| `UCITests.cpp:269–282`, request 6 | Reports/retains 4 MiB with the same 65,536 buckets |
| `UCITests.cpp:290–299`, requests 5 and clamped 0 | 5 -> 4 MiB / 65,536 buckets; clamped 1 -> 1 MiB / 16,384 buckets; remove sub-MiB wording |
| `TranspositionTable.h:59` and `UCIHandler.cpp:181–183` | Size tripwire moves to packed storage; exact-fit guidance becomes 128/256/512/1024 |

## E2: Equivalence with exercised replacement

Require Threads=1 per-iteration depth, score, nodes, PV, final best move and main/quiescence node
split equality at **Hash=192 and mandatory Hash=3**, initially depth 12. Small Hash must actually
exercise eviction: add a temporary verification-only eviction counter or coverage trace, show
nonzero occupied-slot evictions, and deepen/extend the workload if necessary. Do not assume small
allocation proves coverage. Existing forced-collision TT tests remain mandatory as well.

`Compare-SearchEquivalence.ps1` currently has neither a Hash parameter nor retained-process mode
(`Invoke-FixedDepthSearch`, lines 390–412, launches a fresh process per position). Add narrow harness
support or an experiment-local driver for both. Set Hash/Threads once, confirm `readyok` and actual
bucket count, then issue each position/search only after the preceding `bestmove`. For the retained
sequence, keep one process per binary and send no intervening `ucinewgame` or Hash changes. Replay
the same fixed suite order, including a repeated position, and compare each transcript separately.
Keep fresh-process coverage too; test driver ordering/completion rather than silently weakening it.

Any difference at equal capacity blocks interpretation. Doubled-capacity cases retain semantic
tests but must not be called equivalent. Run full Engine-tier and existing abort/SMP/lifecycle checks;
fixed-depth equality says nothing about interrupted search or deterministic multi-thread behavior.

## E3: Small performance campaign

Start with Hash=192 and 3 only. Reuse `Run-Bench.ps1`'s eight positions and CSVs, extending its missing
Hash control using E2's mechanism. Use absolute binary paths, one warm-up per binary, then ten
alternating paired samples per Hash at Threads=1. Use identical depths and increase them if positions
fall below the 200 ms timing floor. Report per-position and aggregate nodes/engine-time nps, paired
percentage deltas and spread. Record exclusions; do not interpret node reductions as code speed.

At default Hash, add one representative SMP comparison (Threads=4 if physical cores permit,
otherwise 2), ten pairs, with no competing jobs. Report total worker nps, elapsed time and spread;
SMP changes the tree nondeterministically and supplies no equivalence or Elo result.

Record bucket count, entry/lock/allocated bytes and actual mutex size. At equal capacity P must save
32 bytes per bucket with unchanged locks. At default Hash, retain a small five-pair direct-TT timing
check of construction, populated clear and empty clear (setup/seeding outside timers, one warm-up).
This tests changed initialization/encoding code without multiplying lifecycle runs across the matrix.
Array payload is not RSS; collect process memory only if explaining unexpected allocation cost.

No full TT-counter instrumentation for equivalent searches. E2's eviction witness is verification-only;
disable it for timing. Hardware counters are optional from the outset: if available, collect separate
cache/cycle runs to investigate D2's cache hypothesis. Record unavailable evidence explicitly. Inspect
optimized decode/store/clear code if a repeatable timing regression needs explaining. Smaller storage
does not establish free alignment, free decoding or faster lifecycle operations.

## E4: Capacity and strength follow-up

After the equal-capacity gates, run the same paired bench and lifecycle checks at Hash=256 to expose
capacity growth; Hash=1's mapping is already unit/UCI-tested. Explain search changes with existing
occupancy counts; add temporary phase-separated probes/hits/cutoffs and store/eviction counters only
if needed to resolve the capacity result. Collect separately from timing, under existing locks or
thread-local aggregation. Publish extra lock/total memory beside speed; this is not equal-memory A/B.

Shipping natural sizing requires assessment of capacity-changing behavior, not just default results.
Agree reference/candidate hashes, Hash, threads, openings, time control and stopping rule before any
paid strength run; verify the harness actually applies them. For gain claims propose the CI strength
lab (~3 hours, 18/20 CI slots). A local NonRegression SPRT is an optional budgeted screen, not a precise
gain estimate. No mixed compilers or cumulative anchor substitution; retain intervals and discard
batches with time losses, illegal moves or disconnects under the measure-strength skill.

## Decision and evidence

Adopt only with layout/contracts, exercised equivalence, memory and timing evidence, and the required
capacity/strength assessment. A slowdown needs an explicit accepted benefit; an unresolved small
effect is not zero. Retain current storage or defer if evidence/budget is insufficient. Record raw
samples and reasons, following `Measurements/README.md`; do not extend sampling until significance
appears. Harvest findings as specified in the design. This protocol authorizes no paid run.

## Initial prototype evidence — 2026-09-11

Design commit: `317e3e5`. Prototype source is the working engine diff on `4b425cf`, preserved locally
as `build/compact-tt-experiment/prototype.patch`. Both binaries use shipping clang-cl Release on
Windows 11 Pro 10.0.26200, AMD Ryzen AI 9 HX 370 (12 cores / 24 logical processors).

| Binary | SHA256 |
|---|---|
| Baseline | `9ed6c89d79fd74294f8bca53b6a89c386e5cf3cdfe16be313ddf28987f3e613e` |
| Packed | `b14cce4db5d61061ba41d56a1298f737cb231373de0729c5c5655850ed65f9a3` |

E2's experiment-local Python driver passed seven self-tests. At depth 12, Threads=1, every compared
iteration's depth, score, nodes and PV, final bestmove and main/quiescence split matched in all four
cells: Hash=192 and 3, each fresh and retained. Allocation diagnostics confirmed 2,097,152 and
32,768 buckets respectively. Each sequence includes seven searches, with startpos repeated; retained
sessions send no intervening resets. Corpus SHA256 (compact JSON of ordered name/position pairs):
`eb21055e5586bcfdc530dc104c5792fca9e990ac3f524ea33ce848e0edff01b2`.

Reproduce from the repository root:

```text
python build/compact-tt-experiment/compact_tt_driver.py --baseline build/compact-tt-experiment/baseline-clang-cl.exe --candidate build/windows-clang-cl/StratChessEvolved.exe --output build/compact-tt-experiment/equivalence-depth12 --depth 12 --hash 192 3 --mode both --timeout 300
```

Raw transcripts and structured comparisons remain in the ignored
`build/compact-tt-experiment/equivalence-depth12/` directory. A separate temporary test-only atomic
counter incremented on accepted stores where the replaced slot was occupied by a different key.
Startpos, Hash=3, Threads=1, depth 12 produced **1,118,858 occupied-slot evictions** twice. Witness
source, patch and log are preserved under `build/compact-tt-experiment/`; instrumentation was removed
after verification. The rebuilt shipping binary retained the recorded packed SHA256.

Focused TT/search diff review found no actionable correctness issue. These results establish only
the exercised equivalence checkpoint. They do not establish speed or Elo; E3/E4 and Linux Debug
sanitizer validation remain outstanding.

E1 local validation: clang-cl Release engine/tests build and the full extended suite passed
(15,155 assertions / 641 cases), including the permanent packed-layout/metadata, capacity-domain,
TT/search, abort, SMP and lifecycle checks. MSVC Release engine/tests build and fast suite also passed
(15,099 assertions / 638 cases). Formatting
and `git diff --check` passed. The initial test build exposed unsupported chained Catch2 assertions;
those were split into individual checks before these successful runs. The witness counter and
temporary test are absent from the final source and shipping binary.

## Second-session evidence — 2026-09-11

### E1 Linux portability complete

The user supplied WSL `Ubuntu-24.04`. Commit `36e8aab` was exported with `git archive` to
`build/compact-tt-experiment/linux-validation-36e8aab.tar`, then extracted onto native ext4 at
`/tmp/strat-compact-tt-442.RJQMdc`. Nothing was built under `/mnt/c` or from the Windows worktree.
Toolchain: GCC 13.3.0, CMake 3.28.3 and Ninja 1.11.1.

- GCC Release full build and fast suite: 15,099 assertions / 638 cases passed.
- GCC Debug full build and fast suite: 15,094 assertions / 635 cases passed.
- Debug ASan + UBSan + `_GLIBCXX_DEBUG` test build and fast suite: 15,094 assertions / 635 cases passed.
- Debug TSan engine build and repository SMP driver: six scenarios passed with no TSan report.
- Expected injected `sink failure (test)` logging appeared during the passing Catch2 suites.

### E3 paired timing complete

The ignored Python driver reuses `Run-Bench.ps1`'s exact eight positions. It launches a fresh process
per position, applies and verifies Hash/Threads, requires the tree-node split, enforces the 200 ms
floor, alternates A/B order, preserves raw rows, and reports aggregate nps plus paired and per-position
spread. One full warm-up per binary preceded each ten-pair campaign. Both binaries are the preserved
shipping clang-cl Release artifacts and retain the hashes listed above. No builds or other searches ran
concurrently. Single-thread pairs also enforced node/split/bestmove equality.

| Hash | Threads | Depth | Aggregate candidate nps delta, 10 pairs | Interpretation |
|---:|---:|---:|---:|---|
| 192 | 1 | 13 | median **+1.101%**, mean +1.389%, range +0.519% to +3.519%, SD 0.892 pp | Equal capacity; all pairs positive. |
| 3 | 1 | 14 | median **+6.559%**, mean +7.792%, range +4.400% to +13.016%, SD 2.862 pp | Equal capacity, eviction-heavy; all pairs positive. |
| 192 | 4 | 15 | median +2.600%, mean +0.381%, range -16.608% to +10.905%, SD 8.564 pp | Lazy SMP is very noisy; no equivalence or Elo claim. |

The complete JSON contains per-position medians/ranges and every raw row. This establishes a repeatable
single-thread speed improvement at equal capacity, not a strength gain.

### E3 direct lifecycle and memory complete

A temporary Catch2 experiment (removed after use) compared both headers in one clang-cl Release process.
Each of five alternating pairs measured construction, clear after seeding every bucket, and one million
empty clears; one warm-up per layout preceded sampling. Raw data is `lifecycle-h192.csv`.

- Windows `sizeof(std::shared_mutex)`: 8 bytes.
- Hash=192 baseline: 2,097,152 buckets, 192 MiB entries + 16 MiB locks = 208 MiB.
- Hash=192 candidate: same buckets, 128 MiB entries + 16 MiB locks = 144 MiB.
- Construction median paired delta is about -35.6% (candidate faster).
- Populated clear median paired delta is approximately zero; there is no demonstrated improvement.
- Empty clear is approximately 10 ns/call for both and indistinguishable at this resolution.

### E4 complete — Hash=256 paired timing

Restarted from scratch per the handoff's instruction not to extend the cancelled five-pair run. One
warm-up plus ten alternating pairs, Hash=256, Threads=1, depth 13, same preserved shipping clang-cl
Release binaries (hashes unchanged, re-verified before the run). Output:
`build/compact-tt-experiment/timing-h256-t1-d13-full/`. The earlier five-pair cancelled run remains at
`timing-h256-t1-d13/` as interrupted-run provenance only, per the handoff, and is not used below.

**Aggregate candidate nps delta, 10 pairs: median +1.215%, mean +1.750%, range -1.474% to +5.544%,
SD 2.182 pp.** Total node counts differ negligibly between binaries (e.g. 21,513,817 vs 21,514,916 at
Hash=256, Threads=1, depth 13, summed over the eight-position suite) and every pair's per-position
bestmove matched at this depth — but this is **not an equivalence result**: candidate capacity is
4,194,304 buckets against baseline's 2,097,152 (D3's doubling case), so collisions differ and node/
bestmove agreement here is observation, not a guarantee E2 makes only at matching capacity.

### E4 complete — Hash=256 direct lifecycle and memory

Reconstructed the removed temporary Catch2 lifecycle experiment (E3's approach) for Hash=256, since no
source survived the prior session's cleanup — rebuilt from the E3 CSV schema and the design's D2/D3
narrative, preserved this time at `build/compact-tt-experiment/TTLifecycleExperimentTests-h256.cpp`
before removal. Same method: one warm-up per layout, five alternating pairs, construction / clear-
after-seeding-every-bucket / one million empty clears. Raw data is `lifecycle-h256.csv`
(`build/compact-tt-experiment/`).

| Metric | Baseline | Candidate |
|---|---:|---:|
| Buckets | 2,097,152 | 4,194,304 |
| Entry bytes | 192 MiB | 256 MiB |
| Lock bytes | 16 MiB | 32 MiB |
| Allocated bytes | 208 MiB | 288 MiB |
| Construction, median of 5 | 28,960 us | 37,529 us |
| Populated clear, median of 5 | 22,260 us | 44,440 us |
| Empty clear, median of 5 | 9.787 ns/call | 9.770 ns/call |

At this doubled-capacity Hash, **both entry and lock allocations grow for the candidate**, matching the
design's D3 table exactly. Construction and populated clear both track bucket count rather than byte
layout: the candidate is slower on both at Hash=256 (roughly 2x, matching its 2x bucket count) where it
was faster at matching-capacity Hash=192 (E3's -35.6% construction delta). This is expected, not a
regression in the packed code path — the H=192 result isolates the layout change; H=256 additionally
changes how much table exists. Empty clear remains indistinguishable at this resolution for both.

One run had a same-process outlier at pair 5 (candidate: 72,965 us populated clear, 17.14 ns/call empty
clear, versus ~44,000-45,000 us / ~9.7-9.8 ns/call on every other candidate sample in both runs); a
repeat clean run reproduced the same pair-5-candidate anomaly (this document reports the second, clean
run's data above). The first attempt overlapped the tail of the concurrent E4 timing campaign and was
discarded for that reason; the second had no other process running. The anomaly's repeatability across
both runs suggests a real effect — plausibly allocator/heap fragmentation from repeated large (~300 MiB)
alloc/free cycles rather than system noise — but it was not root-caused, consistent with not extending
sampling past what the protocol requires. It does not change the interpretation above.

### Interpretation after E4

Equal-capacity results (Hash=192 and 3) support the layout: less memory at unchanged capacity, faster
single-thread search, faster construction, no populated-clear regression. Hash=256 shows a net-positive
but noisier single-thread search delta (median +1.2%) bought at roughly double total memory and roughly
double construction/clear cost — a capacity-changing outcome, not a free win, exactly as D3 anticipates
and the design's adoption criteria require flagging. No strength (Elo) evidence exists at any Hash; the
CI lab run required for a gain claim has not been authorized or launched.
