# AIPerplex file split, slice 1: telemetry and lifecycle — Design

**Issue:** #990

## Goal

`AIPerplex.cpp` is 2,399 lines, and most changes only edit node search. Slice 1 of #990 (option A)
moves the telemetry and lifecycle member definitions into `AIPerplexTelemetry.cpp` and
`AIPerplexLifecycle.cpp`. The class, the header and the search stay the same. The triage asked for a
design document if an anonymous-namespace helper turned out to be shared across seams, and one is:
`s_logger`.

## Review focus

- D1: routing the three non-telemetry logger uses through telemetry members.

## Scope

**This change will:**

- Move the constructor, destructor, launch/stop handshake, `StartAsync`/`StopAndWait`/`Wait`,
  `SetHash`, `SetTuning`, `StartNewGame`, `init_search`, `reset_prefetch_targets` and
  `helper_loop` (current lines 133-340) into `AIPerplexLifecycle.cpp`. This is the triage's
  ~190-line lifecycle seam.
- Move the seven `log_*` members and `emit_iteration_info` into `AIPerplexTelemetry.cpp`.
- Add three private logging members to `AIPerplex.h` (D1).
- Update the `AIPerplex.cpp` references in `Docs/Architecture.md` and #889's row.

**This change will not:**

- Change the public interface, the test fixture, or any moved body beyond D1's three call sites.
- Move the root driver. That is slice 2.
- Change how the logger is initialised, or its thread-safety contract.

## Decisions

### D1: the telemetry file owns the logger

`s_logger` was used by the constructor (lifecycle), by `Search()` and `iterative_deepening()`
(they stay in `AIPerplex.cpp`), and by every telemetry function.

**Chosen (owner, 2026-11-22: prefer the cleaner option over a smaller diff):** `s_logger` and
`ensure_logger_initialized()` stay in an anonymous namespace, now in `AIPerplexTelemetry.cpp`. The
three outside uses go through new private members declared with the other `log_*` helpers:

- `init_logger()` carries the constructor's best-effort setup block.
- `log_search_result(const SearchResult&)` carries `Search()`'s "GetMove complete" line. It reads
  `result.nodes_searched`, which equals `total_nodes` at that point.
- `log_mate_stop(int depth)` carries `iterative_deepening()`'s "Mate found" line.

All logging then lives in one file, and no other file can reach the logger.

**Rejected:**

- An `extern` in a shared internal header. The diff is smaller, but it adds a global with external
  linkage and keeps raw logger calls spread across three files.
- A private static member. It would put an spdlog type in `AIPerplex.h`.

**Cost:** two cold call sites now make a member call. One runs once per search, the other once per
mate stop. `pvs` and `quiescence` are untouched.

### D2: the other anonymous-namespace helpers stay in `AIPerplex.cpp`

`ScopeExit` is used only by `Search()`, `pawn_push_to_seventh` only by `pvs`, and
`MATERIAL_PRUNING_MIN_PIECES` only by `quiescence`, so no moved body needs them. Moved functions
keep their source order and their preceding comments.

## Assumptions I cannot verify from the code

- LTO keeps cross-TU inlining, so the moved definitions inline as they did before
  (`CMakeLists.txt` LTO block, per the triage). Verified by `Compare-Bench`.

## Invariants

- Search behaviour is identical: same nodes, scores and best moves.
- The lines removed from `AIPerplex.cpp` equal, as a multiset, the moved lines in the new files.
  The only other changes are includes, file preambles and D1's three call sites.
- `AIPerplex.h` gains only D1's three private declarations.

## Validation

- Line-multiset diff: lines deleted from `AIPerplex.cpp` minus lines added to the new files is
  empty, apart from D1's call sites and the thread-safety comment's two cross-references.
- `Compare-SearchEquivalence.ps1 -After <exe>` against the merge base: IDENTICAL.
- `Compare-Bench.ps1`: no slowdown. On Unresolved, rerun on a `New-OrderedBuildPair.ps1` pair with
  `-Control -Rounds 60`.
- `Validate-PrePR.ps1` and CI (Linux Debug + sanitizers, Windows clang-cl).
- No Elo match: the search is identical by construction, and the equivalence gate proves it.

## Cost

- **Size:** about 350 lines moved, about 30 added (preambles, includes, the header). Four source files
  and two docs.
- **Blast radius:** engine tier, with no behaviour change.
- **Review:** one standard code review.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1: the telemetry file owns the logger | header comment above the logging helpers in `AIPerplex.h` |
| File-to-seam map | `Docs/Architecture.md` navigation |
| Bench and equivalence result | PR body |
