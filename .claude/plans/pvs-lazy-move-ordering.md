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
- **Validation: no Elo match.** This agrees with the
  [2026-10-05 re-triage](https://github.com/theEscape2207/StratChess/issues/725#issuecomment-5999220206):
  node equivalence plus a Speedup verdict is the gate, and Elo confirmation is optional. The owner
  has not yet agreed to it explicitly.

## Evidence: where the sort work goes

**Probe:** throwaway counters in `pvs()` and `quiescence()`, run on clang-cl Release at `2a6062b`.
The workload was the eight `BenchPositions.ps1` positions at depth 13, `Threads=1`, one fresh process
per position. The probe was run twice with identical output, and then reverted. Its diff, driver and
raw per-position output are in the appendix.

For each scored node it recorded the list size *n* and how many entries the loop reached before it
returned or broke ("consumed"). Each aggregate in the table below is a sum of those per-node costs,
not a value derived from the means. The cost proxy is comparisons:
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

**Bounded estimate, conditional on A1 and A4:** 79% of 15.9% puts about 12.5% of runtime in
`pvs()`'s sort. Halving it would save roughly **6.6% of runtime**. This is a proxy-derived ceiling,
not a forecast. **It is unknown until measured.**

## Scope

**This change will:**

- Factor `MoveSorter::ScoreMoves` into three pieces that share one scoring loop and one comparator:
  - `ScoreMovesBestFirst`: scores the list, then brings the first entry in order to index 0;
  - `OrderRemaining`: orders `[first, n)`;
  - `ScoreMoves`: scores the list, then calls `OrderRemaining(…, 0, n)`. This is the same work it
    does today.
- Make `pvs()` call `ScoreMovesBestFirst`, and call `OrderRemaining(…, 1, n)` only when its loop
  reaches `si == 1`.
- Add unit tests that compare both paths against an independently computed reference order.
- Update the order-contract comments and `Docs/EngineContracts.md`, and add a `Docs/Changelog.md`
  entry.

**This change will not:**

- Change any move's score, any score tier, or the tie-break. The order is identical by construction
  and by test.
- Make in-check quiescence lazy. It keeps a full order through `ScoreMoves`, with unchanged work.
  - Its proxy gain is 0.82× on 21% of the sort work, about 4% of that work.
  - Being lazy there would also mean holding the 1.7 KiB score array across the first child's
    recursion. That breaks the frame-size contract in `order_quiescence_moves`, where an in-check
    chain is bounded only by `MAX_PLY`.
- Touch the out-of-check capture path. It is already an insertion sort over a mean 2.5 entries.
- Defer *scoring* itself (staged move generation, e.g. "search the hash move before scoring the
  rest"). Scores read history, killers and continuation history. The hash move's subtree writes all
  three, so scoring later produces different scores and a different tree. That is a behaviour
  change that needs an Elo measurement; it would be a separate issue if wanted.

## Decisions

### D1: Pick the first move, then sort the rest once

`pvs()` brings the first move in order to index 0 with one linear pass after scoring. It sorts
`[1, n)` with the existing comparator only when the loop reaches the second entry.

Rejected:
- **Incremental selection for every move:** 0.91× on the proxy, because the 17% of nodes that search
  every move pay O(*n*²).
- **Picking K = 2 or 3 before sorting:** no better than K = 1 on the proxy (0.475×, 0.50×), and it
  adds a threshold.
- **`std::partial_sort` / `nth_element`:** these are the same family as picking K. They give up the
  single-comparator construction in D2 and gain nothing over K = 1.

K = 1 has no tunable threshold, so this change adds no unmeasured constant.

### D2: One scoring loop and one comparator, three entry points

**Comparator.** The comparator moves verbatim from the `std::sort` lambda at `Sort.cpp:148-154` into
a named function in `Sort.cpp`'s anonymous namespace. Its branches are unchanged:
- different scores order by score, descending;
- otherwise, when the seed is non-zero, by the seed key alone;
- otherwise by original index, ascending.

All three entry points use it. The seed-key branch does not fall through to the index.

**Why the order cannot change.**
- With seed 0 the comparator is a strict total order on any list, because original indices are
  always distinct.
- With a non-zero seed it is a strict total order on lists of distinct moves, because the key is
  bijective on the move's 16 bits (A3).

Where it is a strict total order, the maximum is unique, so sorting `[1, n)` after extracting it
gives exactly the full sort's order.

**The three entry points.**
- **Scoring:** one internal function writes the unordered `(score, index)` pairs. It is the current
  scoring loop, moved, not copied.
- **`ScoreMovesBestFirst`:** scores, then runs one linear max pass (*n − 1* comparator calls) and
  swaps the winner into index 0. It does nothing beyond scoring when *n* ≤ 1.
- **`ScoreMoves`:** scores, then calls `OrderRemaining(…, 0, n)`. That is the same full sort it
  does today, so in-check quiescence pays no best-tracking pass.

Rejected:
- **Composing `ScoreMoves` as best-first plus a sort of `[1, n)`.** It is tidier, but it adds the
  max pass to every in-check quiescence node: about 1.16× that path's proxy cost at a mean *n* of
  35.5.
- **Keeping `ScoreMoves`' own `std::sort` lambda next to the new path.** That leaves two copies of
  the order, which could drift apart, and only a search-level equivalence run would catch it.

Signatures (in `Sort.h`):

```cpp
// As ScoreMoves, but only out_scored_idx[0] is in order; [1, n) is unordered until OrderRemaining.
// For n <= 1 the result is already fully ordered.
static void ScoreMovesBestFirst(const MoveList& moveList, int n, const Board& board, eColor side,
                                const Move& hash_move, const Move& killer0, const Move& killer1,
                                const int32_t (&history)[2][64][64],
                                std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx,
                                ContinuationRows cont = {});
// Puts [first, n) in ScoreMoves' order. Requires 0 <= first and n <= MAX_MOVES; does nothing when
// fewer than two entries remain (n - first < 2), so OrderRemaining(.., 1, 0) is a no-op.
// moveList supplies the profile tie-break key.
static void OrderRemaining(const MoveList& moveList,
                           std::array<std::pair<int, int>, MoveList::MAX_MOVES>& scored_idx, int first, int n);
```

### D3: `OrderRemaining` runs at the top of the loop body when `si == 1`

It runs before the exclusion `continue` and before `move` is read. Every path that reaches the
second entry therefore orders the tail first, including the cases where entry 0 was the excluded
move, failed `DoMove()`, or was pruned. Cost: one predictable branch per iteration. The loop reaches
`si == 1` only when *n* ≥ 2.

Rejected: running it after the first child search returns. Every `continue` path would skip it,
including an excluded or illegal first entry.

The pre-loop singular check reads only `scored_idx[0]`, and only when `n > 0`. D2 guarantees that
entry is in order.

## Assumptions I cannot verify from the code

- **A1: comparison count predicts sort time.** `std::sort` is introsort with an insertion-sort
  finish, so its real cost is not exactly *n*·log₂*n* comparisons. The max pass is a separate linear
  scan. Branch prediction and code placement shift either way.
  *Not verified.* Settled by `Compare-Bench.ps1`, which is the success criterion.
- **A2: the depth-13 bench mix represents games.** The 70.5% / 17% split comes from eight positions
  at depth 13. Game searches at time control go deeper, and the cut/all mix may differ. The #719
  profile found ordering's share steady from depth 13 to 16 (22.1% → 22.7% on Linux, at an older
  revision). That is about aggregate shares, not the consumption distribution. *Not verified.* It
  does not affect correctness. Only an Elo run measures the gain in timed games, and none is
  planned (see Validation).
- **A3: lists contain distinct moves.** This is needed only for the profile-seeded comparator, and
  `MoveList::push` does not enforce it. Two equal moves would tie on the seed key, and their
  relative order is unspecified before and after this change alike. With seed 0, ties are
  impossible because indices are distinct.
  - The generator emits no duplicates: perft would count them, and the corpus passes. That is
    finite coverage, not a proof.
  - The seeded path gets its own search-level check under Validation.
- **A4: the Windows 15.9% splits between callers as the comparison proxy does.** The profile cannot
  separate the two `ScoreMoves` callers because they share one symbol. *Not verified.* The
  before/after profile under Validation bears on it, together with A1 and symbol attribution.

## Invariants

- **I1:** At every `pvs()` node, the sequence of `scored_idx[si]` the loop reads is identical to the
  pre-change full sort's, pair for pair. This holds for any list with seed 0, and for lists of
  distinct moves with a non-zero seed.
- **I2:** After `ScoreMovesBestFirst` with `n > 0`, `scored_idx[0]` is the first entry of that
  order, so the hash move is first whenever it is in the list. Singular eligibility at
  `AIPerplex.cpp:861` stays valid.
- **I3:** No code reads `scored_idx[i]` for `i ≥ 1` before `OrderRemaining(…, 1, n)` has run at that
  node. The LMR score read at `si`, `penalize_searched_quiets`' prefix `[0, si)`, and the loop's own
  read all happen at `si ≥ 1`, after the call.
- **I4:** Each `(score, index)` pair moves as a unit and is never split.
- **I5:** `ScoreMoves`' result and work are unchanged under the same condition as I1. In-check
  quiescence, the tests and `SearchTestFixture`'s predictions are therefore unaffected.
- **I6:** No allocation, no new shared or mutable state, and no new array in `pvs()` or quiescence;
  `pvs()` adds only scalars. This is a source-level constraint. Machine frame size is not separately
  inspected.

## Validation

**Engine tier.** `Validate-PrePR.ps1` runs the build, extended `[slow]` tests, the tactical suite and
self-play. CI adds Linux Debug with sanitizers and the Windows leg.

- **I1, I2, I5 (unit):** new `SortTests` cases compare against a reference computed independently of
  `Sort.cpp`'s comparator. The reference takes the scored pairs, puts them in original-index order
  (from either production output, which is already reordered), then applies
  `std::ranges::stable_sort` by score descending. Stability gives the generation-order tie-break.
  - **Positions:**
    - a cold history table, where every quiet ties at 0;
    - killers, a hash move, losing captures and promotions;
    - *n* = 0, 1 and 2;
    - a list of **more than 32** entries, beyond both libstdc++'s (16) and MSVC STL's (32)
      small-sort paths, as the existing 64-entry tests do. It includes tied entries whose
      generation order is disturbed when a later best entry is swapped into index 0.
  - **Assertions** (for *n* > 0; for *n* = 0 every call is a no-op):
    - `ScoreMovesBestFirst`'s `[0]` equals the reference's `[0]`;
    - its `[1, n)` is a permutation of the reference's `[1, n)`;
    - after `OrderRemaining(…, 1, n)` the array equals the reference pair for pair;
    - `ScoreMoves` equals the reference.
  - **Falsify:** flip the index tie-break in the comparator and confirm the tests fail.
- **I1, I3 (search):** `Compare-SearchEquivalence.ps1 -After <candidate> -BaselineRef <merge-base sha>`
  must report IDENTICAL: node counts, best moves and every `info string` line. Pin the SHA, because
  `origin/main` is resolved directly rather than as a merge base. Record the SHA if the branch
  rebases.
- **I1 with a non-zero seed (A3):** build baseline and candidate with `-DSTRAT_SEARCH_PROFILE=1`.
  Run `Compare-SearchEquivalence.ps1 -Before <baseline> -After <candidate>` with
  `STRAT_PROFILE_TIEBREAK_SEED=7` set in the calling environment. The engine reads the seed before
  `main()`, so each fresh engine process picks it up. Both sides must print
  `info string tiebreak seed 7`, and the result must be IDENTICAL, including the profile build's
  ordering and LMR telemetry lines.
- **Cross-platform:** at the candidate commit, a Linux GCC 15 build (WSL) and the Windows clang-cl
  build must report identical node counts and best moves on the eight bench positions at depth 13,
  `Threads=1`. Use matched options and a fresh process per position. This is #727's acceptance check,
  rerun on the candidate. The Linux and Windows profile arms below can supply the transcripts if
  their driver records each position's final `info … nodes` line and `bestmove` from a completed
  search.
- **I6:** code review of the diff, plus a `/W4 /WX` build.
- **Speed (the success criterion):** follow `measure-strength` → `reference/regression-check.md`.
  `Compare-Bench.ps1` on clang-cl builds made by each tree's own `build.ps1` must give a **Speedup**
  verdict twice. Both series run with `-Control`:
  - once on the two `build.ps1` builds;
  - once on the placement-equalised pair from `New-OrderedBuildPair.ps1`.

  Report the interval and the per-position rows.
  - **Park threshold:** a Slowdown, or an Unresolved result after the documented escalation, means
    the change is not merged. A node-identical change with no measured gain adds code for nothing.
- **Before/after CPU profile (diagnostic, both platforms):** repeat #719's profiles using the recipe
  in `Docs/Workflow.md` → Profiling. It shows whether time left the sort.
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
  - **Expected, declared before looking.** These are conditional proxy estimates (A1, A4), measured
    against the newly collected baseline:
    - The sort's absolute cost falls to about 0.585× of baseline: 79% of it shrinks to 0.475×, and
      in-check quiescence's 21% does the same work as before (D2).
    - From a baseline sort share near 16%, the sort symbol lands at **8–12% of samples**. The
      ordering area falls by **4–8 points**. The max pass shows up under ordering, not elsewhere.
  - **Decision rule:** the profile prompts questions; it does not gate.
    - If the ordering area's share barely moves from the new baseline (the sort symbol stays at
      ≥ 14%), check before spending a timing window. Possible explanations are the lazy path not
      engaging, A1, A4, or symbol attribution.
    - Shares share a changing denominator, so a percentage-point rise in another area does not by
      itself show that cost moved there.
    - Collection costs about 30% nps, and the result is a single run with no interval, so a profile
      delta is never quoted as a speedup. `Compare-Bench.ps1` alone carries that claim.
    - Windows is authoritative for the shipping effect. Linux shows what the strength lab will see.

**No Elo match.** At `Threads=1` the tree is node-identical, so at a fixed node count the engine
plays exactly the same moves. The change can affect strength only through search speed, and the
Speedup verdict measures that on the bench workload. Timed-game Elo stays unmeasured (A2). A lab run
would measure game strength under the lab's Linux/GCC build and time control, which is a different
quantity from Windows nps. The owner can add it; it is the optional item under Cost.

## Cost

- **Size:** 50–200 lines.
  - `Sort.h` and `Sort.cpp`: about 50 lines.
  - `AIPerplex.cpp`: about 10 lines, plus comment updates.
  - `SortTests.cpp`: about 90 lines.
  - `Docs/EngineContracts.md` and `Docs/Changelog.md`: about 15 lines.

  Six files.
- **Blast radius:** Engine tier. No gate, script or skill changes.
- **Review:** one code review (170–270k tokens, 3–5 min). A search-reviewer dispatch fits: the
  change touches move ordering.
- **Equivalence:**
  - the default comparison, which builds and caches its own baseline;
  - two `-DSTRAT_SEARCH_PROFILE=1` builds for the seeded run, about 10 min;
  - the cross-platform check, which reuses the profile arms' runs.
- **Timing** (author estimates):
  - each `Compare-Bench` series: about 5 min;
  - control escalation: up to about 35 min;
  - ordered pair: one relink of both trees, plus another series.

  The timing runs need a quiet machine; tell the owner before a window starts.
- **Profiling:** about an hour of local wall time (author estimate), no CI.
  - Four profile builds: baseline and candidate, on Windows and on Linux. The Linux builds come
    from a `git archive` in WSL.
  - Four short collection runs.
  - Symbol bucketing.

  Run it outside the timing window, because the profile builds would disturb `Compare-Bench`.
- **Optional:** a CI strength-lab run of about 3 hours (18/20 slots, about ±4 Elo nominal). It
  measures timed-game strength on Linux/GCC (A2). The owner's call; it is not part of acceptance.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2: one comparator, the three entry points, the `OrderRemaining` range contract | `Sort.h` contract comments |
| D3 and I3: lazy tail, read nothing beyond `si` before ordering | comment at the `OrderRemaining` call in `pvs()`; `Docs/EngineContracts.md` → move ordering tripwire |
| Probe numbers (70.5% / 17%, proxy ratios), the bench result | `Docs/Changelog.md`, and the PR body; the raw probe stays in this document's git history |
| Before/after profile tables (both platforms) | #725 comment, as #719 recorded its profile; a one-line summary in `Docs/Changelog.md` |
| Rejected: in-check quiescence lazy, staged scoring | `Docs/Changelog.md` entry (one line each) |
| Review findings rejected or changed (none rejected) | PR body |

## Appendix: probe artifacts

Applied to `2a6062b` with `git apply`, then built with `build.ps1 main` (clang-cl Release). Counts
are per scored node. A frame that aborts or returns before its move loop is not recorded.

<details><summary>Probe diff</summary>

```diff
diff --git a/StratEngine/AIPerplex.cpp b/StratEngine/AIPerplex.cpp
index a6026ee..60bdc7d 100644
--- a/StratEngine/AIPerplex.cpp
+++ b/StratEngine/AIPerplex.cpp
@@ -14,6 +14,7 @@
 #include "MoveFormatter.h"
 #include <bit> // std::popcount
 #include <cmath>
+#include <cstdio>
 #include <cstdlib>
 #include <cstring>
 #include <iterator>
@@ -25,6 +26,35 @@
 #include <spdlog/sinks/basic_file_sink.h>
 #include <spdlog/sinks/stdout_color_sinks.h>
 
+namespace {
+	// PROBE (#725 design, throwaway): sort work by caller.
+	struct OrderProbe {
+		const char* name;
+		long long calls = 0, sum_n = 0, sum_consumed = 0, finished = 0;
+		double sort_cost = 0, sel_cost = 0; double hyb[5] = {};
+		long long hist[8] = {};
+		void record(int n, int consumed)
+		{
+			calls++; finished++;
+			sum_n += n; sum_consumed += consumed;
+			if (n > 1) sort_cost += n * std::log2(static_cast<double>(n));
+			for (int i = 0; i < consumed; ++i) sel_cost += n - i;
+			for (int k = 1; k <= 4; ++k) { double c = 0; if (consumed <= k) { for (int i = 0; i < consumed; ++i) c += n - i; } else { for (int i = 0; i < k && i < n; ++i) c += n - i; const int m = n - k; if (m > 1) c += m * std::log2(static_cast<double>(m)); } hyb[k] += c; }
+			hist[consumed <= 1 ? 0 : consumed <= 2 ? 1 : consumed <= 3 ? 2 : consumed <= 5 ? 3 : consumed <= 10 ? 4 : consumed <= 20 ? 5 : consumed < n ? 6 : 7]++;
+		}
+		~OrderProbe()
+		{
+			std::fprintf(stderr, "PROBE %s calls %lld avg_n %.2f avg_consumed %.2f sort_cost %.4g sel_cost %.4g ratio %.3f hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all)", name, calls,
+			             calls ? double(sum_n) / calls : 0.0, calls ? double(sum_consumed) / calls : 0.0, sort_cost, sel_cost, sort_cost > 0 ? sel_cost / sort_cost : 0.0);
+			for (int k = 1; k <= 4; ++k) std::fprintf(stderr, " hyb%d %.4g", k, hyb[k]); for (long long h : hist) std::fprintf(stderr, " %lld", h);
+			std::fprintf(stderr, "\n");
+		}
+	};
+	OrderProbe g_probe_pvs{"pvs"};
+	OrderProbe g_probe_evasion{"qs_evasion"};
+	OrderProbe g_probe_capture{"qs_capture"};
+} // namespace
+
 namespace {
 	// Lazy SMP thread-safety: s_logger's sinks
 	// (stdout_color_sink_mt, basic_file_sink_mt — see ensure_logger_initialized()
@@ -928,7 +958,9 @@ int AIPerplex::pvs(ThreadData& td, int depth, int alpha, int beta, int ply, bool
 	ThreadData::SearchedMoves searched;
 
 	// Iterate by sorted index — no rebuild of move_list needed
+	int probe_consumed = 0;
 	for (int si = 0; si < n; ++si) {
+		probe_consumed = si + 1;
 		const Move& move = move_list[scored_idx[si].second];
 
 		// The move a verification search is proving the alternatives against is not one of
@@ -1124,6 +1156,7 @@ int AIPerplex::pvs(ThreadData& td, int depth, int alpha, int beta, int ply, bool
 		}
 	}
 
+	g_probe_pvs.record(n, probe_consumed);
 	// A skipped move was not searched, only judged unable to beat static_eval + margin, so the node
 	// may claim no less. Quiescence fails high at exactly its beta, so a fail-low here is usually
 	// exactly alpha already; this binds when a searched child returned below alpha -- a draw, or a
@@ -1488,7 +1521,11 @@ int AIPerplex::quiescence(ThreadData& td, int alpha, int beta, int qsearch_budge
 		                std::popcount(qboards[ePiece::ALL_BLACK_PIECES])) >= MATERIAL_PRUNING_MIN_PIECES;
 	}();
 
+	int probe_consumed = 0;
+	const int probe_n = static_cast<int>(move_list.size());
+	auto& probe = in_check ? g_probe_evasion : g_probe_capture;
 	for (const auto& move : move_list) {
+		probe_consumed++;
 		// Promotions stay: their tactical value is not bounded by immediate material gain.
 		if (material_bounds_hold && !MoveHelper::IsPromote(move) &&
 		    stand_pat +
@@ -1539,6 +1576,7 @@ int AIPerplex::quiescence(ThreadData& td, int alpha, int beta, int qsearch_budge
 		move_found = true;
 
 		if (score >= beta) {
+			probe.record(probe_n, probe_consumed);
 			record_tt_store(td, tt.store(key, static_cast<int16_t>(beta), static_cast<int16_t>(qsearch_budget),
 			                             static_cast<int16_t>(ply), move, BoundType::LOWER, NodeType::CUT_NODE,
 			                             SearchPhase::QUIESCENCE));
@@ -1550,6 +1588,7 @@ int AIPerplex::quiescence(ThreadData& td, int alpha, int beta, int qsearch_budge
 			best_move = move;
 		}
 	}
+	probe.record(probe_n, probe_consumed);
 	// In check, the move list was every legal evasion, so no survivor means checkmate — the
 	// one terminal state quiescence can identify with certainty. Score it by ply so shorter
 	// mates are preferred, and store it exact with an empty move: best_value is still the
```

</details>

<details><summary>Driver (<code>probe_run.py 13</code>)</summary>

```python
import subprocess, sys, re, collections
EXE = r"C:\Users\thees\source\repos\StratChessEvolved\.claude\worktrees\vivid-splashing-babbage\build\windows-clang-cl\StratChessEvolved.exe"
DEPTH = int(sys.argv[1]) if len(sys.argv) > 1 else 13
FENS = {
 'startpos': 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
 'kiwipete': 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1',
 'rook-endgm': '2r3k1/1p3pp1/p3p2p/8/2PR4/1P3P2/P4KPP/8 w - - 0 1',
 'tactical-4': 'r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1',
 'tactical-5': 'rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8',
 'open-mid': 'r1bqkb1r/pp3ppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP3PPP/RN1QK2R w KQkq - 0 7',
 'closed-mid': 'r1bq1rk1/pp2ppbp/2np1np1/8/2PNP3/2N1B3/PP2BPPP/R2QK2R w KQ - 0 9',
 'piece-endgm': '2r3k1/pp3pp1/4p2p/3n4/3P4/P1NBP3/1P3PPP/2R3K1 w - - 0 1',
}
tot = collections.defaultdict(lambda: collections.defaultdict(float))
for name, fen in FENS.items():
    p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
    def send(x): p.stdin.write(x + "\n"); p.stdin.flush()
    send("uci"); send("setoption name Threads value 1"); send("isready")
    for line in p.stdout:
        if line.startswith("readyok"): break
    send("position fen " + fen); send(f"go depth {DEPTH}")
    nodes = None
    for line in p.stdout:
        m = re.search(r" nodes (\d+)", line)
        if m: nodes = m.group(1)
        if line.startswith("bestmove"): best = line.strip(); break
    send("quit")
    _, err = p.communicate(timeout=60)
    print(name, nodes, best)
    for l in err.splitlines():
        if l.startswith("PROBE"):
            print("  ", l)
            f = l.split()
            who = f[1]
            kv = dict(zip(f[2:14:2], f[3:14:2]))
            calls = float(kv['calls'])
            t = tot[who]
            t['calls'] += calls; t['n'] += calls * float(kv['avg_n']); t['cons'] += calls * float(kv['avg_consumed'])
            t['sort'] += float(kv['sort_cost']); t['sel'] += float(kv['sel_cost'])
            for k, v in re.findall(r'hyb(\d) (\S+)', l): t['hyb'+k] += float(v)
            hist = list(map(int, f[-8:]))
            for i, h in enumerate(hist): t[f'h{i}'] += h
print("\nTOTAL")
for who, t in tot.items():
    c = t['calls']
    print(f"{who}: calls {c:.0f} avg_n {t['n']/c:.2f} avg_consumed {t['cons']/c:.2f} sort_cost {t['sort']:.4g} sel_cost {t['sel']:.4g} ratio {t['sel']/t['sort'] if t['sort'] else 0:.3f}")
    print("   hybrid/sort ratio K=1..4:", [round(t[f"hyb{k}"]/t["sort"],3) for k in range(1,5)])
    print("   consumed hist (<=1,2,3,4-5,6-10,11-20,>20 partial,all):", [f"{t[f'h{i}']/c*100:.1f}%" for i in range(8)])
```

</details>

<details><summary>Raw output</summary>

```text
startpos 2339351 bestmove e2e4
   PROBE qs_capture calls 555472 avg_n 1.67 avg_consumed 1.28 sort_cost 1.205e+06 sel_cost 1.366e+06 ratio 1.134 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.197e+06 hyb2 1.296e+06 hyb3 1.341e+06 hyb4 1.357e+06 380344 109904 44448 19139 1451 186 0 0
   PROBE qs_evasion calls 80123 avg_n 34.38 avg_consumed 12.62 sort_cost 1.411e+07 sel_cost 1.926e+07 ratio 1.365 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 9.674e+06 hyb2 9.522e+06 hyb3 1.004e+07 hyb4 1.063e+07 39096 7299 2009 1935 1302 686 195 27601
   PROBE pvs calls 383883 avg_n 33.88 avg_consumed 7.13 sort_cost 6.634e+07 sel_cost 5.33e+07 ratio 0.803 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 3.064e+07 hyb2 2.984e+07 hyb3 3.113e+07 hyb4 3.263e+07 273864 21862 5368 4617 3546 2462 329 71835
kiwipete 9812419 bestmove e2a6
   PROBE qs_capture calls 2220816 avg_n 4.08 avg_consumed 2.48 sort_cost 2.097e+07 sel_cost 1.802e+07 ratio 0.859 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.595e+07 hyb2 1.659e+07 hyb3 1.711e+07 hyb4 1.75e+07 1026244 349381 289519 357774 189454 8444 0 0
   PROBE qs_evasion calls 579802 avg_n 40.65 avg_consumed 22.25 sort_cost 1.266e+08 sel_cost 2.815e+08 ratio 2.223 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.106e+08 hyb2 1.169e+08 hyb3 1.246e+08 hyb4 1.335e+08 163532 32599 19733 17886 23876 9974 2350 309852
   PROBE pvs calls 1086523 avg_n 41.51 avg_consumed 11.66 sort_cost 2.432e+08 sel_cost 2.819e+08 ratio 1.159 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.254e+08 hyb2 1.28e+08 hyb3 1.353e+08 hyb4 1.434e+08 695518 44795 14796 13324 11568 6142 1809 298571
rook-endgm 5246287 bestmove f2e3
   PROBE qs_capture calls 1134855 avg_n 0.81 avg_consumed 0.70 sort_cost 6.079e+05 sel_cost 1.104e+06 ratio 1.816 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 9.667e+05 hyb2 1.084e+06 hyb3 1.102e+06 hyb4 1.104e+06 995245 119310 18403 1890 7 0 0 0
   PROBE qs_evasion calls 168630 avg_n 21.01 avg_consumed 9.27 sort_cost 1.576e+07 sel_cost 1.872e+07 ratio 1.188 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.201e+07 hyb2 1.245e+07 hyb3 1.319e+07 hyb4 1.385e+07 64227 9338 4300 6820 15509 38326 107 30003
   PROBE pvs calls 1501058 avg_n 21.83 avg_consumed 4.31 sort_cost 1.467e+08 sel_cost 8.349e+07 ratio 0.569 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 5.968e+07 hyb2 6.057e+07 hyb3 6.33e+07 hyb4 6.591e+07 1144535 38278 8936 10907 41054 155007 165 102176
tactical-4 1655080 bestmove c4c5
   PROBE qs_capture calls 413324 avg_n 3.93 avg_consumed 2.42 sort_cost 3.67e+06 sel_cost 3.407e+06 ratio 0.928 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 3.032e+06 hyb2 3.099e+06 hyb3 3.193e+06 hyb4 3.265e+06 204230 67379 47501 56963 33571 3677 0 3
   PROBE qs_evasion calls 98867 avg_n 37.73 avg_consumed 22.23 sort_cost 1.963e+07 sel_cost 4.432e+07 ratio 2.258 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.667e+07 hyb2 1.791e+07 hyb3 1.942e+07 hyb4 2.096e+07 30219 3917 1796 1869 2612 1159 428 56867
   PROBE pvs calls 161884 avg_n 37.60 avg_consumed 11.80 sort_cost 3.201e+07 sel_cost 3.869e+07 ratio 1.208 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.857e+07 hyb2 1.847e+07 hyb3 1.95e+07 hyb4 2.072e+07 92829 10226 2857 2237 2204 1248 239 50044
tactical-5 3211569 bestmove d7c8q
   PROBE qs_capture calls 689733 avg_n 1.84 avg_consumed 1.34 sort_cost 1.8e+06 sel_cost 1.908e+06 ratio 1.060 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.696e+06 hyb2 1.809e+06 hyb3 1.869e+06 hyb4 1.894e+06 472156 123563 57925 31183 4889 17 0 0
   PROBE qs_evasion calls 175568 avg_n 38.13 avg_consumed 16.72 sort_cost 3.54e+07 sel_cost 6.045e+07 ratio 1.708 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 2.618e+07 hyb2 2.682e+07 hyb3 2.855e+07 hyb4 3.041e+07 72646 11997 4101 4241 3656 2767 991 75169
   PROBE pvs calls 502408 avg_n 39.52 avg_consumed 8.92 sort_cost 1.059e+08 sel_cost 9.828e+07 ratio 0.928 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 5.15e+07 hyb2 5.048e+07 hyb3 5.303e+07 hyb4 5.569e+07 337757 28473 6672 8383 7881 5260 1130 106852
open-mid 4239769 bestmove e1g1
   PROBE qs_capture calls 1015981 avg_n 2.53 avg_consumed 1.82 sort_cost 4.265e+06 sel_cost 4.28e+06 ratio 1.004 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 3.793e+06 hyb2 4.011e+06 hyb3 4.154e+06 hyb4 4.229e+06 547880 221821 129890 100065 16211 114 0 0
   PROBE qs_evasion calls 66577 avg_n 36.71 avg_consumed 13.84 sort_cost 1.275e+07 sel_cost 1.852e+07 ratio 1.452 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 8.732e+06 hyb2 8.75e+06 hyb3 9.219e+06 hyb4 9.772e+06 31860 5169 1843 1593 980 795 556 23781
   PROBE pvs calls 602802 avg_n 38.24 avg_consumed 9.09 sort_cost 1.216e+08 sel_cost 1.162e+08 ratio 0.956 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 5.659e+07 hyb2 5.683e+07 hyb3 6.005e+07 hyb4 6.347e+07 423596 25708 5878 5518 4963 2034 704 134401
closed-mid 2586463 bestmove e1g1
   PROBE qs_capture calls 759317 avg_n 2.61 avg_consumed 1.79 sort_cost 3.321e+06 sel_cost 3.186e+06 ratio 0.959 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 2.866e+06 hyb2 3.004e+06 hyb3 3.103e+06 hyb4 3.155e+06 410332 172672 97613 69436 9229 35 0 0
   PROBE qs_evasion calls 32244 avg_n 38.77 avg_consumed 19.45 sort_cost 6.629e+06 sel_cost 1.341e+07 ratio 2.023 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 5.443e+06 hyb2 5.506e+06 hyb3 5.832e+06 hyb4 6.281e+06 11260 3029 1086 310 539 450 388 15182
   PROBE pvs calls 346137 avg_n 40.79 avg_consumed 9.18 sort_cost 7.579e+07 sel_cost 7.139e+07 ratio 0.942 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 3.516e+07 hyb2 3.465e+07 hyb3 3.651e+07 hyb4 3.854e+07 242106 17995 3674 3460 3842 1256 553 73251
piece-endgm 4594688 bestmove d3e4
   PROBE qs_capture calls 1066306 avg_n 1.16 avg_consumed 0.93 sort_cost 1.156e+06 sel_cost 1.58e+06 ratio 1.366 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 1.396e+06 hyb2 1.533e+06 hyb3 1.572e+06 hyb4 1.579e+06 866436 152305 39540 7959 66 0 0 0
   PROBE qs_evasion calls 100987 avg_n 22.96 avg_consumed 14.06 sort_cost 1.064e+07 sel_cost 1.762e+07 ratio 1.656 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 9.159e+06 hyb2 9.668e+06 hyb3 1.048e+07 hyb4 1.128e+07 26849 4844 1299 1369 3785 27878 147 34816
   PROBE pvs calls 984875 avg_n 24.24 avg_consumed 5.34 sort_cost 1.113e+08 sel_cost 7.766e+07 ratio 0.698 hist(<=1,2,3,4-5,6-10,11-20,>20 partial,all) hyb1 5.095e+07 hyb2 5.028e+07 hyb3 5.263e+07 hyb4 5.509e+07 716590 42404 8882 8752 16322 79102 397 112426

TOTAL
qs_capture: calls 7855804 avg_n 2.49 avg_consumed 1.67 sort_cost 3.699e+07 sel_cost 3.485e+07 ratio 0.942
   hybrid/sort ratio K=1..4: [0.835, 0.876, 0.904, 0.921]
   consumed hist (<=1,2,3,4-5,6-10,11-20,>20 partial,all): ['62.4%', '16.8%', '9.2%', '8.2%', '3.2%', '0.2%', '0.0%', '0.0%']
qs_evasion: calls 1302798 avg_n 35.54 avg_consumed 18.10 sort_cost 2.415e+08 sel_cost 4.738e+08 ratio 1.962
   hybrid/sort ratio K=1..4: [0.822, 0.859, 0.916, 0.98]
   consumed hist (<=1,2,3,4-5,6-10,11-20,>20 partial,all): ['33.7%', '6.0%', '2.8%', '2.8%', '4.0%', '6.3%', '0.4%', '44.0%']
pvs: calls 5569570 avg_n 31.93 avg_consumed 7.57 sort_cost 9.028e+08 sel_cost 8.209e+08 ratio 0.909
   hybrid/sort ratio K=1..4: [0.475, 0.475, 0.5, 0.527]
   consumed hist (<=1,2,3,4-5,6-10,11-20,>20 partial,all): ['70.5%', '4.1%', '1.0%', '1.0%', '1.6%', '4.5%', '0.1%', '17.0%']
```

</details>
