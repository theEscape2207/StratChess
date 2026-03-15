# Pre-UCI / Pre-Lazy SMP Priorities

**Date**: 2026-03-09
**Status**: Approved
**Goal**: Establish the ordered work queue between now and the two next major milestones: UCI protocol (for first public ELO testing) and Lazy SMP (parallel search).

---

## Strategic Context

- **UCI comes before Lazy SMP.** UCI lets us run the engine in Arena/Cutechess to measure ELO. That baseline is only meaningful if we've already landed the biggest single-threaded gains (especially LMR). We measure first, then parallelize.
- **MoveFormatter (ToUCI / FromUCI) is already done.** UCI has no hard code blockers — the remaining work is correctness confidence and time management.
- **Test/QA is interwoven, not a separate track.** Each test item is bundled with the feature work that triggers it, not deferred.

---

## Agreed Ordered Work Queue

### Tier 1 — Now (overdue / in-progress)

| # | Item | Type | Notes |
|---|------|------|-------|
| 1 | **`[board]` DoMove/UndoMove completeness tests** | QA | ✅ Already done — 6 test cases, 33 assertions, all passing. Docs were out of date. |
| 2 | **Migrate Move Scoring → MoveSorter + `[sort]` tests** | Refactor + QA | In-progress (`MoveOrdering.h/cpp` exists). Bundle: move the inline scoring out of `pvs()`, write [sort] tests as proof-of-correctness. Cleans `pvs()` before LMR adds new logic. |

### Tier 2 — Before UCI (strength & confidence)

| # | Item | Type | Notes |
|---|------|------|-------|
| 3 | **LMR + `[search]` tests** | Feature + QA | Bundle. LMR is the biggest single ELO gain (2-3x speedup). [search] tests (assess_iteration_quality, should_stop_early, handle_empty_move_emergency) belong in the same PR. `STRAT_ENABLE_TEST_ACCESS` macro can be set up ahead of time. |
| 4 | **Full tactical suite (WAC subset + mate-in-N)** | QA | 25 WAC positions + 10 mate-in-2. 90%+ pass = ready for public play. Failing positions identify remaining weaknesses. **Trigger moved to "before UCI"** — does not need to wait for King Safety / Mobility evaluation extension. |
| 5 | **Time Management improvements** | Feature | Required for UCI `go wtime btime` to work correctly in tournament mode. Without it, UCI is functional but not competitive. |
| 6 | **UCI protocol** | Feature | MoveFormatter prerequisite already met. |

### Tier 3 — Between UCI and Lazy SMP (structural safety net)

| # | Item | Type | Notes |
|---|------|------|-------|
| 7 | **De-Singleton Board** | Refactor | `Position.h/cpp` already created. [board] tests from Tier 1 serve as regression baseline. |
| 8 | **Extract ThreadData + C++23 bundle** | Refactor | `std::mdspan` for `history_[2][64][64]` is primary C++23 motivator. One PR. |
| 9 | **NPS performance regression baseline** | QA/Infra | Set up after LMR (solid single-threaded reference) and before Lazy SMP (need baseline to measure parallel speedup). |
| 10 | **Lazy SMP** | Feature | ThreadData extraction is the last blocker. |

---

## Three Corrections Applied to Existing Docs

These three items were mis-stated or mis-timed in `Roadmap.md` / `TestDesign.md` at the time of this decision:

1. **`[board]` tests are overdue** — TestDesign.md marks them ⏳ Phase 1 but the trigger condition (Move Phases 3&4) already landed. Status updated to reflect they should happen now.

2. **Full tactical suite trigger moved** — Roadmap.md gated it on "when evaluation is extended (King Safety, Mobility)." Moved to "before UCI" — we need it to assess engine readiness before showing to others, regardless of evaluation completeness.

3. **LMR + `[search]` tests are one PR** — Roadmap.md listed them separately. They belong bundled: LMR modifies `pvs()`; [search] tests validate the helper methods that govern when to accept/reject an LMR-modified iteration result.

---

## Items Explicitly Not Included Here

The following are valid roadmap items but do not belong in this queue:

- **SEE / Futility Pruning** (🟢 medium) — Measurable ELO gain, but small compared to LMR. Can land post-UCI as distinct ELO steps.
- **Bitboard tests** (⏳ opportunistic) — No specific trigger; add when touching bitboard code.
- **C++23 / Clang / CMake** — Downstream of ThreadData extraction.
- **NNUE, Tablebases, Opening Book** — Long-term; not on this horizon.

---

## Success Criteria

- Before UCI: full Catch2 suite passes, tactical suite passes 90%+, engine plays sensibly in `go wtime btime` mode
- Before Lazy SMP: De-Singleton complete, single-threaded NPS baseline recorded, all existing tests pass with non-singleton Board
