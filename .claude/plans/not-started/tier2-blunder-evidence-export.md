# Tier 2 blunder evidence export — Design

**Issue:** #484

**Status:** Approved after cross-agent review on 2026-09-07; implementation not started. No implementation or measurement authorized by this document.

**Scope agreed:** Fully specify export A; define the boundaries of B/C.

**Code inspected:** main `da974148cebdb92e1fc08ae3781a6ca5608c46e7`.

**Location:** `.claude/plans/not-started/tier2-blunder-evidence-export.md`.

## Goal

Turn a Tier 2 scan into a reusable collection of identifiable blunder observations, with the
information needed to sample them and replay their positions. Today the scanner discards raw scores
and metadata and exposes only aggregate counters plus a truncated worst-row list. Capturing the
evidence now lets later agents implement sampling and replay against a fixed data contract.

The design deliberately settles the statistical and serialization decisions here. Implementers
should not have to decide what a blunder means, reconstruct unavailable history, invent a mate
conversion, or infer why the engine selected a move.

## Scope

This change adds `--worst-jsonl PATH` and optional `--source-run TEXT` to
`Scripts/analyze_external_quality.py`. It exports every existing eligible Tier 2 row whose
**legacy clamped loss is at least 150 cp**, including self-admitted blunders.

It preserves Tier 2's population, search order, cache, reset behavior, counters, report and existing
`--json` output. It adds no oracle searches. The existing human-readable worst-row list stays as-is.

Sampling, deeper confirmation, StratChess replay, mechanism attribution, full-corpus execution,
engine changes and changes to #481/#483's measurement definitions are outside this implementation.
Export is restart-from-scratch; resumable engine analysis belongs to B.

## Decisions

### D1: One self-contained JSONL stream

Use three record types, in this order:

1. Exactly one `manifest`.
2. Zero or more `blunder` records.
3. Exactly one final `complete` record.

Each record is one UTF-8 JSON object followed by LF. Set `allow_nan=False`; use a fixed field order,
compact separators, and `ensure_ascii=False`. Every record has `schema_version: 1`.
The writer belongs to the parent process; workers never append to the file.

Open PATH with exclusive creation before launching workers. Refuse an existing file, a missing
parent directory, or a collision with the existing `--json` destination. Do not add overwrite or
resume flags. Reject --source-run when --worst-jsonl is absent. Fail before expensive work on a bad output path.

After all selected input has been scored and all export rows/counts reconcile, append complete,
flush and close the artifact **before** report()/bootstrap or aggregate --json output. Report or
aggregate-output failures still exit nonzero but leave the completed scan artifact usable. Completion
is a property of scoring/export, independent of presentation.

Flush at each completed worker result. A scoring/export exception or interruption before completion
retains the partial file and returns failure. A later reader must reject missing/duplicate completion,
records after completion, mismatched counts, malformed JSON or an unsupported schema version. The
footer marks logical completion, not a power-loss durability guarantee.

A successful scan with zero exported rows is a valid empty artifact, including zero eligible rows.
Keep the existing CLI error for no eligible rows; its completed artifact records the empty result.
No input PGNs is a preflight failure. Document retries: use a new output path, or explicitly remove
or rename the partial file before retrying; exclusive creation will not overwrite it.

**Alternatives:** Separate manifest/row files need cross-file consistency handling. A database adds
transactions, queries and dependencies that this sequential export does not need. Repeating a move
prefix in selected rows is an acceptable simplicity tradeoff; do not compress or normalize it into
another file in v1.

### D2: Freeze the selection population

Eligibility is the current `extract()` contract:

- Finished game accepted by the existing parser/replay.
- Numeric current-mover score and numeric score on its next turn, two plies later.
- Absolute current-mover score <=150 cp.
- All eligible rows are oracle-scored in the current order.
- Export when `max(0, legacy_before_cp - legacy_after_cp) >= 150`.

The self-swing does not restrict export. The display threshold of 300 and five-per-game/twenty-global
limits do not restrict export. Selection uses the legacy clamped loss, even when raw finite loss
would put a row in a different population.

Keep the existing treatment of missing annotations, game results and corrupt games. A corrupt game
contributes no rows. An unrecognised annotation remains fatal. Do not change draw detection or send
move histories to the Tier 2 oracle as part of this work.

**Reason:** Changing the filter while adding export would make a mismatch indistinguishable from
an exporter defect. Rows hidden by legacy clipping and the last-two-plies exclusion remain outside
this sample; consumers must state that limit. For every eligible row, also count the finite-score
clipping blind spot: legacy_loss_cp <150 and raw_loss_cp >=150. This counter adds no oracle searches,
does not export those rows and does not alter the existing counters. It measures this threshold
crossing within eligible finite-score observations, not mate losses or other selection exclusions.

### D3: Preserve typed endpoint scores

All exported oracle scores use the **original mover's** perspective, including the after-position.

`RawScore` has exactly one of these JSON shapes:

- Finite: `{"kind":"cp","value":-350}`.
- Mate: `{"kind":"mate","moves":3,"winner":"opponent"}`.

Mate `moves` is a nonnegative integer in the oracle's UCI mate-distance units.
`winner` is `"mover"` or `"opponent"`; it keeps mate-in-zero unambiguous.
For an already checkmated board, moves=0 and the winner is the side that was not mated.
Terminal legacy_cp is assigned directly: checkmate is -1000 if the original mover was mated and
+1000 otherwise; stalemate and insufficient material are 0. Terminal best_move_uci is null.
Stalemate and insufficient material have raw finite zero; checkmate has raw mate-in-zero.

Each endpoint has:

| Field | Meaning |
|---|---|
| `score` | RawScore above |
| `legacy_cp` | The current oracle_eval centipawn projection, clamped to [-1000,1000] |
| `finite_clipped` | True only for a finite raw score strictly outside [-1000,1000] |
| `source` | `search`, `checkmate`, `stalemate` or `insufficient_material` |
| `best_move_uci` | First oracle PV move, or null when absent/terminal |

For searched positions only, preserve the current python-chess
`score(mate_score=MATE_CP)` conversion followed by the +/-1000 clamp for legacy_cp.
Terminal values follow the direct assignments above.

Do not export a bound/exactness field or change engine.analyse() to streaming analysis. Python-chess
merges info dictionaries, so a lowerbound/upperbound flag from an earlier line can survive a later
exact score. Omitting that field avoids publishing false provenance while retaining Tier 2's current
oracle interaction. The scores remain depth-limited observations; v1 makes no certified-exact claim.

Each row has these derived fields:

- `legacy_loss_cp = max(0, before.legacy_cp - after.legacy_cp)`.
- `raw_loss_cp = max(0, before.score.value - after.score.value)` only when both endpoints are
  finite; otherwise null.

Exactly +/-1000 is not clipped. Do not store loss_stratum or band counts: every selected row is
present, so B1 can derive its own disjoint bands and their frequencies from these observations.
A's schema does not choose B1's sampling design.

### D4: Every exported row identifies and reconstructs its position

A blunder record contains these required fields. Nullable fields are present as JSON null.

| Field | Type / exact convention |
|---|---|
| `type`, `schema_version` | `"blunder"`, 1 |
| `row_id` | `input_index:game_index:ply_index`, decimal integers separated by colons |
| `input_index` | Zero-based index in manifest.inputs |
| `game_index` | Zero-based ordinal yielded by parse_games, before filtering/skipping |
| `ply_index` | Zero-based move index from the PGN setup position, including book moves |
| `headers` | Original parsed PGN header dictionary, including build labels and TimeControl |
| `build`, `mover` | Existing mover build label; `"white"` or `"black"` |
| `setup_fen` | Normalized full FEN from PGN setup, otherwise the standard initial position |
| `moves_before_uci` | All PGN moves before this decision, including book moves |
| `before_fen`, `after_fen` | Full FENs from the existing extraction path |
| `played_move_uci` | Legal move from before_fen to after_fen |
| `phase` | Existing material-phase bucket before the move |
| `ply_since_book_exit` | Nonnegative integer or null, defined below |
| `book_exit_basis` | `explicit_book_prefix`, `setup_assumed`, `unknown` |
| `annotation` | Current move's parsed comment text, braces excluded |
| `engine_score_cp` | Current numeric mover score used in selection |
| `engine_depth`, `engine_time_s` | Current annotation's parsed depth and time |
| `next_annotation`, `next_engine_score_cp` | Comment and numeric mover score at ply_index+2 |
| `self_swing_cp` | engine_score_cp - next_engine_score_cp |
| `oracle_before`, `oracle_after` | Endpoint objects from D3 |
| `legacy_loss_cp`, `raw_loss_cp` | D3's derived values |

Within an artifact, row_id is unique. Across artifacts, input SHA256 + game_index + ply_index
identifies the source observation; do not deduplicate by FEN. The same position may occur in
different games, builds, clock states and histories.

Replay setup_fen, then moves_before_uci; the result must equal before_fen under the same
python-chess FEN serialization. Pushing played_move_uci must produce after_fen.
The prefix covers only the PGN: for a setup position, the preceding real game's history is unknown.

For book exit: when book annotations form a nonempty leading prefix, the first subsequent ply is
0 and basis is explicit_book_prefix. If no book annotations exist and SetUp=1 has a FEN, use
ply_index with basis setup_assumed, matching the corpus convention rather than asserting it.
Otherwise use null/unknown. A book annotation after non-book play makes book exit ambiguous for
the whole game: null/unknown.

Export the already-parsed engine_time_s and original TimeControl header, but do not reconstruct
remaining clocks in A. B/C may locate the original game by input SHA256 + game_index + ply_index
and replay its annotation times. That operation requires the original PGN; this export's UCI move
prefix carries positions, not the preceding clock annotations. Preserve the source PGNs for later
clock analysis. Missing PGNs make that optional analysis unavailable, not the exported position invalid.

**Alternative:** FEN-only export is smaller but loses PGN-known repetition history. Keep the move
prefix for self-contained position replay; defer clock reconstruction because it has no consumer in A.

### D5: Provenance and completeness are part of the format

Manifest fields:

| Field | Contents |
|---|---|
| `type`, `schema_version` | `"manifest"`, 1 |
| `source_run` | Optional --source-run string, otherwise null; never inferred from a directory name |
| `source_root` | Resolved input root as diagnostic text |
| `inputs` | Ordered objects: relative_path (POSIX separators), SHA256; root-file input uses its filename |
| `producer` | Python/python-chess versions; SHA256 of both analyzers and the new export helper |
| `oracle` | Binary basename and SHA256; requested depth; configured Threads=1 and Hash=64 MiB |
| `scan` | Actual jobs, batch size, shard cap and games cap |
| `selection` | `tier2_contested_v1`, contested_abs_cp=150, min_legacy_loss_cp=150, clamp_cp=1000 |
| `oracle_policy` | `bare_fen_per_game_hash_v1` |

Use the exact existing sorted file selection before assigning input_index. Include only selected
files in inputs. The game cap keeps its current meaning: eligible games per shard, not source-game
ordinal. Input files must remain immutable during the scan; concurrent source edits are unsupported.
Input and executable hashes provide identity; no dependency on a Git checkout or a GitHub request.

The completion record has `type: "complete"`, schema_version=1, `eligible_rows`,
`exported_rows`, `finite_clipping_misses`, `scored_games`, and `cells`, a sorted list of:
`{build, phase, eligible_rows, exported_rows, finite_clipping_misses}`.
All counts are nonnegative integers and present even when zero. finite_clipping_misses is D2's
predicate counted across **all** eligible rows, before the export filter. Mate endpoints do not
qualify because raw_loss_cp is null; a finite row with raw loss exactly 150 does qualify.

The footer must reconcile to the existing counters:
eligible_rows = sum(c[0]); exported_rows = sum(c[3]); per-cell exported_rows is that cell's c[3].
All emitted records must reconcile by build/phase. Global finite_clipping_misses must equal the
sum of its per-cell counts and never exceed eligible_rows - exported_rows. Its value is validated
against all-row fixture inputs, because non-exported misses cannot be recomputed from the artifact.
There is no mean raw loss over mate records and no new blunder-rate calculation here.

Exclude volatile timestamps and elapsed performance readings from this data format.
For fixed input, producer and oracle bytes/options, rows and counts must be deterministic across
worker scheduling. jobs/batch settings are intentionally recorded and may differ in the manifest.
Normal progress timing remains in stderr.

### D6: Keep storage and search work bounded

extract() runs in the **parent**. Build one immutable context per game (headers, setup, move list)
plus per-ply metadata and eligible row indices. Submit that game context once with its game's task
(or once within its batch), so it crosses the process boundary once per game. Do not attach a copied
prefix/context to every eligible row. Materialize moves_before_uci only for selected rows in the worker.

Keep existing game/batch task boundaries and ordered parent consumption. Workers return export
records alongside their current counters and display candidates; the parent writes and releases
each result. Do not retain an all-corpus export list or change engine process reuse.

This adds records to the existing per-shard work footprint, not a new guarantee of constant-memory
extraction. Large --batch values still increase retained worker-result data. Parallel scheduling
redesign, persistent engines and background writer queues are outside this change.

Raw endpoint retention must not add analyze calls, move a cache lookup, clear hash, reorder rows,
request MultiPV or change search limits. Cache the full before-endpoint result and reuse it wherever
the existing score cache is reused.

Output size is an arithmetic planning estimate, not a reason to require a measurement first:
1.5M rows x 6.3% = about 94,500 selected rows; at an assumed 1-1.5 KB per row, approximately
95-142 MB (rounded to 100-150 MB). The 6.3% is the recorded opening rate, not a measured pooled rate;
headers and longer prefixes can increase row size. Even a several-fold increase is acceptable for
an offline file, so v1 needs no compression or normalized sidecar. This is not a hard size bound.

### D7: Later-stage contracts, deliberately not executable plans

| Stage | Consumes | Produces | Contract |
|---|---|---|---|
| B1: sampling | Complete v1 export | Frozen selected row IDs, source identity, stratum population/sample counts, seed and inclusion probabilities | Sampling is reproducible; no top-N shortcut or silent failure exclusion |
| B2: confirmation | B1 sample plus named oracle/protocol | Per-row controlled shallow and deeper endpoint observations and outcomes | History/reset settings match across depths; legacy-to-controlled drift is separate |
| B3: StratChess replay | Confirmed sample, exact build identity, explicit budgets | Per-row budget/iteration traces and oracle assessment of chosen moves | Preserve full iteration PVs; accept alternative good moves; timeouts/provenance failures remain visible |
| C: reporting | Sample design plus all B outcomes | Weighted confirmation/improvement summaries and reproducible mechanism evidence | Unknown cases keep their weight; extra-search improvement does not prove an eval/search cause |

B1 owns band definitions and weighting. B/C own optional clock reconstruction from preserved PGNs.
B2 must settle final-score acceptance, mate outcome rules, draw-history treatment, budget limits and
timeout semantics before implementation. B3 must settle historical-build availability and reference
budgets. C must settle the clustered uncertainty calculation for the actual sampling design.
These choices do not block A because A retains the source observations rather than resolving them.
#484 as a whole remains not-ready; approval of A only makes the export package ready for handoff.

## Agent-sized ownership

These are implementation boundaries for review, not permission to start implementation.
A0 establishes automated Python coverage and accurate classification before the export packages.
Run them sequentially; several modify the same analyzer. Each handoff includes this design, its
named functions/files, independent fixture expectations and a stop rule: report a contract conflict
instead of changing the schema or scientific method.

| Package | Owner's files / responsibility | Review gate |
|---|---|---|
| A0: automated coverage/classification | .github/workflows/nightly.yml; Scripts/Get-ChangeTier.ps1 and its self-test assertions | Python unittest discovery runs nightly; explicit analyzer/export/test paths classify as Tooling; workflow/classifier remain Build |
| A1: score/format contract | New Scripts/external_quality_export.py; its tests in Scripts/test_external_quality_export.py | Typed scores, loss arithmetic, stream order/exclusive creation and footer checks work without a live engine |
| A2: source metadata | Scripts/analyze_external_quality.py extract(); extraction tests in the same test module | Existing eligible source plies unchanged; row IDs, histories, annotations and book rules pass known PGN fixtures |
| A3: scoring integration | oracle_eval(), score_batch(), _score_rows() and cache consumers; corresponding tests | Existing request sequence/counters/display candidates unchanged; export gets every c[3] event with correct mover POV; all-row clipping-miss counts match fixtures |
| A4: CLI and completion | analyse()/main(), parent writer integration, CLI tests and Docs/MoveQuality.md usage link | Preflight failures do no searches; scoring interruptions lack completion; report failures retain completion; successful files reconcile; option-off compatibility holds |

A1's OracleResult corresponds exactly to the D3 endpoint table; RawScore is its finite/mate union.
Record constructors and mapping keys must use those field names. A1 also owns the serialized
manifest/blunder/complete records from D4/D5. A2/A3 reuse
them. The implementation plan must pin their exact constructors and helper signatures before any
worker is dispatched. Do not have separate agents independently invent the Python interfaces.

The controller retains schema/method decisions, integration review and live-oracle verification.
A lower-tier agent may implement a package with a closed edit list and fixed tests.
Schema revisions or experimental choices return to the controller. Do not split one package into
multiple agents that must coordinate edits to the same file.

## Invariants

1. Every emitted row belongs to the original Tier 2 eligible population and has legacy loss >=150.
2. Each original c[3] event is exported exactly once, regardless of self-swing or display truncation.
3. Same inputs/options produce the same oracle request sequence with export on and off.
4. Every score is interpreted from the original mover's viewpoint.
5. Mate scores never enter raw centipawn arithmetic.
6. Every row reconstructs its PGN-known before/after position; absent history is not fabricated.
7. A scoring/export failure before completion leaves an incomplete artifact; later report failure does not invalidate a completed scan.
8. No new Python dependency, C++ engine change, or strength measurement is needed for A.

## Validation

Use standard-library unittest, following Scripts/test_build_corpus.py; use the already required
python-chess dependency for board fixtures. Keep exporter fixtures runnable without a Stockfish
binary. Do not make unit tests download or launch one.

Exact fixture expectations:

| Case | Required result |
|---|---|
| Finite losses 149 and 150 | 149 excluded, 150 exported; no sampling bands stored |
| before=0, after=-1000 | legacy/raw loss=1000; neither endpoint clipped |
| before=0, after=-1500 | legacy=1000, raw=1500; after endpoint finite_clipped=true |
| before=1000, after=-1000 | legacy/raw=2000; neither endpoint clipped |
| before=1200, after=1000; before=1150, after=1000 | Neither exported; each adds one finite_clipping_miss to its cell and total |
| Finite raw loss 149, or either endpoint mate | Adds no finite_clipping_miss |
| Finite-to-mate and both mate directions, including mate=0 | Winner/perspective correct; raw_loss=null; terminal legacy scores exactly -1000/+1000/0 |
| Seven selected rows in one game, >20 across games, including self-swing>=150 | Every selected row exported; existing display still truncates |
| Same FEN at different source ordinals | Distinct IDs; no deduplication |
| Leading book, FEN setup, standard start, late book annotation | Exact D4 book basis and 0-based/null index |
| Annotation with known score/depth/time | Parsed engine_time_s and original TimeControl retained; no reconstructed clock fields |
| Corrupt game followed by valid game | Corrupt contributes nothing; valid source ordinal is not renumbered |
| Missing current/next score and final two plies | Existing eligibility unchanged |
| Stub oracle records every invocation | Cache hits, order, resets and limits equal with option on/off |
| Worker/export exception before completion | Nonzero failure; partial output has no complete footer |
| Report/bootstrap or aggregate-output exception after scoring | Nonzero exit; completed export still validates |
| Existing output path, including a previous partial | Fail before searches; existing file unchanged; retry requires new path or operator removal/rename |
| Zero selected blunders, with or without eligible rows | Valid manifest + complete with exported_rows=0; preserve no-eligible CLI error |
| Jobs/batch variants under deterministic fixture oracle | Identical ordered row payloads and completion counts |

Run during implementation, from repository root:
`python -m unittest discover -s Scripts -p test_external_quality_export.py -v`.
Also run both analyzers' existing self-tests and Tier 1 --self-check on an explicitly selected
small PGN fixture/corpus. Live oracle perspective checks are separate from the no-engine unit suite.

A controller-run bounded live smoke must compare export on/off report/counters for the same
small input and verify worker shutdown; control the same Threads, Hash, depth and game ordering.
No full-corpus scan is part of accepting the code.

**Chosen automation: explicit nightly unittest discovery**, not only --self-test. A0 adds a dedicated
Python-fixtures step beside the existing Every script self-test step in nightly.yml's Windows job.
Configure Python 3.12 and install the existing dependency with explicit pins
`python-chess==1.999` and `chess==1.11.2` (the locally verified versions). Run standard-library unittest
discovery over `Scripts/test_*.py`, including existing test_build_corpus.py and the new exporter tests.
Fail if no tests are discovered or any test fails; missing dependencies also fail the step. Do not
require Stockfish. Keep the PowerShell self-test step and its exit behavior intact.

A0 must demonstrate discovery/failure propagation with a temporary failing fixture and a passing run;
remove that temporary fixture before committing. An empty-discovery probe must also fail. This gives
the test module an automatic post-merge execution path. Each A package additionally runs its focused
unittest command locally; nightly is not a substitute for pre-PR verification.

**Classification:** the existing analyzers currently fall through to Engine, but they are engine-inert
PGN readers. A0 is a separate Build-tier PR that explicitly lists analyze_move_quality.py,
analyze_external_quality.py, external_quality_export.py and test_external_quality_export.py as Tooling,
with literal path assertions in Get-ChangeTier.ps1's self-test. Keep unlisted paths fail-closed and
workflow/classifier files Build-tier. Allowlisting these inert paths is accurate classification.
Land A0 first, run its full Build validation once, and then apply the ordinary Tooling gates plus the
explicit Python fixture command to A1-A4. A0's PowerShell edit uses the write-powershell skill.
This draft alone is Docs tier. No Elo match is needed: exporter work changes neither engine play nor
the oracle search sequence.

## Assumptions I cannot verify from the code

- The target depth-12 corpus and exact oracle binary remain available. Not checked in this design;
  verify files and hashes before an export run. Implementation tests use fixtures.
- The existing python-chess/Stockfish reset protocol is deterministic under the documented settings.
  Prior repository measurements support it; this design has not rerun them. Check in the bounded
  live smoke, without changing the legacy protocol.
- FEN setup loses earlier repetition history; replay covers only the PGN-known prefix.
  Later clock analysis needs the original PGNs and cannot reconstruct original scheduler traces.
  These are stated data limits, not claims that A can recover the missing information.

## Harvest

| Durable decision | Destination |
|---|---|
| JSONL schema, population definition, score/history rules and clipping-miss counter | Docs/MoveQualityExport.md, written with implementation |
| Why export cannot change the oracle sequence | Source comment at scoring/export integration |
| Run commands and interrupted-output behavior | Docs/MoveQuality.md link to export-format documentation |
| Exact edge-case contracts and automatic execution | Scripts/test_external_quality_export.py; nightly.yml Python-fixtures step |
| Execution handoffs and transient review notes | Working notes; not permanent project documentation |
| Future B/C protocol choices | Their own reviewed designs before those implementations |

Keep this document as a deliberate specification until those destinations exist. Changes to the
approved schema or eligibility during implementation require a recorded design amendment.

## Cross-agent review disposition

Removed the merged-info bound field and its flip fixture; local python-chess reproduced a depth-12
score carrying a stale depth-3 lowerbound flag. Completion now belongs to scoring/export and precedes
reporting. A0 explicitly wires unittest discovery into nightly and classifies engine-inert files.
Clock reconstruction and sampling strata are deferred to their consumers. Added finite clipping-miss
counts, direct terminal legacy scores, once-per-game parent-to-worker context transfer and retry
instructions. Size is an arithmetic planning scenario, not a measured pooled-rate bound.
