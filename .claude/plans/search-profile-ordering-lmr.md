# Search profile counters: ordering and LMR — Design

**Issue:** #637 (PR 2 of 3). Epic #636.

## Goal

Epic #636's children are judged by how much of the tree is spent before the cutting move and in
LMR re-searches. Today those numbers come only from a throwaway probe on pushed branch
`worktree-extended-futility-probe-634` (`2df7c67`, `0068128`). Every child would have to rebuild
that probe to measure itself against its merge base. This change lands the probe's counters as
permanent telemetry. It is compiled out of the shipping build and node-identical when compiled in.

## Scope

**This change will:**

- Add the compile flag `STRAT_SEARCH_PROFILE`, default 0, which follows the `STRAT_TT_STATS`
  contract.
- Add #637 item 3, ordering and LMR, as two `info string` payloads with fixed wording (D3).
- Teach `Compare-SearchEquivalence.ps1` to compare a profile build against a default build (D5).
- Document the flag and its lines in `Docs/Engine-Readme.md`, next to `ttstats`.

**This change will not:**

- Add #637 items 4-7 (tree shape, null move, pruning by depth, quiescence). See D1.
- Add the comparison script. That is PR 3, and it relies on the contract in D3.
- Split hash-move presence by depth band (#636 child C). That lands when C starts.
- Change any search decision, and any always-on counter.

## Decisions

### D1: Item 3 only; items 4-7 wait for their first user

#636's children consume item 3 and nothing else. It is the only item whose numbers #636's baseline
table reports, and the only one #637's acceptance ("reproduces #636's baseline table") needs.

Items 4-7 have no named consumer until #545 and #634 are re-screened after children A and B land.
Each of them adds per-node write sites that PR 3's output would then have to freeze. This is the
same rule that deferred the per-depth aspiration split. Each item lands, behind this flag, when a
question needs it.

Rejected: all of items 3-7 in one PR. It would be about four times the write sites, and it would
freeze formats for questions nobody has asked yet.

**#637's scope table changes with this.** Items 4-7 become "on first user", and #637 closes after
PR 3.

### D2: The flag, and where the counters live

`STRAT_SEARCH_PROFILE` is a CMake cache string, 0 or 1, and maps to
`inline constexpr bool kSearchProfileCompiled`. It is defined in `SearchTelemetry.h`, beside
`kSingularExtensionsCompiled`. The test target always defines it as 1, as it does for
`STRAT_TT_STATS`.

Two new structs, `OrderingStats` and `LmrStats`, each carry
`static constexpr bool compiled = kSearchProfileCompiled`. They are added last in
`SearchTelemetry`. They reuse the existing `reset`/`add`/`append_info` path, so neither Lazy SMP
summing nor UCI output needs new plumbing.

In the shipping build the members still exist as cold tail bytes of `ThreadData`. Every write site
is under `if constexpr`, and a discarded branch in non-template code is still type-checked, which
needs the members to exist. This is the trade `TTStats` already makes.

Rejected: an empty stand-in type with `[[no_unique_address]]` when the flag is off. The discarded
write sites would then fail to type-check.

The layout risk (memory: 144 bytes of cold code moved nps 3.9%) is closed by measurement, not
argument; see Validation.

### D3: Output contract (PR 3 parses this)

Each line is emitted only when its first field is non-zero, after the `aspiration` line:

```
info string ordering cuts N index I0/I1/I2/I3to5/I6plus latecut H/C/K/Q hashnodes N hashcuts N latenodes N latebands D1to2/D3to6/D7plus
info string lmr reduced N reducednodes N researched N confirmed N researchnodes N
```

- **Cuts:** fail-high nodes in `pvs()`, `ply > 0`.
  - `index` bins the legal index of the cutting move.
  - `latecut` classifies cuts at index > 0 by the cutting move: the hash move, else a capture or
    promotion, else a killer, else a quiet.
  - `hashnodes` counts cut nodes that had a hash move, and `hashcuts` those where the hash move
    was the cutting move.
- **`latenodes`:** nodes, main and quiescence, spent on the moves searched before the cutting move
  at a late-cut node. It is nesting-exclusive (D4). `latebands` splits it by the cut node's depth.
- **`reduced`:** LMR reduced searches.
  - `researched`: reduced searches that beat alpha and were searched again at full depth.
  - `confirmed`: re-searches that still beat alpha.
  - `reducednodes` and `researchnodes`: nodes inside each kind of search, outermost only.

Slash lists keep a fixed-length histogram to one token. Their bin edges are named in the header
comment and never change without changing the key.

These are exactly the fields #636's baseline table uses. The probe's `first`, `bandcuts`,
`bandfirst`, `idxsum` and `faillow` are dropped (D1).

### D4: Nesting-exclusive accounting, as the probe does it

- **`latenodes`:** at loop start, snapshot the node total and the running `latenodes`. Before each
  searched move's edge is counted, snapshot both again. On a cut at index > 0, add
  (move snapshot − loop snapshot) − (nested `latenodes` added in that span). A late-cut node nested
  inside another node's earlier moves is therefore counted once.
- **`reducednodes` and `researchnodes`:** a per-thread nesting depth for each kind. Nodes are added
  only when the outermost search of that kind returns. The decrement comes straight after the
  recursive call, before any abort return, so an abort cannot leave a depth unbalanced.
- **The two LMR totals are exclusive within their own kind only.** A reduced search inside a
  re-search counts in both, so they must not be summed. The header comment states this.

### D5: The equivalence gate compares profile payloads only when both builds emit them

`Compare-SearchEquivalence.ps1` already treats `treenodes` this way. `ordering` and `lmr` join that
rule, so a profile build compared with a default build of the same commit reads IDENTICAL.

Rejected: stripping the lines ad hoc outside the script, as #638 did. #637's acceptance names this
script as the gate, and a check done by hand is not repeatable.

## Assumptions I cannot verify from the code

- **The shipping build compiles the snapshot locals away.** Their only readers are discarded
  `if constexpr` branches. To be verified by the nps gate. A snapshot the optimiser keeps would
  show there first.
- **The ported counters reproduce the probe's numbers.** `pvs()`'s search logic is unchanged since
  the probe's base `af2f996`. Only telemetry commits touched `AIPerplex.cpp` since then, which I
  checked with `git log af2f996..origin/main`. So a profile build at depth 16 on the 8 `Run-Bench`
  positions must reproduce #636's baseline percentages. Verified when the run is made; see
  Validation.

## Invariants

- The default build is node-identical to the merge base and prints no new line.
- A profile build is node-identical to the default build at `Threads=1`.
- Per search:
  - the `index` bins sum to `cuts`
  - the `latecut` categories sum to `cuts − I0`
  - `hashcuts ≤ hashnodes ≤ cuts`
  - `latebands` sums to `latenodes`
  - `latenodes`, `reducednodes` and `researchnodes` are each ≤ the total nodes
  - `confirmed ≤ researched ≤ reduced`

## Validation

Engine tier.

- **Tests:** a fixed kiwipete search at `Threads=1` asserts every per-search invariant above. Each
  assertion is falsified once.
- **Default build:** `Compare-SearchEquivalence.ps1 -BaselineRef origin/main` reports IDENTICAL.
- **Profile build:** `Compare-SearchEquivalence.ps1 -Before <default> -After <profile>` reports
  IDENTICAL.
- **Port correctness:** the profile build at depth 16 on the 8 `Run-Bench` positions reproduces
  #636's pooled table to its printed decimals.
- **nps, default build:** interleaved wall clock at depth 14, `Threads=1`, 9 rounds against the
  merge base. The median must lie within the round spread #638 measured on this instrument
  (−1.39% to +1.04%), with identical nodes.
- **No Elo run:** no search decision changes, and node identity proves it.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Flag contract, cold members, `if constexpr` only (D2) | header comment in `SearchTelemetry.h` |
| Output contract, bin edges, the "do not sum the LMR totals" rule (D3, D4) | struct comments; `Docs/Engine-Readme.md` |
| Nesting-exclusive method (D4) | source comment at the `pvs()` write sites |
| Both-emit comparison rule (D5) | `Compare-SearchEquivalence.ps1` help |
| Items 4-7 on first user (D1) | #637 body; PR body |
| Reproduced baseline, nps result | PR body; `Docs/Changelog.md` |
