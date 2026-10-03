# Strength lab comparison and completion integrity — Design

**Issue:** [#705](https://github.com/theEscape2207/StratChess/issues/705)
**Baseline:** `origin/main`, `ee45484be904c1560f795d79b77195508df9a53d`.
**State:** self-reviewed; awaiting owner-routed design review before implementation.

## Goal

Refuse an accidental null experiment before match shards start, and refuse a
parseable but incomplete or wrongly routed batch before reporting Elo. Retain
what each arm intended to compare without claiming the engine acknowledged its
settings. This protects experiment validity and avoids wasted runs; it changes
neither engine strength nor search cost.

## Review focus

- D1–D2: whether the identity rule catches default-on/null comparisons without
  rejecting legitimate source, option or time-control comparisons.
- D3–D4: whether retained settings and independently checked shard identities,
  opening starts and pair counts justify the report's claims.
- D5 and Assumptions: coverage through local fixtures, compatibility with older
  option tables, and preserving the active #702 experiment.

## Scope

**This change will:** compare each resolved candidate arm with the reference;
require a declaration for intentional identical calibration; retain a readable
comparison in existing run summaries and an artifact; assert shard identity,
arm routing, opening starts and planned pairs; and document ledger links.

**This change will not:** change engine heuristics, statistical formulae, shard
scheduling, opening allocation or measurement budgets; build a comprehensive
manifest/database; rewrite historical results; or dispatch a lab merely to
validate these safeguards. #506/#602's broader evidence storage stays separate.

## Decisions

### D1: Resolve both engines once, including empty overrides

Keep `validate_uci_options.py` as the owner of advertisement parsing and option
validation. Add resolution helpers reused by a small
`.github/scripts/compare_lab_configs.py` driver. Query each staged binary once
with `uci`, require successful exit, a complete `uciok` reply and a nonempty
table, and reject malformed or duplicate advertised option names. Process all
arms against that candidate table and resolve the reference independently.

The supported table remains spin/check, matching current and historical
StratChess options. An unsupported advertised type fails with a diagnostic
rather than disappearing from the comparison. An older reference may omit a
new candidate option: empty overrides remain valid, while an explicit override
of the missing option fails under the existing validation rule. Option-table
differences are reported, never filled with the other engine's defaults.

Resolve advertised defaults plus validated overrides, and force harness-owned
`Threads=1` on each side. Spin values normalize to integers (`01` equals `1`);
check values normalize to booleans. Option order and redundant default overrides
do not create a difference. Existing typo/range/unsigned-decimal/duplicate/
reserved-option checks remain. Correct the validator's current overstatement
that success proves every option takes effect.

Reject comparing raw override strings: an empty reference and an explicit
default-on candidate can describe the same settings.

### D2: Refuse demonstrably identical comparisons; declare calibration explicitly

Add workflow input `calibration`, a boolean defaulting to false. It explicitly
declares a harness calibration (null or known-sign control), is recorded in the
comparison and report, and permits an identical arm. It does not bypass option,
query, routing or completion validation. A mixed run with an identical arm must
declare calibration for the run; the report still classifies every arm's actual
differences separately so differing arms are not described as null.

For each arm, refuse when all three hold and calibration was not declared:

1. Same source/build identity, **or** byte-identical staged engine binaries.
2. Equal normalized resolved option maps, including harness-owned Threads.
3. Equal normalized time controls.

Source/build identity means equal tracked Git entries (path, mode and object)
under `CMakeLists.txt`, `cmake/`, `StratEngine/` and `StratChessEvolved/`, with the
same requested CMake definitions and fixed workflow toolchain/Release recipe.
These are the current CMake target/build inputs; compare the actual entries at
both resolved revisions, not commit SHA alone. Record full revision SHAs and
SHA-256 binary hashes too. Equal inputs suffice even if build locations produce
different binary bytes; equal binary bytes suffice even if revisions differ.
This catches a docs-only revision difference without inventing semantic
equivalence for arbitrary code changes. A failure to obtain identity evidence
fails preflight. Different inputs/binaries are a permitted code comparison,
not proof of different chess behaviour.

Accept time controls in the documented lab form `seconds+increment`, with
finite decimal base > 0 and increment >= 0, normalized using Decimal. Thus
`10+0.10` equals `10.0+0.1`; `5+0.1` versus `10+0.1` is a legitimate difference.
Reject unsupported forms rather than treating spelling as evidence of a
different condition. Leave the existing low-increment warning intact.

Remove the setup warning equating a matching commit with a null experiment:
the complete comparison now owns that decision. Reject the simpler same-SHA
rule because #702 deliberately compares options on one binary.

### D3: Retain a readable intended comparison, with its verification limit

The comparison driver writes Markdown with revisions, input identity result,
binary hashes, shared build definitions/toolchain, time controls, calibration
declaration, the reference's resolved defaults/overrides and every arm's resolved
settings and differences. Write it before returning an identical-comparison
failure so the refusal is explainable. Distinguish query/validation failures
from a complete resolved comparison; never fabricate missing settings.

Append that file to the build job summary and upload it as a small separate
comparison artifact, retained 90 days like shard evidence. Download and append
it to the aggregate report; both the run summary and existing PR comment then
carry the same comparison. Missing evidence must prevent a successful result
report. A failed preflight still has its build summary/artifact and launches no
shards; an aggregate failure reports DISCARDED without a partial Elo figure.

State next to the settings: advertisement/domain/syntax validation establishes
**intended configuration**, not runtime readback or proof `setoption` was applied.
`readyok` would not acknowledge individual settings and is not added as a false
guarantee. Cross-field engine validation remains a stated limitation.

`Measurements/README.md` will require each new row detail to link the retained
comparison in its run summary/artifact, alongside the existing pre-dispatch
role, stopping rule, budget and all-arms convention. No historical row is
invented. The next eligible owner-approved experiment exercises that convention;
it is not a prerequisite for this implementation PR.

### D4: Validate the whole shard set before pooling any arm

Extend `plan_arms.py` with a batch-verification command used for single-arm and
multi-arm runs. First require exactly one artifact directory per shard index
0..S-1, each containing its log and PGN; reject duplicates, missing, unexpected
or noncanonical indices. Use the existing `i mod K` routing, including the
single-arm case, rather than a second allocation formula.

Verify every PGN game's White/Black names against the expected staged candidate name
(with `-armX` only for that shard's arm) and reference name. Both colour orders
are valid. Missing names or another arm's names fail. Locate the games tagged
`Round "1"` and verify both FENs' four position fields against the pinned EPD
book entry assigned by
`opening_offset + shard * rounds_per_shard` (zero-based). Download the existing
toolkit's book for this check. Require the two round-one games with opposite
colours. Keep the existing distinct-start-FEN check as an additional invariant,
using round one rather than file order; move its ownership into the verifier
to avoid parallel shell/Python checks. Concurrency writes completed games out
of round order, so the first physical PGN game need not use the assigned start.
This verifies the assigned start and existing disjointness evidence, not every
opening played or the PGN's move legality.

Add `pool_pentanomial.py --expect-pairs-per-shard N` and check
`sum(last_pentanomial_counts) == N` for **each** log before computing or printing
any pooled figure. Reject both under-counts and over-counts, even if their total
across shards matches the plan. CLI counts must be positive when supplied.
Keep the flag optional for historical standalone pooling; the workflow always
passes `setup.rounds_per_shard` in both single-arm and per-arm calls. Retain
`--expect-shards` and the existing pair-based statistics unchanged.

Run whole-batch routing and count checks before emitting any arm's result.
Write all arm pools to a temporary report and publish it only after every arm
succeeds. A failed shard or integrity assertion still discards the entire run.
Artifact/PGN-name checks establish internal consistency, not tamper-proof
provenance; no new security boundary is claimed.

### D5: Test these boundaries locally and through normal Build gates

Keep existing `--self-test` entry points. Add fixture checks to their owning
Python scripts and the new comparison driver; invoke all from workflow setup.
No PowerShell edits are planned. Use subprocess fixtures for failed/incomplete
UCI replies and full CLI paths, not only tests of comparison helpers.

The workflow's existing Build-tier classification and repository validation
remain authoritative. Do not re-dispatch, cancel, alter or duplicate #702's
active experiments. Existing runs use the workflow of their dispatched revision;
these changes apply only to future dispatches containing this implementation.

## Assumptions I cannot verify from the code

- **Pinned fastchess output:** verified against completed #702 run
  `37125713346`, artifact `strength-37125713346-shard-0`, using its toolkit book.
  The PGN has `candidate-a922cee-armA` / `reference-4dafbdd` White/Black names,
  1,480 games and rounds 1–740. Its first completed game is round 2, whose
  position matches book entry 13,322; both round-one games match entry 13,321.
  This changed D4 to use round tags, not physical order. Preserve these minimal
  headers as fixtures during implementation. Future runner formats are not
  assumed compatible: missing/ambiguous required headers fail validation.
- **Source/build inputs:** the enumerated paths cover the current CMake engine
  target and dependency pins (verified in `CMakeLists.txt`); future externally
  generated inputs would require extending this identity rule. Fixture tests
  distinguish docs-only, engine/header and CMake/dependency-pin changes. This
  rule is a conservative identity check, not a universal reproducibility proof.
- **Artifact retention:** workflow summaries/artifacts follow GitHub's existing
  services and retention limits. Verify upload/download wiring with local
  workflow fixtures and the normal PR checks; the next eligible experiment
  supplies integration evidence. No guarantee of permanent archival is made.

## Invariants

Every arm reaches the same validated reference; empty overrides still query
defaults; spelling cannot defeat equality; calibration bypasses only the null
refusal. No match starts after failed preflight. Every shard is uniquely routed,
starts at its assigned book position and contains exactly its planned pairs.
Only a fully validated batch publishes any Elo; complete inputs reproduce the
existing pentanomial result. Retained settings always state their verification
limit. Search, scheduling and measurement budgets remain unchanged.

## Validation

Baseline probes: the existing option validator's 18 checks, arm-router self-test
and seven pooling formula cases all pass on the baseline using bundled Python.
Source inspection confirms the empty-override early return and absent pair-count
assertion. These passes demonstrate existing coverage, not coverage of the gaps.

Fixtures must close the invariants above: default-on candidate versus empty
reference fails; reordered/redundant/zero-padded overrides cannot evade it;
real option differences, code/build-input differences and unequal time controls
pass; declared null and known-sign calibrations pass; old option tables and
failed queries retain validation semantics; and every multi-arm setting appears
in retained output with its limitation. Test equal source inputs with unequal
binary bytes, equal binaries at unequal revisions and docs-only revisions.

For completion/routing, test complete single/multi-arm batches, a truncated but
parseable log, an excess-count log, counts that offset each other, missing and
duplicate indices, swapped-arm PGNs, wrong assigned round-one FEN, missing or
duplicated round-one games, out-of-order completed PGNs, and absent
comparison evidence. Complete cases preserve known pooled Elo/error bars;
invalid cases publish no pooled figure. Exercise the actual CLI arguments used
by the workflow so omission of planned counts is observable.

Implementation is **Build tier** because `.github/` changes; run the new/existing
Python fixtures, `Validate-PreCommit.ps1`, `Validate-PrePR.ps1` and normal required
PR CI, plus Standards/Spec review. This design-only commit is Docs tier with
pre-commit validation. **No Elo match, nps benchmark or fresh CI lab is needed:**
engine code is unchanged and fixture failures can directly prove the safeguards.

## Cost

Initial estimate, not measured: **over 200 changed lines**, roughly 350–600,
across 7 files (four Python owners, strength workflow, CI guide and measurement
convention), excluding this temporary plan. Build-tier gates and two-axis code
review apply; actual time/token cost is unknown. Use the issue's requested
Sol-class maximum for reviewer dispatch where the review workflow permits it;
no design-review subagent is dispatched here because the user routes that round.
No optional subsystem or paid measurement is included. The book/name check adds
cold artifact/PGN work, with runtime magnitude unknown until fixture execution;
it adds no engine per-node work.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Defaults, normalization, supported table types and identity limits (D1–D2) | Python helper contracts/self-tests; `Docs/CI.md` strength-lab section |
| Calibration declaration and intended-setting verification limit (D2–D3) | Workflow input/help and comparison report; `Docs/CI.md` |
| Retained comparison links and experiment declaration (D3) | `Measurements/README.md` |
| Per-shard counts, assigned starts and routing (D4) | Python verifier/pool contracts and fixtures; `Docs/CI.md` |
| Validation evidence and any approved decision changes | PR body; durable policy changes in the destinations above |

Self-review changed D4's start check to use round-one tags after retained PGN
evidence disproved the first-physical-game assumption. No approved decisions
have changed: implementation has not started. Delete this
plan in the implementation PR only after review dispositions and Harvest are
complete and no inbound reference requires retaining it.
