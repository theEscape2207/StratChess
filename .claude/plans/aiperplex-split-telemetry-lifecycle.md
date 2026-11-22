# AIPerplex file split, slice 1: telemetry and lifecycle — Design

**Issue:** #990

## Goal

`AIPerplex.cpp` is 2,399 lines, and most changes only edit node search. Slice 1 of #990 (option A)
moves the telemetry and lifecycle member definitions into `AIPerplexTelemetry.cpp` and
`AIPerplexLifecycle.cpp`. The class, the header and the search stay the same. The triage asked for a
design document if an anonymous-namespace helper turned out to be shared across seams, and one is:
`s_logger`.

## Review focus

- D1: how `s_logger` is shared across three translation units without editing any body that uses it.

## Scope

**This change will:**

- Move the constructor, destructor, launch/stop handshake, `StartAsync`/`StopAndWait`/`Wait`,
  `SetHash`, `SetTuning`, `StartNewGame`, `init_search`, `reset_prefetch_targets` and
  `helper_loop` (current lines 133-340) into `AIPerplexLifecycle.cpp`. This is the triage's
  ~190-line lifecycle seam.
- Move the seven `log_*` members and `emit_iteration_info` into `AIPerplexTelemetry.cpp`.
- Add `StratEngine/AIPerplexLogger.h` (D1).
- Update the `AIPerplex.cpp` references in `Docs/Architecture.md` and #889's row.

**This change will not:**

- Change `AIPerplex.h`, the test fixture, or any moved body.
- Move the root driver. That is slice 2.
- Change how the logger is initialised, or its thread-safety contract.

## Decisions

### D1: share `s_logger` through a small internal header

`s_logger` is used by the constructor (lifecycle), by `Search()` and `iterative_deepening()` (they
stay in `AIPerplex.cpp`), and by every telemetry function. Only the constructor calls
`ensure_logger_initialized()`.

**Chosen:** `AIPerplexLogger.h` declares
`namespace AIPerplexLog { extern std::shared_ptr<spdlog::logger> s_logger; void ensure_logger_initialized(); }`.
Both are defined in `AIPerplexTelemetry.cpp`, with the existing thread-safety comment, and its
reference to `search_with_aspiration()` "below" is corrected. Each of the three `.cpp` files adds
`using AIPerplexLog::s_logger;`, and `AIPerplexLifecycle.cpp` also adds
`using AIPerplexLog::ensure_logger_initialized;`. Every body keeps its text, so the line-multiset
check still applies. A namespace-scope `extern` has precedent in `Board.h:294` (`Zobrist` keys).

**Rejected:**

- A private static member of `AIPerplex`. This would edit the header, put an spdlog type in front
  of every includer, and break "no header change".
- An accessor function, such as `aiperplex_logger()`. Every call site would change, about 20 lines
  across moved and staying bodies, which defeats the pure-move check.
- Moving the constructor into the telemetry file. That files it under the wrong seam, and
  `Search()` still needs the logger.

**Cost of the choice:** `s_logger` changes from internal to external linkage. Every use sits behind
`verbose_logging_`, off the node path, and `pvs`/`quiescence` never read it.

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
  The only additions are includes, `using` lines, the logger header and the file preambles.
- `AIPerplex.h` is unchanged.

## Validation

- Line-multiset diff: lines deleted from `AIPerplex.cpp` minus lines added to the new files is
  empty, apart from the edits D1 lists.
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
| D1 logger sharing and its thread-safety contract | comment in `AIPerplexLogger.h` / `AIPerplexTelemetry.cpp` |
| File-to-seam map | `Docs/Architecture.md` navigation |
| Bench and equivalence result | PR body |
