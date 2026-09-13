# TT probe/store counters behind STRAT_TT_STATS — Design

**Issue:** #532

## Goal

`hashfull` says how much of the table is occupied, not whether that occupancy earns anything. The
capacity question left open by #442 — would a larger table have mattered at our TC — needs probe
outcomes. The counters sit on the per-node probe and store paths, so a build that does not ask for
them must not run them.

## Scope

**This change will:**

- add `STRAT_TT_STATS` (CMake cache option, default `0`), following `STRAT_FUTILITY_PROBE`;
- count per thread: main and quiescence probes, hits and cutoffs; stores by outcome (declined,
  filled an empty slot, refreshed the same key, evicted an entry written this search, evicted a
  stale one);
- sum them in `Search()` and print one `info string ttstats` line after a search;
- document how to read them and the workload trap.

**This change will not:**

- change any search or replacement decision — a stats build must be node-identical;
- run the Hash 192 vs 256 sweep (the issue's "first use"; a follow-up measurement).

## Decisions

### D1: store() reports its outcome through a return value

Probe-side counting happens at the call sites in `pvs()`/`quiescence()`, which hold `ThreadData`.
The store outcome is only known inside `store()`, which does not. `store()` therefore returns a
`TTStoreOutcome` enum the call site records under `if constexpr (kTTStatsCompiled)`.

Rejected: a `TTStats&` parameter (an extra argument on every store in the shipping build);
`thread_local` counters inside the table (hidden aggregation across threads); atomic counters in
the table (cross-thread cache contention in the stats build, and not per-thread). The return value
reuses locals `store()` already computes; the shipping build ignores it. Bench confirms the cost.

### D2: "still useful" is hashfull's definition

An evicted entry counts as useful when it was written since the current search began — the same
age window `hashfull()` counts, via one shared helper. Only the stats build does the comparison;
the table learns the search's start age from `Search()` under the same gate.

## Assumptions I cannot verify from the code

None beyond what bench and `Compare-SearchEquivalence.ps1` measure directly.

## Invariants

- `STRAT_TT_STATS=0`: node-identical to `main`, bench nps unchanged outside noise.
- `STRAT_TT_STATS=1`: node-identical too; `hits <= probes`, `cutoffs <= hits`, stores equal the sum
  of their outcomes.

## Validation

Engine tier. Unit test pins the counter invariants (the test binary always compiles the stats in).
`Compare-SearchEquivalence.ps1` against a `main` build for both configurations. `Run-Bench.ps1` on
the default build. No Elo match: no search decision changes.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1, D2 | source comments in `TTStats.h`; D1's rejected options in `Docs/Changelog.md` |
| how to read the counters, workload trap | `Docs/Engine-Readme.md` |
| bench/equivalence result | PR body, `Docs/Changelog.md` |
