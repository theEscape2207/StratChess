# Search profile counters — Design

**Issue:** #637, items 3-7. It is PR 2 of #637 and ships as PRs 2a and 2b. Epic #636.

## Goal

Search changes are gated on wall clock and Elo, and nothing explains a result. #634's tree moved by
−33% to +55% per position and the cause stayed a guess. #636's children are judged by how much work
goes before the cutting move and into LMR re-searches. Today those numbers exist only in a throwaway
probe (`worktree-extended-futility-probe-634`, `2df7c67`, `0068128`). Null move, reverse futility,
node types and quiescence pruning have no counters at all.

This change lands per-node counters for #637 items 3-7 as permanent telemetry. They are compiled out
of the shipping build, and node-identical when compiled in.

## Scope

**This change will:**

- Add the compile flag `STRAT_SEARCH_PROFILE`, default 0, which follows the `STRAT_TT_STATS`
  contract (D2).
- Add the five items 3-7 as six `info string` payloads with fixed wording (D3). They ship in two
  PRs (D1).
- Teach `Compare-SearchEquivalence.ps1` to compare a profile build against a default build (D5).
- Document the flag and its lines (D7).

**This change will not:**

- Add the comparison script. That is PR 3, and it relies on the contract in D3.
- Split hash-move presence by depth band (#636 child C). That lands when C starts.
- Change any search decision, or any always-on counter. The `frontier skips`, `lmp skips` and
  `aspiration` lines stay as they are.
- Time components (eval, movegen, TT) inside the engine. #637 leaves that to a sampling profiler.

## Users

| Item | Line | Consumer |
|---|---|---|
| 3 Ordering and LMR | `ordering`, `lmr` | #636 A1-A4, B1-B2: the metric each child is judged by |
| 4 Tree shape | `nodetypes` | #636 C (missing hash move); B1/B2, where LMR moves the work |
| 5 Null move | `nullmove` | re-screening #545 and #634; the largest pruner, unmeasured today |
| 6 Pruning by depth | `pruning` | #636 B3 (the improving flag feeds these margins); the #545/#634 re-screens |
| 7 Quiescence | `qsearch` | #636 A4 (capture ordering, SEE); any evaluation change |

## Decisions

### D1: One design, two PRs

- **PR 2a** adds the flag, D5 and item 3. #636 A1 needs item 3 first. It is also the one item that
  can be checked against measured numbers: #636's baseline table (Validation).
- **PR 2b** adds items 4-7 on the flag PR 2a lands.

Settling all six formats here freezes the contract PR 3 parses once. The split keeps each diff small
enough to review.

Rejected:
- One PR for items 3-7: about four times the write sites in one review.
- Items 4-7 on first user: each already has a named consumer (Users).

### D2: The flag, and where the counters live

`STRAT_SEARCH_PROFILE` is a CMake cache string, 0 or 1. It maps to
`inline constexpr bool kSearchProfileCompiled`, declared in `SearchTelemetry.h` beside
`kSingularExtensionsCompiled`. The test target always defines it as 1, as it does
`STRAT_TT_STATS`.

Each payload is a struct with `static constexpr bool compiled = kSearchProfileCompiled`, added last
in `SearchTelemetry` in D3 order. They reuse the existing `reset`/`add`/`append_info` path, so
neither Lazy SMP summing nor UCI output needs new plumbing.

In the shipping build the members still exist, as cold bytes at the tail of `ThreadData`. Every
write site is under `if constexpr`, and a discarded branch in non-template code is still
type-checked, which needs the members to exist. `TTStats` already makes this trade.

Rejected: an empty stand-in type with `[[no_unique_address]]` when the flag is off. The discarded
write sites would then fail to type-check.

The layout risk is closed by measurement, not argument. #555/#556 measured a −3.90% median nps
swing (9 of 9 pairs slower) from a 144-byte change in cold code, on a node-identical search. See
Validation.

### D3: Output contract (PR 3 parses this)

The lines follow `aspiration`, in this order. Each prints only when its first field is non-zero.
Slash lists are fixed-length histograms, one token each. Their bin edges are named in the header
comment and never change without changing the key. Depth bands are `D1to2/D3to6/D7plus` everywhere,
the bands #636 reports.

```
info string ordering cuts N index I0/I1/I2/I3to5/I6plus latecut H/C/K/Q hashnodes N hashcuts N latenodes N latebands B/B/B
info string lmr reduced N reducednodes N researched N confirmed N researchnodes N
info string nodetypes pv B/B/B cut B/B/B all B/B/B cutfaillow B/B/B
info string nullmove tried N cutoffs N failed N failnodes N
info string pruning rfp D1/D2/D3/D4/D5/D6plus floorbinds N
info string qsearch roots N delta N see N maxdepth N
```

PR 2a lands the first two lines, PR 2b the other four. "Nodes" in any `*nodes` field means main plus
quiescence, the same unit as the `info depth` total.

**`ordering`:**
- `cuts`: fail-high nodes in `pvs()` at `ply > 0`.
- `index`: the legal index of the cutting move.
- `latecut`: cuts at index > 0, classified by the cutting move. In order: the hash move, else a
  capture or promotion, else a killer, else a quiet.
- `hashnodes`: cut nodes that had a hash move. `hashcuts`: those where the hash move cut.
- `latenodes`: nodes spent on the moves searched before the cutting move, at nodes that cut at
  index > 0. Nesting-exclusive (D4). `latebands` splits it by the cut node's depth.
- These are exactly the fields #636's baseline table uses. The probe's `first`, `bandcuts`,
  `bandfirst`, `idxsum` and `faillow` are dropped.

**`lmr`:**
- `reduced`: LMR-reduced searches.
- `researched`: reduced searches that beat alpha and were searched again at full depth.
- `confirmed`: re-searches that still beat alpha.
- `reducednodes` and `researchnodes`: nodes inside each kind, outermost only (D4).

**`nodetypes`:** `pvs()` frames that reach the transposition-table probe, by depth band and by
expected type (D6).
- `cutfaillow` counts expected-cut frames that searched moves and failed low: the classification
  gives UPPER, or the LMP fail-low return fires.
- Frames that return before the probe (abort, draw, `depth <= 0`) are not counted.

**`nullmove`:**
- `tried`: calls where `should_try_null_move()` returned true.
- `cutoffs`: attempts with `null_score >= beta`.
- `failed`: attempts that completed below beta.
- An aborted attempt is in `tried` only.
- `failnodes`: nodes inside failed null searches, nesting-exclusive (D4).

**`pruning`:**
- `rfp`: reverse-futility cutoffs, by the node's depth. The depth limit is tunable, so the last bin is
  open.
- `floorbinds`: frontier fail-low floors that raised `best_value` at a node where a searched child
  returned (#529's counter).
- Frontier and LMP skips need no per-depth split. Each pruner fires at exactly one depth (1 and 2),
  so the existing totals already are per depth.

**`qsearch`:**
- `roots`: `pvs()` calls handed to `quiescence()` at `depth <= 0`. qs nodes per main leaf is
  `treenodes qs` / `roots`.
- `delta` and `see`: moves each pruner skipped.
- `maxdepth`: the largest `QSEARCH_BUDGET − qsearch_budget` reached. In-check chains can exceed the
  budget.
- `maxdepth` is summed across threads by **max**, not by addition. It is the one field that does not
  add.

### D4: Nesting-exclusive accounting

- **`latenodes`, as the probe does it.** At loop start, snapshot the node total and the running
  `latenodes`. Before each searched move's edge is counted, snapshot both again. On a cut at
  index > 0, add (move snapshot − loop snapshot) − (nested `latenodes` added in that span). A
  late-cut node nested inside another's earlier moves is counted once.
- **`failnodes`.** The same subtraction across the null-move call: the nodes it spent, minus
  `failnodes` added by failed null searches nested inside it.
- **`reducednodes` and `researchnodes`, as the probe does it.** Each kind keeps its own per-thread
  nesting depth, and nodes are added only when the outermost search of that kind returns. The
  decrement comes straight after the recursive call, before any abort return, so an abort cannot
  leave a depth unbalanced.
- **The two LMR totals are exclusive only within their own kind.** A reduced search inside a
  re-search counts in both, so the totals must not be summed. The header comment says so.

### D5: The equivalence gate compares profile lines only when both builds print them

`Compare-SearchEquivalence.ps1` already treats `treenodes` this way. The six D3 lines join that rule.
A profile build compared with a default build of the same commit then reads IDENTICAL. It lands in
PR 2a, and 2b only extends the list.

That comparison proves node identity only: it passes even if a profile line is missing or
malformed. The wording is pinned separately by an exact payload test per struct, which also checks
that the line is silent when its first field is zero, as `SearchTelemetryTests.cpp` already does for
the existing lines.

**For PR 3:** the comparison script's zero-delta self-check runs a profile build against itself. A
default-versus-profile pair is what the script must refuse, and it serves only the node-identity
gate here. #637's acceptance wording is updated to say this.

Rejected: stripping the lines by hand outside the script, as #638 did. #637's acceptance names the
script as the gate, and a check done by hand is not repeatable.

### D6: Expected node type is tracked per ply, not passed as a parameter

`pvs()` knows `is_pv_node`, but it does not know whether a non-PV node is expected to cut or to
fail low.

The profile struct holds `expected[MAX_PLY]`. The parent writes `expected[ply + 1]` before each
recursive `pvs()` call, and the root iteration writes `expected[0] = PV`. The rules are Knuth-Moore:

| Call | Child's expected type |
|---|---|
| PV node, first move, and every PV re-search | PV |
| PV node, later null-window move (reduced and re-search alike) | CUT |
| CUT node, first move | ALL |
| CUT node, later moves | CUT |
| ALL node, every move | CUT |
| Null-move child | ALL |
| Singular verification frame | ALL (below) |

A verification search re-enters `pvs()` at its parent's ply, not `ply + 1`, so a write before the
call would land on the parent's own slot. A scoped guard, the same shape as `ExcludedMoveGuard`, saves
`expected[ply]`, sets it to ALL for the verification, and restores it on scope exit. That covers the
abort return too. The guard compiles to nothing when the flag is off.

Rejected: a `cut_node` parameter on `pvs()`. It changes the shipping signature and codegen for a
measurement-only feature, and the node-identity gate would not catch that cost.

### D7: Documentation, by PR

- **2a:**
  - `Docs/Engine-Readme.md` documents the flag and the `ordering` and `lmr` lines, next to `ttstats`.
  - `Docs/TestDesign.md` points to the invariant and payload tests.
  - `Docs/Workflow.md` names the flag beside `STRAT_TT_STATS`.
- **2b:** `Docs/Engine-Readme.md` documents the other four lines.
- **PR 3:** `Docs/Workflow.md` and the `measure-strength` skill point to the comparison script and
  say when to use it. The script does not exist before then.

## Assumptions I cannot verify from the code

- **The shipping build compiles the snapshot locals away.** Their only readers are discarded
  `if constexpr` branches. To be verified by each PR's nps gate: a snapshot the optimiser keeps shows
  there first.
- **The ported item-3 counters reproduce the probe.** The search logic in `pvs()` is unchanged since
  the probe's base `af2f996`. Since then only telemetry commits have touched `AIPerplex.cpp`,
  `Sort.cpp` or `ThreadData.h` (checked with `git log af2f996..origin/main`). So a profile build must
  reproduce #636's baseline percentages. This is verified in PR 2a.
- **Items 4-7 have no reference measurement.** Their correctness rests on the invariant tests and on
  review of the write sites. PR 2b's body reports a first depth-16 table as the new baseline, and
  checks it for plausibility against `frontier skips`, `lmp skips` and `treenodes`, which already
  exist.

## Invariants

All of these hold per search, at `Threads=1`.

**Both builds:**
- The default build is node-identical to the merge base and prints no new line.
- A profile build is node-identical to the default build.

**`ordering` and `lmr`:**
- The `index` bins sum to `cuts`.
- The `latecut` categories sum to `cuts − I0`.
- `hashcuts ≤ hashnodes ≤ cuts`.
- `latebands` sums to `latenodes`.
- `confirmed ≤ researched ≤ reduced`.

**`nodetypes`:**
- `cutfaillow[b] ≤ cut[b]` in every band.
- Every band of `pv` is greater than zero.

**`nullmove` and `pruning`:**
- `cutoffs + failed ≤ tried`.
- `floorbinds ≤ frontier skips`.

**Node totals:**
- `latenodes`, `reducednodes`, `researchnodes` and `failnodes` are each ≤ the total nodes.
- `maxdepth ≤ MAX_PLY`, and `roots > 0`.

## Validation

Engine tier, per PR.

**Tests:**
- A fixed kiwipete search at `Threads=1` asserts every invariant its PR adds.
- An exact payload test covers each line its PR adds, and its silence when empty (D5).
- 2b only: a test of the verification guard. `expected[ply]` reads ALL inside the guard's scope and
  its prior value after.
- Each assertion is falsified once.

**Equivalence:**
- Default build: `Compare-SearchEquivalence.ps1 -BaselineRef origin/main` reports IDENTICAL.
- Profile build: `-Before <default> -After <profile>` reports IDENTICAL.

**Port correctness (2a only):** a profile build at depth 16 on the 8 `Run-Bench` positions
reproduces #636's pooled table to its printed decimals.

**nps, default build:** the paired `Run-Bench.ps1` series in `measure-strength` →
`reference/regression-check.md`.
- The baseline is the merge base, built with its own `build.ps1 main` in a detached worktree.
- 6 pairs on the 8 default positions at `-Depth 12`. The first pair is discarded.
- Node counts match per position in every pair.
- **Done** is a per-pair aggregate delta whose spread sits at or above zero.
- A negative spread is escalated as that reference says, with a shared `/ORDER` relink (#555), and
  not concluded from.
- The report gives the mean, standard deviation and range of the kept pairs.

**No Elo run:** no search decision changes, and node identity proves it.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Flag contract, cold members, `if constexpr` only (D2) | header comment in `SearchTelemetry.h` |
| Output contract, bin edges, the "do not sum" and "max, not sum" rules (D3, D4) | struct comments; `Docs/Engine-Readme.md` |
| Nesting-exclusive method (D4) and expected-type rules (D6) | source comments at the `pvs()` write sites |
| Both-emit comparison rule (D5) | `Compare-SearchEquivalence.ps1` help |
| PR 3's self-check pair (D5) | #637 acceptance |
| Where each doc pointer lives (D7) | the named docs |
| Reproduced baseline (2a), first items 4-7 baseline (2b), nps results | PR bodies; `Docs/Changelog.md` |
