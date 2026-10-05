# Lazy move ordering in pvs() — Design

**Issue:** #725

## Goal

`pvs()` scores its whole move list and then fully sorts it before searching the first move. On the
shipping clang-cl build, that sort (`std::_Sort_unchecked<std::pair<int,int>*>`, the symbol shared
by both `ScoreMoves` callers) is **15.9% of sampled self time**
([#719 profile](https://github.com/theEscape2207/StratChess/issues/719#issuecomment-5982919844)).
Most of the sort is wasted: in the probe below, **70.5% of scored `pvs()` nodes search at most one
move**. The goal is to stop sorting moves that are never searched, while leaving every node's move
order unchanged. The search tree must stay node-identical, so the only effect is on speed.

## Review focus

- **D1, the hybrid and Assumption A1.** The probe counts comparisons, not time. "Pick the first move,
  sort the rest only when move 2 is needed" beats a full sort on that proxy (0.48×). Pure selection
  barely does (0.91×), because 17% of nodes search every move. If comparisons are a poor stand-in
  for the sort's real cost, the gain shrinks. Only `Compare-Bench.ps1` settles it.
- **D3 and Invariant I3: every reader of `scored_idx`.** Lazy ordering is only safe if no code reads
  an entry beyond the current `si` before `OrderRemaining` has run. Check each reader at
  `AIPerplex.cpp:861,932,1026,1028,1120` and in `ThreadData::penalize_searched_quiets`.
- **Validation: no Elo match.** The 2026-10-04 triage expected a strength-lab gate for any change
  that claims a speed gain. This document argues that a node-identical tree plus a Speedup verdict
  is enough (see Validation). Challenge that if you disagree.

## Evidence: where the sort work goes

**Probe:** throwaway counters in `pvs()` and `quiescence()` (since reverted; the diff is not kept).
Run on clang-cl Release at `2a6062b`, over the eight `BenchPositions.ps1` positions at depth 13,
`Threads=1`, one fresh process per position. For each scored node the probe recorded the list size
*n* and how many entries the loop reached before it returned or broke ("consumed"). The cost proxy
is comparisons:
- a full sort is modelled as *n*·log₂*n*;
- selection costs *n − i* for the *i*-th pick;
- "pick K, then sort" costs K picks plus a sort of the remaining *n − K*.

| Caller | Nodes | Mean *n* | Consumed ≤ 1 | Consumed all | Pure selection | Pick 1, sort rest | Pick 2 | Pick 3 |
|---|---|---|---|---|---|---|---|---|
| `pvs()` | 5,569,570 | 31.9 | 70.5% | 17.0% | 0.91× | **0.475×** | 0.475× | 0.50× |
| quiescence in check | 1,302,798 | 35.5 | 33.7% | 44.0% | 1.96× | 0.82× | 0.86× | 0.92× |
| quiescence captures | 7,855,804 | 2.5 | 62.4% | 0% | n/a: already a stable insertion sort (#727) | | | |

The ratios are each caller's proxy cost divided by its full-sort proxy cost. `pvs()` accounts for
**79%** of the two `ScoreMoves` callers' combined proxy cost (902.8M of 1,144.3M).

**Bounded estimate, conditional on A1:** 79% of 15.9% puts about 12.6% of runtime in `pvs()`'s sort.
Halving it would save roughly **6% of runtime**. This is a proxy-derived ceiling, not a forecast.
**It is unknown until measured.**

## Scope

**This change will:**

- Split `MoveSorter::ScoreMoves` into two steps:
  - `ScoreMovesBestFirst`, which scores the list and moves the first entry in order to index 0;
  - `OrderRemaining`, which orders `[first, n)`.

  `ScoreMoves` becomes the composition of the two.
- Make `pvs()` call `ScoreMovesBestFirst`, and call `OrderRemaining(…, 1, n)` only when its loop
  reaches `si == 1`.
- Add unit tests that compare both paths against an independently computed reference order.
- Update the order-contract comments and `Docs/EngineContracts.md`, and add a `Docs/Changelog.md`
  entry.

**This change will not:**

- Change any move's score, any score tier, or the tie-break. The order is identical by construction
  and by test.
- Make in-check quiescence lazy. It keeps a full order through `ScoreMoves`. Its proxy gain is 0.82×
  on 21% of the sort work, about 4% of that work. Being lazy there would also mean holding the
  1.7 KiB score array across the first child's recursion. That breaks the frame-size contract in
  `order_quiescence_moves`, where an in-check chain is bounded only by `MAX_PLY`.
- Touch the out-of-check capture path. It is already an insertion sort over a mean 2.5 entries.
- Defer *scoring* itself (staged move generation, e.g. "search the hash move before scoring the
  rest"). Scores read history, killers and continuation history. The hash move's subtree writes all
  three, so scoring later produces different scores and a different tree. That is a behaviour
  change that needs an Elo measurement; it would be a separate issue if wanted.

## Decisions

### D1: Pick the first move, then sort the rest once

`pvs()` puts the first move in order at index 0 during scoring. It sorts `[1, n)` with the existing
comparator only when the loop reaches the second entry.

Rejected:
- **Incremental selection for every move:** 0.91× on the proxy, because the 17% of nodes that search
  every move pay O(*n*²).
- **Picking K = 2 or 3 before sorting:** no better than K = 1 on the proxy (0.475×, 0.50×), and it
  adds a threshold.
- **`std::partial_sort` / `nth_element`:** these are the same family as picking K. They give up the
  single-comparator construction in D2 and gain nothing over K = 1.

K = 1 has no tunable threshold, so this change adds no unmeasured constant.

### D2: One comparator; `ScoreMoves` becomes a composition

The comparator stays as it is: score descending, then the profile seed's key when it is non-zero,
then original index ascending. It moves into a named function in `Sort.cpp`'s anonymous namespace,
and both new functions use it. It is a strict total order over distinct list entries, so the
maximum is unique and sorting `[1, n)` after extracting it gives exactly the full sort's order.
Original indices are always distinct. The seed key is bijective on the move's 16 bits (assumption
A3 covers the seeded case).

`ScoreMovesBestFirst` tracks the running best while it scores: one comparator call per move inside
the loop that already exists. It then swaps that entry into index 0. `ScoreMoves` becomes
`ScoreMovesBestFirst` followed by `OrderRemaining(…, 1, n)`. This keeps its contract, its callers
(in-check quiescence, tests, `SearchTestFixture`) and its result unchanged.

Rejected: keeping `ScoreMoves`' own `std::sort` next to the new path. That leaves two copies of the
order, which could drift apart, and only a search-level equivalence run would catch it.

Signatures (in `Sort.h`):

```cpp
// As ScoreMoves, but only out_scored_idx[0] is in order; [1, n) is unordered until OrderRemaining.
static void ScoreMovesBestFirst(const MoveList& moveList, int n, const Board& board, eColor side,
                                const Move& hash_move, const Move& killer0, const Move& killer1,
                                const int32_t (&history)[2][64][64],
                                std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx,
                                ContinuationRows cont = {});
// Puts [first, n) of a ScoreMovesBestFirst result in ScoreMoves' order. moveList supplies the
// profile tie-break key.
static void OrderRemaining(const MoveList& moveList,
                           std::array<std::pair<int, int>, MoveList::MAX_MOVES>& scored_idx, int first, int n);
```

### D3: `OrderRemaining` runs at the top of the loop body when `si == 1`

It runs before the exclusion `continue` and before `move` is read. Every path that reaches the
second entry therefore orders the tail first, including the cases where entry 0 was the excluded
move, failed `DoMove()`, or was pruned. Cost: one predictable branch per iteration.

Rejected: running it after the first child search returns. Every `continue` path would skip it,
including an excluded or illegal first entry.

The pre-loop singular check reads only `scored_idx[0]`, which D2 guarantees is in order.

## Assumptions I cannot verify from the code

- **A1: comparison count predicts sort time.** `std::sort` is introsort with an insertion-sort
  finish, so its real cost is not exactly *n*·log₂*n* comparisons. Tracking the best while scoring
  adds a compare per move. Branch prediction and code placement shift either way.
  *Not verified.* Settled by `Compare-Bench.ps1`, which is the success criterion.
- **A2: the depth-13 bench mix represents games.** The 70.5% / 17% split comes from eight positions
  at depth 13. Game searches at time control go deeper, and the cut/all mix may differ. The #719
  profile found ordering's share steady from depth 13 to 16 (22.1% → 22.7% on Linux), but that is
  indirect. *Not verified.* It does not affect correctness. Only an Elo run measures the gain in
  games, and none is planned (see Validation).
- **A3: the move generator emits no duplicate moves.** This is needed only for the profile-seeded
  comparator: two equal moves would tie on the seed key. With seed 0, ties are impossible because
  indices are distinct. *Verified indirectly:* duplicates would inflate perft, and the perft corpus
  passes. The test binary runs with seed 0, so the seeded path is covered by construction, not by a
  test.
- **A4: the Windows 15.9% splits between callers as the comparison proxy does.** The profile cannot
  separate the two `ScoreMoves` callers because they share one symbol. *Not verified.* Checked
  indirectly by the before/after profile under Validation. If in-check quiescence's share were much
  larger than 21%, the sort symbol would stay above the predicted 8–12% band.

## Invariants

- **I1:** At every `pvs()` node, the sequence of `scored_idx[si]` the loop reads is identical to the
  pre-change full sort's, pair for pair.
- **I2:** After `ScoreMovesBestFirst`, `scored_idx[0]` is the first entry of that order, so the hash
  move is first whenever it is in the list. Singular eligibility at `AIPerplex.cpp:861` stays valid.
- **I3:** No code reads `scored_idx[i]` for `i ≥ 1` before `OrderRemaining(…, 1, n)` has run at that
  node. The LMR score read at `si`, `penalize_searched_quiets`' prefix `[0, si)`, and the loop's own
  read all happen at `si ≥ 1`, after the call.
- **I4:** Each `(score, index)` pair moves as a unit and is never split.
- **I5:** `ScoreMoves`' result is unchanged for every input, so in-check quiescence, the tests and
  `SearchTestFixture`'s predictions are unaffected.
- **I6:** No allocation, no new shared or mutable state, and no growth of the `pvs()` or quiescence
  frame. The array already lives in `pvs()`'s frame.

## Validation

**Engine tier.** `Validate-PrePR.ps1` runs the build, extended `[slow]` tests, the tactical suite and
self-play. CI adds Linux Debug with sanitizers and the Windows leg.

- **I1, I2, I5 (unit):** new `SortTests` cases compare against a reference computed independently of
  `Sort.cpp`'s comparator: score each list, then `std::ranges::stable_sort` by score descending.
  Stability gives the generation-order tie-break.
  - Positions: a cold history table (every quiet ties at 0); killers; a hash move; losing captures;
    promotions; *n* = 0, 1 and 2; and a list over 16 entries, so the test cannot pass on a small-sort
    path.
  - Assertions: `ScoreMovesBestFirst`'s `[0]` equals the reference's `[0]`; its `[1, n)` is a
    permutation of the reference's `[1, n)`; after `OrderRemaining(…, 1, n)` the array equals the
    reference pair for pair; and `ScoreMoves` equals the reference.
  - **Falsify:** flip the index tie-break in `OrderRemaining` and confirm the tests fail.
- **I1, I3 (search):** `Compare-SearchEquivalence.ps1 -After <candidate> -BaselineRef origin/main`
  must report IDENTICAL: node counts, best moves and every `info string` line. Comparing against the
  merge base also covers the cross-platform guarantee, because `2a6062b` is cross-platform equal and
  the comparator has no stdlib-dependent ties.
- **I6:** code review of the diff, plus a `/W4 /WX` build. There is no runtime check.
- **Speed (the success criterion):** follow `measure-strength` → `reference/regression-check.md`.
  `Compare-Bench.ps1` on clang-cl builds made by each tree's own `build.ps1` must give a **Speedup**
  verdict twice:
  - once from a `-Control` series;
  - once on the placement-equalised pair from `New-OrderedBuildPair.ps1`.

  Report the interval and the per-position rows.
  - **Park threshold:** a Slowdown, or an Unresolved result after the documented escalation, means
    the change is not merged. A node-identical change with no measured gain adds code for nothing.
- **Before/after CPU profile (diagnostic, both platforms):** repeat #719's profiles using the recipe
  in `Docs/Workflow.md` → Profiling. It shows whether the time left the sort, and where it went.
  - **Workload:** the eight `Run-Bench` positions at depth 13, `Threads=1`.
  - **Windows:** a clang-cl tree with `/Z7` and `/DEBUG /OPT:REF /OPT:ICF`, sampled with
    VSDiagnostics and read with xperf.
  - **Linux:** a GCC 15 Release build with `-g` in WSL Ubuntu-26.04, sampled with
    `perf -e cycles:u`.
  - **Baseline:** re-profile the merge base. Do not reuse #719's `cbfce87` numbers; #731 and #736
    have changed the tree since. Profile both arms on the same machine, back to back.
  - **Report:** group symbols into #719's areas (evaluation, move ordering, search bodies, move
    generation, TT locks, board, TT probe/store). Give a before/after table per platform, plus the
    sort symbol's own line. If LTO inlines the remaining sort into another symbol, the ordering area
    still covers it.
  - **Expected, declared before looking:**
    - The sort's absolute cost falls to about 0.585× of baseline: 79% of it shrinks to 0.475×, and
      in-check quiescence's 21% is unchanged.
    - From a Windows baseline near 16%, the sort symbol lands at **8–12% of samples**. Linux, from
      about 15%, lands in the same band.
    - The ordering area falls by **4–8 points**.
    - Each other area rises only by its proportional share of the smaller total. A larger rise in
      one area means the cost moved instead of disappearing; the most likely place is
      `ScoreMovesBestFirst`'s best-tracking compare.
  - **Decision rule:** if Windows still shows the sort at **≥ 14%**, the lazy path is not engaging
    or A1 is badly wrong. Find out why before spending a timing window. Otherwise the profile only
    explains; it does not gate. Collection costs about 30% nps, and the result is a single run with
    no interval, so a profile delta is never quoted as a speedup. `Compare-Bench.ps1` alone carries
    that claim. Windows is authoritative for the shipping effect. Linux shows what the strength lab
    will see.

**No Elo match.** At `Threads=1` the tree is node-identical, so at a fixed node count the engine
plays exactly the same moves. The change can affect strength only through nps, and a reproducible
Speedup verdict measures that directly. A lab run would measure the same effect less precisely:
about ±4 Elo for 3 hours of CI. If the owner wants an Elo figure anyway, it is the optional item
under Cost.

## Cost

- **Size:** 50–200 lines.
  - `Sort.h` and `Sort.cpp`: about 40 lines.
  - `AIPerplex.cpp`: about 10 lines, plus comment updates.
  - `SortTests.cpp`: about 80 lines.
  - `Docs/EngineContracts.md` and `Docs/Changelog.md`: about 15 lines.

  Six files.
- **Blast radius:** Engine tier. No gate, script or skill changes.
- **Review:** one code review (170–270k tokens, 3–5 min). A search-reviewer dispatch fits: the
  change touches move ordering.
- **Timing:**
  - default `Compare-Bench` series: about 5 min;
  - `-Control` series: about 5–35 min, depending on escalation;
  - ordered pair: one relink of both trees, plus another series.

  The timing runs need a quiet machine; tell the owner before a window starts.
- **Profiling:** about an hour of local wall time, no CI.
  - Four profile builds: baseline and candidate, on Windows and on Linux. The Linux builds come
    from a `git archive` in WSL.
  - Four short collection runs, about a minute each.
  - Symbol bucketing.

  Run it outside the timing window, because the profile builds would disturb `Compare-Bench`.
- **Optional:** a CI strength-lab run of about 3 hours (18/20 slots, about ±4 Elo nominal). It
  measures A2 and nothing else. The owner's call; it is not part of acceptance.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2: one comparator, `ScoreMoves` = best-first + `OrderRemaining` | `Sort.h` contract comments |
| D3 and I3: lazy tail, read nothing beyond `si` before ordering | comment at the `OrderRemaining` call in `pvs()`; `Docs/EngineContracts.md` → move ordering tripwire |
| Probe numbers (70.5% / 17%, proxy ratios), the bench result | `Docs/Changelog.md`, and the PR body |
| Before/after profile tables (both platforms) | #725 comment, as #719 recorded its profile; a one-line summary in `Docs/Changelog.md` |
| Rejected: in-check quiescence lazy, staged scoring | `Docs/Changelog.md` entry (one line each) |
