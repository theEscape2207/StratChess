# Handoff: #442 compact transposition table

Checkpoint: 2026-09-11, second pause. Paused at the user's request to conserve the 5-hour token budget.
The prototype is implemented; portability, equivalence and equal-capacity timing experiments pass. It is
**not approved for adoption** because Hash=256 and strength/adoption assessment remain incomplete.

## Resume efficiently

Read `CLAUDE.md`, then use the `exec-plan` and `measure-strength` skills. Read the two approved
documents below; use this handoff instead of replaying the previous conversation. Keep reads and
tool output focused. Delegate bounded implementation work to Terra or Luna where cost efficient,
with explicit ownership and fresh prompts. The controller owns acceptance and measurement interpretation.

- Branch: `codex/compact-tt-design` in `C:/Users/thees/source/repos/StratChessEvolved`.
- Design commit: `317e3e5`; workflow-only main merge: `4b425cf` (main through `b53d457`).
- This checkpoint commit contains the prototype, tests, plan lifecycle moves and this handoff.
- [Approved design](.claude/plans/in-progress/compact-transposition-table.md).
- [Experiment protocol and initial evidence](.claude/plans/in-progress/compact-transposition-table-experiments.md).
- No push, PR, paid strength run or adoption decision has been authorized. No background jobs remain.

## Implementation and contract

`StratEngine/TranspositionTable.h` now stores private 16-byte `PackedEntry` objects in aligned
64-byte four-entry buckets. Public decoded `TTEntry`, probe/store signatures and search call sites
are unchanged. Full key, signed score/depth, complete two-byte Move and all eight age bits survive.
Metadata packs phase/bound/node type; its upper three bits remain reserved, default metadata `0x10`.
Size, offsets, enum encodings and alignment are asserted. Debug checks vector allocation alignment.

Replacement ranking/ties, same-key move retention, mate normalization, locking, counters and clear
semantics are preserved. Existing helpers consume decoded entries; decoding cost is still unmeasured.
`bucket_count_for()` exposes pure constexpr natural sizing. No rule-50 policy or lock redesign belongs here.

The UCI default request stays 192 MiB. At Hash=192, capacity stays 2,097,152 buckets while entry
payload falls from 192 to 128 MiB. Hash=3 also preserves capacity (32,768 buckets). Hash=256 and 1
double capacity; those cases must not be described as search-equivalent. Additional locks can increase
total allocation in doubled-capacity cases. `UCIHandler.cpp` updates exact-fit guidance accordingly.

`TTTests.cpp` and `UCITests.cpp` cover metadata combinations/isolation, defaults/clear, ages/wrap,
Move flags and updated geometry. Exhaustive H=0..1536 arithmetic confirms 513 matching-capacity
and 1,024 doubled-capacity cases. Temporary eviction instrumentation has been removed.

## Verified checkpoint

- clang-cl Release engine/tests build: passed.
- clang-cl extended suite: 15,155 assertions / 641 cases passed.
- MSVC Release engine/tests build and fast suite: 15,099 assertions / 638 cases passed.
- Formatting and `git diff --check`: passed before checkpoint preparation; commit validation runs again.
- Terra's focused TT/search diff review: no actionable correctness findings. No Elo inference accepted.
- Experiment driver: seven self-tests passed.
- Depth-12, Threads=1 comparison: every iteration's depth, score, nodes and PV, final bestmove and
  main/quiescence split matched at Hash=192 and 3 in both fresh and retained processes.
  Each cell used seven positions including repeated startpos; allocation and `readyok` were checked.
- Separate test-only counter: startpos, Hash=3, Threads=1, depth 12 caused 1,118,858 occupied-slot
  evictions, reproduced twice. Thus the equivalence workload genuinely exercises eviction.
- Rebuilding after counter removal produced the same shipping candidate binary hash.

The initial new tests failed to compile because Catch2 rejects chained `CHECK(a && b)` expressions;
they were split into individual assertions. This was fixed before the successful runs above.
Expected injected `sink failure (test)` logging appeared during passing suites.

## Local artifacts — ignored, not carried by a clone

Preserve `build/compact-tt-experiment/` in this workspace. It contains:

- `baseline-clang-cl.exe`: baseline engine from `c5c3502` (engine source identical to `0d9ae52`).
- `candidate-clang-cl.exe`: preserved prototype; also built at `build/windows-clang-cl/StratChessEvolved.exe`.
- `compact_tt_driver.py`, `test_compact_tt_driver.py`: fresh/retained UCI driver and self-tests.
- `equivalence-depth12/result.json` and raw transcripts: four passing comparison cells.
- `TTExperimentTests.cpp`, `prototype-with-witness.patch`, `eviction-witness.txt`: temporary counter evidence.
- `prototype.patch`: engine diff without instrumentation; `BaselineTranspositionTable.h`: old header.
- `linux-validation-36e8aab.tar`: committed checkpoint exported for native-ext4 WSL validation.
- `compact_tt_bench.py`, `test_compact_tt_bench.py`: paired timing driver and its 12 passing self-tests.
- `timing-h192-t1-d13/`, `timing-h3-t1-d14/`, `timing-h192-t4-d15/`: completed E3 raw JSON/CSV.
- `lifecycle-h192.csv`: completed five-pair direct-TT lifecycle data.
- `timing-h256-t1-d13/`: five-pair **cancelled partial** E4 run; preserve but do not report as the
  protocol's ten-pair result. Use a new output directory when restarting E4.

SHA256:

| Artifact | Hash |
|---|---|
| Baseline exe | `9ed6c89d79fd74294f8bca53b6a89c386e5cf3cdfe16be313ddf28987f3e613e` |
| Candidate exe | `b14cce4db5d61061ba41d56a1298f737cb231373de0729c5c5655850ed65f9a3` |
| Ordered equivalence corpus, compact JSON | `eb21055e5586bcfdc530dc104c5792fca9e990ac3f524ea33ce848e0edff01b2` |

Machine: Windows 11 Pro 10.0.26200; Ryzen AI 9 HX 370, 12 cores / 24 logical processors.
Both compared binaries are shipping clang-cl Release. Never time MSVC against clang-cl.
The experiment protocol records the exact driver command. Existing evidence need not be rerun unless
the source, harness or controls change. If moving to another workspace, copy ignored artifacts explicitly
or reconstruct them; the tracked source and result summary alone do not preserve the driver.

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

### E4 partial, not accepted

The driver now understands Hash=256's unequal expected capacities and disables equivalence enforcement
only for this declared capacity-changing case. A depth-13 pilot met the timing floor. The full campaign
was stopped at the user's request after five of ten pairs: -0.122%, +0.335%, +2.042%, +0.285%, +0.087%.
These adaptive partial results are preserved only as interrupted-run provenance and must not be treated
as the E4 result. No Hash=256 lifecycle campaign has run.

## Next steps, in order

1. Finish E4 from scratch in a **new output directory**: one warm-up and ten alternating pairs at
   Hash=256, Threads=1, depth 13. Do not append to or summarize the cancelled five-pair directory.
   Report node/wall-time changes as capacity-changing search behavior, not equivalence. Then reconstruct
   the temporary direct-TT experiment for five Hash=256 lifecycle pairs; candidate capacity is 4,194,304
   buckets versus baseline 2,097,152, so entry and lock allocations both change.
2. Record E1/E3/E4 evidence in the approved experiment document. Run remaining Engine-tier gates,
   including full lint/tidy and `Validate-PrePR.ps1`, before adoption or PR completion. The temporary
   lifecycle test source is already removed and `git status` is clean.
3. Interpret adoption only after E4. Equal-capacity results support the layout, but natural sizing also
   changes capacity at Hash=256/1. Retain or defer if the total evidence does not justify that behavior.
4. Strength assessment needs a separately agreed budget and controls. No local SPRT or CI lab has run.
   A gain claim requires the CI lab (~3 hours, 18/20 slots); do not launch it unilaterally. No speed or Elo
   gain claim is licensed from the timing campaign alone. Defer/reject adoption if evidence remains insufficient.
5. Harvest results to the destinations listed in the design, then follow `open-pull-request` if authorized.
   Retain the in-progress plans until lifecycle conditions permit removal. Remove this execution handoff
   once consumed; durable decisions belong in the design/contracts and measured evidence destinations.

## Review context

The new cross-agent-review workflow (#524) was applied before the design commit. Nine review findings
were accepted and one rejected: guaranteed lifecycle improvement was rejected because smaller storage
does not prove faster initialization/clear; a small direct timing check remains mandatory. Private storage
and natural sizing reflect accepted review changes. The ignored review/dispositions remain at
`.claude/plans/in-progress/compact-transposition-table.review.md`; preserve until the artifact lands and
carry the rejection rationale into the eventual PR body. Do not commit the ignored review file.
