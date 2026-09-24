# Search profile comparison script — Design

**Issue:** #637, the comparison script (PR 3). Epic #636.

## Goal

The `STRAT_SEARCH_PROFILE` build prints six counter lines per search, and nothing reads them. Today
each #636 child would re-derive #636's baseline table from raw lines with a throwaway script, as
PRs 2a and 2b did. #634 also showed that pooled numbers can hide opposite effects in endgames and
middlegames. A single script that runs two builds over one position set and prints every derived
measure side by side, split by group, makes each child's before/after a single command.

## Scope

**This change will:**

- Add `Scripts/Compare-SearchProfile.ps1`: two builds, the same positions and depth, `Threads=1`,
  and a fresh process per position. It prints before/after/delta for every #637 item: pooled, by
  endgame group, and per position.
- Move `Run-Bench.ps1`'s built-in positions and FEN-file reader into a shared library, so both
  scripts read one list (D2).
- Refuse a pair where either side is not a profile build (D4).
- Point `Docs/TestDesign.md`, `Docs/Workflow.md` and skill `measure-strength` at it.

**This change will not:**

- Change the engine or any counter's wording. The script parses the output contract that 2a/2b
  froze.
- Measure time or nps. That remains `Run-Bench.ps1`'s job, and a profile build is the wrong binary
  to time.
- Judge significance. A delta is one deterministic run at `Threads=1`, not a sample.
- Write CSV or JSON output. The console table is what goes into PR bodies; add an export when a
  consumer exists.

## Decisions

### D1: A new script, not a `Run-Bench.ps1` mode

Run-Bench answers "how fast is one binary", and it refuses on timing-contract grounds. This script
answers "how did the tree change between two binaries", and it refuses on the profile flag. The two
share only the engine driver (`UciDriver.ps1`, already a library) and the position set (D2).
Rejected: a `-Profile -Against` mode. It would double Run-Bench's parameter surface and its
self-test for a disjoint output.

### D2: One position list, in a shared library

The default set is Run-Bench's 8 positions, because #636's baseline and 2b's baseline were measured
on them. `$DefaultPositions` and `Resolve-Positions` move from `Run-Bench.ps1` to a new
`Scripts/BenchPositions.ps1`. Both scripts dot-source it, and it is registered in
`$SelfTestCoverers` under `Run-Bench.ps1`, whose position cases stay where they are. Rejected: a
second copy of the 8 FENs. A drift between the copies would silently compare a new tree against an
old table.

### D3: Derived measures and how they pool

Each counter is summed over positions first, then any ratio is taken. "Pooled" means a ratio of
sums, as #636 computed it. `maxdepth` pools by max. All values are per search at `Threads=1`.

| Item | Rows |
|---|---|
| totals | nodes; qs share of nodes |
| 1 iterations | EBF (below); then one row per iteration `d`: its nodes `n(d)` and its ratio `n(d)/n(d−1)` |
| 2 aspiration | aspirated iterations, fail-lows, fail-highs, full-window fallbacks; failnodes as % of nodes |
| 3 ordering | cuts; first-move rate; index 1 / 2 / 3-5 / 6+ as % of cuts; latecut hash / capture / killer / quiet as % of late cuts; hashnodes as % of cuts; hashcuts as % of hashnodes; latenodes and each latebands bin as % of nodes |
| 3 LMR | reduced; reducednodes as % of nodes; researched as % of reduced; researchnodes as % of nodes; confirmed as % of researched |
| 4 node types | PV / cut / all frames as % of frames; cutfaillow as % of cut frames, per band |
| 5 null move | tried; cutoffs and failed as % of tried; failnodes as % of nodes |
| 6 pruning | rfp per depth bin; floorbinds; frontier skips; lmp skips |
| 7 quiescence | roots; qs nodes per root; delta; see; maxdepth |
| 8 stability | best-move changes; settled iteration; mean score swing |

- **Iterations** come from the `info depth` lines: the first line per depth carries the cumulative
  node count, so `n(d) = N(d) − N(d−1)`. The final summary line repeats the last depth and is not
  an iteration.
- **EBF** is the geometric mean of `n(d)/n(d−1)` over the last four iterations, which is
  `(n(D)/n(D−4))^(1/4)`. Pooled, `n(d)` is summed over positions first. Rejected: the mean over
  all iterations. Depths 1-4 are dominated by the root move count, and their ratios swing the
  mean.
- **Best-move changes** count iterations whose first PV move differs from the previous one.
- **Settled iteration** is the first iteration from which the first PV move always equals
  `bestmove`. Pooled, it is the mean over positions. Rejected: the literal "first iteration where
  the bestmove appeared" from #637. A move found at depth 3, dropped, and found again at depth 14
  would read 3.
- **Score swing** is the mean `|score(d) − score(d−1)|` in centipawns over consecutive iterations.
  A pair involving a mate score is excluded.
- **Delta**, by the kind of row:
  - Counts, node totals and `n(d)`: relative %.
  - Rates (every "as % of" row): percentage points (`pp`).
  - EBF, `n(d)/n(d−1)`, qs nodes per root, `maxdepth`, settled iteration and score swing: absolute
    difference, in the row's own unit.
  - A zero base for a relative delta prints `n/a`.
- **Silence:** an absent line reads as zeros only where the engine's print rule allows silence on a
  completed search. That applies to `aspiration`, `lmr`, `nullmove`, `pruning`, `frontier skips` and
  `lmp skips`, which each print when their first field is non-zero (`pruning` on either field). D4
  lists the lines that must be present.

### D4: What is refused

- **A transcript that breaks the profile schema**, checked per side and per position. The error
  names the side, the position and the line.
  - **Required lines:** `treenodes`, `ordering`, `nodetypes` and `qsearch`. A completed profile
    search at depth ≥ 5 always has cuts, a PV frame and a quiescence hand-off, so each always
    prints. A missing required line means the flag is off, or a build older than the six-line
    contract; a PR 2a build lacks `nodetypes` and `qsearch`. #637 requires the refusal rather than
    silent zeros.
  - **Malformed lines:** any present line, required or optional, must match its exact field list,
    histogram lengths included. A reworded or truncated line is refused, not read as zero.
- **An unfinished search.** The side does not reach `-Depth` or print `bestmove`. This uses
  `Invoke-UciSearchToBestMove`'s and Run-Bench's existing strictness.
- **Not refused: the same binary on both sides.** That pair is the zero-delta check #637's
  acceptance asks for.
- **Not refused: differing node counts.** That difference is what the script measures.
  Equivalence belongs to `Compare-SearchEquivalence.ps1`.

`Threads` is not a parameter; it is fixed at 1, as in `Compare-SearchEquivalence.ps1`.

### D5: Groups and output layout

- A position is an endgame when each side's non-pawn material is at most 13, counting N and B as 3,
  R as 5 and Q as 9. It is read from the FEN, so a custom positions file is grouped the same way.
  On the built-in set, this selects exactly `rook-endgm` and `piece-endgm`, #634's split. Rejected:
  grouping by position name. It works only for the built-in set.
- The output prints one before/after/delta table per scope, in this order: Pooled, Endgame,
  Non-endgame, then each position. A group with no positions is omitted.
- The header states both exe hashes, the depth, the set name and its hash, as Run-Bench does.

`-Depth` defaults to 16, the depth of both baselines, and its minimum is 5, below which EBF has
no four ratios and the D4 detection has no cuts to see.

## Assumptions I cannot verify from the code

- **The engine prints one `info depth` line per completed iteration, then one summary line.** This
  was checked on a profile build (kiwipete, depth 8). A behaviour change there would make `n(d)`
  wrong. It is covered by a self-test case built from that transcript, and by the equivalence
  gate's per-iteration comparison, which would flag any change to the lines.
- **The current `origin/main` still reproduces #636's table.** Every engine commit since `af2f996`
  is telemetry that was proved node-identical, and 2a reproduced the table. Verified in Validation.

## Invariants

- A profile build compared with itself prints a zero delta on every row.
- The pooled rows of that run reproduce #636's baseline and 2b's first baseline to their printed
  decimals.
- Either order of a default/profile pair is refused, with the non-profile side named. So is a
  transcript missing a required line or carrying a malformed one.
- `Run-Bench.ps1` output is unchanged by the library move.

## Validation

Tooling tier.

- **`-SelfTest`**, on synthetic transcripts: every line parses; an absent optional line reads as
  zeros; a missing required line (each of the four), a PR 2a-shaped transcript and a malformed line
  are each refused (falsified); `maxdepth` pools by max; per-iteration rows, `iterations` and
  `failed` are read; EBF, settled iteration and
  swing are computed on a hand-worked case; the summary line is not counted as an iteration; the
  material classifier is checked on all 8 built-in FENs plus a queen endgame; the delta formats
  cover counts, rates and a zero base.
- **Reproduction:** run the current profile build against itself at depth 16 on the built-in set.
  Pooled first-move 93.1%, index 4.0/0.9/1.0/1.1%, late cuts 29.2/35.6/35.2%, hash 18.7% / 96.3%,
  latenodes 33.0% (8.1/12.7/12.2), reducednodes 19.9%, re-search 0.2% / 11.2% / 60.1%. Frames PV
  0.1% / cut 90.9% / all 9.1%, nullmove cutoffs 35.7%, qs per root 0.94. Every delta is zero.
- **Refusal:** profile-vs-default and default-vs-profile both exit non-zero, naming the side.
- **Run-Bench:** `-SelfTest` passes, and one depth-8 run's table matches the pre-move output.
- `Validate-PrePR.ps1` passes.

No Elo run and no nps run: the engine binary does not change.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1-D5: what is compared, pooling, EBF/settled/swing definitions, refusals, grouping | `Compare-SearchProfile.ps1` help |
| D2 shared positions | `BenchPositions.ps1` header comment; `$SelfTestCoverers` comment |
| When to use it | skill `measure-strength`; `Docs/Workflow.md`; `Docs/TestDesign.md` |
| Reproduction result | PR body; `Docs/Changelog.md` |
