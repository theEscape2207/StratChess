# Compact TT storage — Experiment protocol

**Issue:** [#442](https://github.com/theEscape2207/StratChess/issues/442)
**Design:** [representation decisions](compact-transposition-table.md)
**Status:** Revised after review; all prototype/measurement gates below are **not run**.
**Source baseline:** `0d9ae52`; #524 at `c5c3502` changes workflow only.

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
