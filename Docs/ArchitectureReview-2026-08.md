# StratChess Architectural and Design Review

**Repository:** `theEscape2207/StratChess`  
**Reviewed snapshot:** `eb8fcb917bee08c91118e1ee2ddb3188e3b5a4c2`  
**Review date:** 2026-08-09

> **Point-in-time document.** This records an assessment made on the date above. It is not
> maintained against the tree and will drift — read it as evidence of what was true at that commit,
> not as a current description. Do not update it in place; supersede it with a new dated review.

## Status and project context

This review was produced by an agent working **without project background** — no access to the
issue history or to decisions recorded in `Docs/`. Its observations of the code hold up: every
factual claim spot-checked during triage was accurate, including the P3 documentation drift, the
`UciHandler` downcasts and the `PlayerAiBase` legacy members. What it lacks is the record of what
has already been decided and why.

Read the following alongside it.

### The findings have been triaged into issues

| Review finding | Issue | Outcome of triage |
|---|---|---|
| P1 — UCI output serialization | **#249** | Accepted. Narrowed to a mutex in `send()` plus one stress test; batched with #247/#243. Two of the review's proposed test criteria were dropped as unable to observe the defect |
| P1 — TT synchronization | **#250** | Accepted as **measurement-first**. The rewrite is deferred until measurement justifies it; triggers for revisiting are named in the issue |
| P1 — no reusable engine-core target | **#251** | **Substantially revised** — see below |
| P2 — main executable has too many roles | **#251** | Folded in; the separate-binaries part is deferred with a named trigger |
| P3 — documentation drift | **#253** | Accepted and widened; the omissions turned out to matter more than the broken references |

Also opened from this review: **#254** (no UCI `Hash` option) and **#252** (the TT allocates 25%
less than requested), neither of which the review identified.

### Where the review should not be followed as written

**The engine-library recommendation contradicts a decided question.** P1 recommends "a single
`strat_engine` static library is an appropriate first step." That is precisely what **#83**
proposed and it was closed *not planned after measurement*: the saving was ~25-30 s of PR feedback,
against `STRAT_ENABLE_TEST_ACCESS` and LTO risks that `CMakeLists.txt` itself estimates at 5-15%
nps. The review does not cite #83 and its "without changing runtime behavior" claim is what that
analysis disputes. The roadmap also places this — the highest-risk item in the document — in the
near-term bucket, ahead of much safer work.

**The four-library structure is over-engineered for this project.** `strat_position` /
`strat_search` / `strat_uci` / `strat_tools` presumes multiple consumers or independent release
cycles; there is one production consumer. The `strat_position` ↔ `strat_search` boundary is also
the hottest in the program (move generation and evaluation called from `pvs()`): either LTO crosses
it, making the boundary notional, or it does not, and the split costs nps for a diagram. #251
now expresses **ownership** without introducing compiled artifacts, and defers the artifacts until
something needs them.

**Two of the review's factual premises about the tool sources do not hold.** `StratEngine/Tests/`
is not glob residue in the shipping binary: `Perft.cpp` backs the UCI `go perft` command that
`Run-PerftCheck.ps1` drives across the 142,953-position corpus, and `TacticalTestRunner.cpp` backs
the `tactical` CLI mode that `Validate-PrePR.ps1` runs as step 3 of the required gate. Both are
deliberate dependencies. The *ownership* argument survives; the "accidentally included" framing
does not.

### Corrections and additions to the P2 findings

**The proposed `SearchEngine` interface drops `GameInfo`, which is load-bearing.** The current
`GetMove(GameInfo&, const SearchLimits&)` propagates root state back through that reference, and
`AIPerplex::GetMove()` fires `EGameStateChanged` from `info.gameState`. Game mode depends on it.
Any replacement interface must account for it.

**The interface costs nothing on the hot path,** which the review does not say and should: it is
one virtual call per move, not per node.

**P2's motivation is weaker than P2's case.** "Introduce a concrete-independent interface" reads as
speculative generality with a single implementation. The honest and stronger motivation is the
second half of the same finding — stop production search inheriting a hierarchy built for retired
algorithms. Supporting evidence the review missed: `PlayerAiBase` carries process-global mutable
statics (`m_TotalTime`, `m_TotalCount`, `PlayerAI.h:248-249`), mutated non-atomically in
`StopTimerAndAdjustVars()`. Not a demonstrated race — searches are serialized and helpers never
call it — but it is exactly the baggage the migration exists to shed.

### The strategic gap

Nine roadmap items, none of which improves playing strength. That is defensible for a document
scoped as an architecture review, but it should not be adopted as *the* roadmap. `CLAUDE.md` states
the goal as **measured positive Elo, not nps**; the project's strongest asset is a working SPRT
strength lab; and the eval-improvement epic (**#110**, 12 sub-issues) goes unmentioned.

Architecturally this engine is in good shape. What it lacks relative to its stated goal is search
and evaluation *features*, not structure — and those are where the remaining Elo is.

---

## Executive summary

StratChess is technically mature for an independently developed chess engine. It already has explicit board instances, per-thread search state, a shared transposition table, per-call search limits, extensive Catch2 coverage, sanitizer and TSan jobs, deep perft, tactical-stability testing, UCI race probes, and an automated strength lab.

The principal architectural constraint is no longer a lack of engine functionality. Application concerns, UCI protocol handling, developer tools, legacy player abstractions, and the production search remain bundled into one broad layer. That makes concurrency boundaries harder to enforce, causes implementation-specific downcasts, and prevents the build system from expressing a reusable engine-core boundary.

The recommended direction is evolutionary rather than a rewrite:

1. Serialize all UCI output through one boundary.
2. Create a reusable engine library target.
3. Introduce a concrete-independent search interface.
4. Move production search away from the legacy player inheritance hierarchy.
5. Revisit transposition-table concurrency as a measured performance project.
6. Split the multipurpose entry point and refresh architecture documentation.

## Current architecture

```mermaid
flowchart TD
    Main["Multipurpose executable"] --> UCI["UciHandler"]
    Main --> Game["Game mode"]
    Main --> Tools["Perft / tactical / eval tools"]

    UCI --> Player["PlayerBase factory"]
    Game --> Player
    Player --> Perplex["AIPerplex"]
    Player --> Legacy["Legacy search implementations"]

    Perplex --> TD["ThreadData per worker"]
    Perplex --> TT["Shared transposition table"]
    TD --> Board["Board / position"]
    TD --> Eval["Evaluation"]
    TD --> MoveGen["Move generation"]
```

The desired dependency direction is:

```mermaid
flowchart LR
    Apps["UCI / game / tools"] --> API["Engine API"]
    API --> Search["Search service"]
    Search --> Position["Position + history"]
    Search --> Eval["Evaluator"]
    Search --> TT["TT service"]
```

## Priority findings

### P1 — UCI output lacks a single serialization point

The command loop and search thread can both write to `std::cout`. `UciHandler::send()` performs a message insertion, a newline insertion, and a flush as separate operations. The command thread can emit `readyok`, evaluation output, and refusal diagnostics while the search thread emits `info` and `bestmove`.

The standard library prevents internal corruption of the stream object, but it does not make a multi-operation logical line atomic. Two threads can therefore interleave text or separate a message from its newline. A malformed UCI line can be ignored or misparsed by a GUI and is difficult to reproduce because it depends on scheduling.

Introduce a single output abstraction, such as `UciOutput::send_line(std::string_view)`, and route every protocol line through it. The lowest-risk implementation is a mutex covering construction and emission of one complete line. `std::osyncstream` is also viable in C++20. A dedicated output thread is unnecessary at the engine's current output volume.

Tests should cover:

- `isready` and diagnostic output while a search is active.
- Concurrent complete-line emission under repeated scheduling pressure.
- Exactly one newline per message and no merged or fragmented messages.
- Existing `bestmove` ordering and UCI race probes.

Expected NPS impact is effectively zero because serialization occurs only at the protocol boundary, not in `pvs()`, quiescence, move generation, evaluation, or TT access. Expected intrinsic Elo impact is also neutral. It can nevertheless prevent rare protocol-corruption losses, which is an operational correctness improvement rather than a search-strength gain.

### P1 — Transposition-table synchronization is on the search hot path

Every TT probe takes a `std::shared_mutex` shared lock, every store takes its exclusive side, and the table allocates one mutex per bucket. This creates synchronization and memory overhead proportional to table size, reduces cache density, and can limit Lazy SMP scaling.

The eventual design should use compact, cache-aligned entries with atomic publication or another concurrency scheme that remains valid under the C++ memory model. This must be treated as a measured performance project rather than a cosmetic refactor. Validation should include NPS, scaling by thread count, TSan, tactical stability, and Elo.

### P1 — The build has no reusable engine-core target

CMake recursively gathers `StratEngine/*.cpp`, excludes only `Archived`, and compiles that collection directly into both the engine and test executables. The production executable therefore includes `StratEngine/Tests/Perft.cpp` and `TacticalTestRunner.cpp`, while engine sources are compiled twice.

Recommended target structure:

- `strat_position`: board, moves, move generation, FEN, hashing.
- `strat_search`: evaluation, ordering, TT, time management, AIPerplex.
- `strat_uci`: protocol adapter.
- `strat_tools`: perft, tactical, corpus and diagnostic commands.
- Separate production and test executables.

A single `strat_engine` static library is an appropriate first step. More granular libraries should follow only when their boundaries are useful and measurable.

### P2 — UciHandler depends on concrete implementations

`UciHandler` downcasts to `PlayerAiBase`, `AIPerplex`, and `EvalComplex` for configuration, search results, and evaluation breakdowns. `Game` contains similar implementation-specific branching. These downcasts show that the abstractions do not expose the capabilities their consumers actually need.

Introduce a production-oriented interface similar to:

```cpp
class SearchEngine {
public:
    virtual ~SearchEngine() = default;
    virtual SearchResult search(const Board&, const SearchLimits&) = 0;
    virtual void stop() noexcept = 0;
    virtual void set_threads(unsigned) = 0;
};
```

Evaluation breakdown can be an explicit evaluator capability. UCI should translate protocol commands into engine calls without knowing which concrete search or evaluator implements them.

### P2 — The player hierarchy contains two generations of search state

`PlayerAiBase` retains legacy members for the board, move sequence, best move, node counts, evaluation, timing, events, and game-state propagation. `AIPerplex` separately maintains `ThreadData`, helper states, its shared TT, tuning, logging, and `SearchResult`.

The `ThreadData` extraction is a strong improvement, but production search remains constrained by a base class designed for older algorithms. The migration path is:

1. Introduce the independent search interface.
2. Adapt `AIPerplex` to that interface.
3. Make game-mode AI players wrap the search interface.
4. Move nostalgic algorithms behind a legacy or tooling target.

### P2 — The main executable has too many roles

The entry point contains UCI, game mode, perft, tactical stability, batch evaluation, FEN integration testing, and a retired unit-test command. This mixes production protocol behavior with developer tooling and makes command-specific output contracts harder to isolate.

Extract each mode into a focused runner. Longer term, separate the UCI engine from developer tools if the additional binaries do not complicate distribution.

### P3 — Architecture documentation has drifted

`Docs/Engine-Readme.md` contains a quick-start call to a nonexistent single-argument `GetMove(info)` and lists `globals.h` and `typedef.h`, neither of which exists. The algorithmic documentation remains valuable, but inventories and examples should be kept executable or generated where practical.

## Architectural strengths

- `Board` is explicit, copyable state rather than a singleton.
- Make/unmake history, hashing, material, repetition, and side-to-move invariants are encapsulated together.
- `ThreadData` gives each Lazy SMP worker its own board, PV, history, killers, and counters.
- The TT is passed explicitly to recursive search functions as shared state.
- `SearchLimits` is a per-call value object rather than mutable pre-search configuration.
- Search and evaluation behavior have extensive focused tests.
- Validation includes fast and slow Catch2 tiers, sanitizers, TSan, deep perft, tactical stability, race probes, benchmarks, and differential Elo measurement.
- Comments record subtle concurrency and board-state invariants, including the `bestmove` ordering failure fixed by PR #246.
- FEN and other external inputs are treated as architectural boundaries rather than trusted internal data.

## Recommended roadmap

### Near term

1. Add atomic UCI line emission and concurrency-focused regression tests.
2. Refresh `Docs/Engine-Readme.md` examples and file inventory.
3. Extract a single reusable `strat_engine` library without changing runtime behavior.

### Medium term

4. Define the production search interface and migrate UCI to it.
5. Split tool runners from the main translation unit.
6. Adapt game-mode players around the production search interface.

### Performance research

7. Establish single-thread and multi-thread TT baselines.
8. Prototype a compact concurrent TT representation.
9. Require NPS, scaling, TSan, tactical-stability, and Elo evidence before adoption.

## Review scope

This was a static architectural and design review of the repository at the commit recorded above. It did not change engine behavior and did not establish a fresh build or test result.

