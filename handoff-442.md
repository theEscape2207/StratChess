# Handoff: #442 compact transposition table

Checkpoint: 2026-09-11. Paused at the user's request to conserve token budget.
The prototype is implemented and initial correctness experiments pass; it is **not approved for adoption**.

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

## Next steps, in order

1. Complete E1 portability: Linux GCC / Debug sanitizers remain outstanding. No usable WSL distribution
   was found locally. Do not claim Windows tests replace Linux validation. Remaining Engine-tier gates,
   including full lint/tidy and pre-PR checks, must run before adoption/PR completion.
2. E3: prepare the bounded timing campaign from the approved protocol. Reuse Run-Bench's eight positions;
   the existing experiment driver currently covers equivalence only. Add Hash control, timing capture and
   paired reporting in the ignored experiment harness or narrowly extend supported tooling (PowerShell
   edits require `write-powershell`). No permanent general-purpose harness refactor is needed.
3. Run one warm-up and ten alternating paired samples at Hash=192 and 3, Threads=1. Enforce the 200 ms
   position timing floor, report per-position and aggregate nps plus paired spread. No concurrent builds
   or searches while timing. Add ten default-Hash SMP pairs at Threads=4 and five direct-TT lifecycle
   pairs (construction, populated clear, empty clear). Measure actual mutex size and payload allocations.
   Counter instrumentation stays disabled; hardware counters are optional and currently unavailable evidence.
4. E4 after equal-capacity gates: paired bench/lifecycle at Hash=256, explicitly explaining changed capacity
   and lock/total memory. Search equivalence at the default does not settle this case.
5. Strength assessment needs a separately agreed budget and controls. No local SPRT or CI lab has run.
   A gain claim requires the CI lab (~3 hours, 18/20 slots); do not launch it unilaterally. No speed or Elo
   gain is established yet. Defer/reject adoption if evidence remains insufficient.
6. Harvest results to the destinations listed in the design, then follow `open-pull-request` if authorized.
   Retain the in-progress plans until lifecycle conditions permit removal. Remove this execution handoff
   once consumed; durable decisions belong in the design/contracts and measured evidence destinations.

## Review context

The new cross-agent-review workflow (#524) was applied before the design commit. Nine review findings
were accepted and one rejected: guaranteed lifecycle improvement was rejected because smaller storage
does not prove faster initialization/clear; a small direct timing check remains mandatory. Private storage
and natural sizing reflect accepted review changes. The ignored review/dispositions remain at
`.claude/plans/in-progress/compact-transposition-table.review.md`; preserve until the artifact lands and
carry the rejection rationale into the eventual PR body. Do not commit the ignored review file.
