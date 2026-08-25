# Quiescence may use MAIN TT entries — Design

**Issue:** #337

## Goal

`quiescence()` uses a TT entry only when `entry->phase == SearchPhase::QUIESCENCE`. Since #319 a
quiescence store that lands on a main entry for the same key is declined, so at those keys quiescence
has no cache at all: it probes, finds a `MAIN` entry it refuses to read, re-searches, tries to store,
is declined, and repeats — every visit, for the whole iteration. The keys affected are the frontier
nodes `pvs()` stores at depth 1 and quiescence then revisits many times within one iteration, which
is why the effect concentrates in endgames.

## Scope

**This change will:**

- Let `quiescence()` take a bound from a `MAIN` entry, under a depth condition.

**This change will not:**

- Mine `best_move` from any entry (see D3).
- Change what quiescence stores, or the replacement policy in `TranspositionTable::store()`.
- Address the `closed-mid` regression it introduces (see D2) — that is filed separately.

## Decisions

### D1: accept any `MAIN` entry, `depth >= 1`

`pvs()` hands `depth <= 0` to `quiescence()` before it computes a key (`AIPerplex.cpp:487`), so every
main store covers a full-width ply plus the quiescence below it — strictly more search than a
quiescence node at the same position performs, never less. Its bound is therefore valid here. The
`depth >= 1` test asserts that locally rather than importing the guarantee from `pvs()`.

**Rejected: a higher threshold.** Four rules were benched at depth 12, `Threads=1`, against `main` @
`3840612` (total nodes, and the three positions that move):

| rule | total | rook-endgm | tactical-5 | closed-mid |
|---|---:|---:|---:|---:|
| `depth >= 1` (chosen) | −4.7% | −41.4% | −23.2% | +163.2% |
| MAIN + `EXACT` only | +2.9% | −19.0% | +35.6% | +23.7% |
| `depth >= 3` | −5.7% | −40.4% | −3.3% | +93.6% |
| `depth >= 5` | −3.2% | −40.7% | +41.1% | −2.2% |
| `depth * 2 >= qsearch_budget` | +0.1% | −0.0% | +0.0% | −0.5% |

`depth >= 3` and `depth >= 5` each look better than the chosen rule on one column and worse on
another, with opposite signs on `tactical-5`. That disagreement across eight positions is the
signature of a fitted constant, and no argument distinguishes 3 from 5 other than these numbers.
`depth >= 1` is the only rule with a reason behind it, so it is the one that goes to measurement.

**The last row is the informative negative.** `depth * 2 >= qsearch_budget` compares the two phases on
the TT's *own* phase-equivalence scale — the 0.5 discount `quiescenceEquivalentDepth()` applies in
`replacementScore()` — and is a near-total no-op. So the entire benefit comes from serving `MAIN`
entries that scale says are worth *less* than the quiescence node's budget. Soundness (a main entry
is strictly more real search) and the table's ranking disagree, and the win lives precisely in that
gap. Anyone tempted to "make the probe consistent with the replacement policy" is proposing to
delete the change.

### D2: ship despite the `closed-mid` regression

`closed-mid` costs +163% nodes, reproduced at depths 10/11/12/13 (+58/+56/+163/+115%) with the same
best move throughout, so it is systematic rather than search chaos at one depth. No variant in D1
removes it without giving back the endgame win.

Shipping anyway, because the measured strength result covers the whole set and is clearly positive
(see Validation), and because a threshold picked to hide one bench position is worse than a known
open question. Filed as a follow-up rather than tuned away.

### D3: do not mine `best_move`

`store()` inherits a same-key entry's move across a phase change, so an entry can hold a quiet move
this capture-only generator would never produce. The comment at that inheritance says it is inert
"while `quiescence()` never reads `best_move`". Reading it here would make that a defect, so the
probe takes bounds only and the inheritance stays inert.

### D4: relax `qnodes() > 0` in the warm-TT case

`Search - node counters reset between searches` fails after the change: on a warm-TT re-search of the
same position to the same depth, quiescence searches **zero** edges — every call is served at the
probe. That is legitimate under D1, and the test's subject is that counters *reset*, which its
sentinel and its independent-fixture assertions both still prove. The `qnodes() > 0` line is
over-specified against the new behaviour, so it moves to the cold-TT `fresh` fixture the test already
constructs.

This is worth a comment at the test, because "quiescence did no work" reads like a bug.

## Assumptions I cannot verify from the code

None. The one assumption this rests on — that there are no depth-0 `MAIN` entries — was checked:
`AIPerplex.cpp:487` returns before the key is computed, and all three `MAIN` stores (`:542`, `:685`,
`:705`) write the `depth` that guard has already bounded below by 1.

## Invariants

- Every `MAIN` entry in the table has `depth >= 1`. If that ever stops holding, D1's soundness
  argument goes with it — hence the explicit test in the probe rather than an unguarded phase check.
- `quiescence()` never reads `best_move` from the TT.
- Quiescence stores are unchanged: still `SearchPhase::QUIESCENCE`, still keyed on remaining budget.

## Validation

Engine tier. This changes what quiescence returns, so it is not equivalence-preserving and
`Compare-SearchEquivalence.ps1` does not apply — node counts and best moves are *expected* to differ.

- `Run-Bench.ps1`, depth 12, `Threads=1`, per-position: the table in D1. The two positions #319
  regressed are the two that recover, which was the falsification test for the issue's hypothesis.
- `Run-EloMatch.ps1 -Sprt NonRegression`, 500 games, 10+0.1, vs `main` @ `3840612`:
  **+35.56 ± 23.25, LOS 99.87%, LLR 1.33 — inconclusive at the cap, interval [+12.3, +58.8]**.
  Read per `Docs/EloMeasurement.md` § verdicts.
- Full test suite, with D4 applied.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1's soundness argument, and why `depth >= 1` is tested explicitly | source comment at the probe in `quiescence()` |
| D3: why `best_move` is not mined, tied to the inheritance comment in `store()` | source comment at the probe |
| D4: why zero qnodes is legitimate on a warm TT | comment at the test in `SearchTests.cpp` |
| The variant table, and the phase-equivalence-scale negative result | PR body, and `Docs/Changelog.md` |
| Elo result | `Docs/EloLog.md` (already landed), `Docs/Changelog.md` |
| D2: the `closed-mid` regression, unresolved | its own issue, opened before this document is deleted |

No approved decision changed during implementation: this document was written after the measurement,
so D1–D4 are the decisions as measured.
