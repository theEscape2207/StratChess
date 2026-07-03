# Extract ThreadData Structure

Roadmap item: 🔴 Critical — step 2 of the Near-Term Sequence (`Docs/Roadmap.md`).
Prerequisite for Lazy SMP. Unblocked by De-Singleton Board (PR #67).

## Goal

Collect all per-search mutable state used by `AIPerplex` into a single `ThreadData` struct and
pass it explicitly through the search call chain, so that spawning a Lazy SMP helper thread later
becomes "construct another `ThreadData`, call the same functions". Zero behavioral change — this
is a deterministic refactor validated by perft equivalence and byte-identical self-play node
counts.

**Scope limits:**
- `AIPerplex` only. `AIAgent`/`ABIterative`/`AIBasic` (via `PlayerAiBase`/`PlayerAiIterBase`)
  keep the existing member-based state (`m_SearchCount`, `m_infoSeq`, `m_BestMove`, base
  `Quiescent`) — they will never run under Lazy SMP.
- No thread pool, no helper-thread loop — that is the Lazy SMP item itself.
- No C++23: `history` stays a raw `int32_t[2][64][64]`. The `std::mdspan` slice waits for the
  separate `stdcpplatest` bump commit (roadmap "Upgrade to C++23", validation-hygiene rule).
- **GetMove TimeControl refactor is explicitly NOT part of this PR** (user decision 2026-07-02).
  It lands afterwards as its own small PR, in the step-3 window before Lazy SMP. Rationale:
  ThreadData's validation gate is byte-identical node counts — any unrelated change makes drift
  unattributable (same reason the C++23 bump is excluded). The two refactors are independent by
  construction: the time-control plane deliberately stays out of ThreadData.

## Design Decisions

1. **Parameter-threaded, not member-aggregated.** `ThreadData&` is passed through
   `iterative_deepening` → `search_with_aspiration` → `pvs` → `quiescence`, absorbing the
   existing `PVTable&` parameter. Merely bundling members into a struct accessed implicitly
   would rename state without extracting it — the explicit parameter is the SMP-ready shape.
2. **`ThreadData` owns a `Board` copy, starting now.** The search runs on `td.board`
   (copy-assigned from `m_Board` at `GetMove()` entry), not on the game board. A faithful copy
   must produce byte-identical node counts, so this PR's strict validation regime is exactly
   where a `Board` copy bug (first real workout of PR #67 copyability) gets caught — not later
   amid nondeterministic thread races. Game-state side effects are propagated back explicitly
   (see Step 4).
3. **What stays OUT of ThreadData, and why:**
   - `TranspositionTable` — shared by design in Lazy SMP. Kept as an explicit `tt` parameter so
     every call site documents the shared-vs-local split.
   - `tuning_` — read-only configuration.
   - Time-control plane (`time_manager_`, `stop_search_`, `nodes_since_check_`,
     `clock_info_set_`, `time_limit_`) — control plane owned by `PlayerAiBase`; helper threads
     will read shared stop state. This is also the seam the later TimeControl refactor works on
     — keeping it out makes the two PRs non-overlapping by construction.
     *Lazy SMP follow-up (documented, not done here):* `nodes_since_check_` is a per-node hot
     counter and would race; each helper thread needs its own amortization counter. Likewise
     `_bestScore` and `last_result_` (written at root/end of search) and the timer/abort state
     must stay main-thread-only — flagged by search-reviewer at final review (LGTM otherwise).
   - Static `m_TotalTime`/`m_TotalCount` perf stats — known SMP hazard, game-mode-only;
     flagged for Lazy SMP, not touched here.
   - `tt_misses` debug multimap + `last_result_` — debug/result plumbing written outside the
     hot path; main-thread-only even under SMP. Left in place.
4. **`ThreadData` is a persistent member (`td_`), not a `GetMove()` local.** `history` is aged
   (`age_history`) but never cleared between moves — it must survive across `GetMove()` calls.
   Per-search state (`board`, `nodes_searched`, `info_seq`, `pv_table`, killers, null flags) is
   reset at `GetMove()` entry, matching today's reset points exactly.
5. **State-maintenance helpers move onto `ThreadData` as methods** (`clear_killers`,
   `store_killer`, `clear_null_move_flags`, `clear_history`, `age_history`, `update_history`,
   `store_info_at_ply`, `add_move_to_seq`, `add_null_move_to_seq`, `get_last_info`,
   `check_draws`). ThreadData is then self-contained: it holds the per-thread state and the
   operations that maintain it. The `PlayerAiBase` originals (`StoreInfoAtPly`, `AddMoveToSeq`,
   `AddNullMoveToSeq`, `checkDraws`, `GetLastBoardInfo`) remain untouched for the legacy AIs —
   AIPerplex simply stops calling them. (The roadmap's "Files Affected: PlayerAiIterBase.h" is
   stale — AIPerplex derives directly from `PlayerAiBase`; `PlayerAiIterBase` is not touched.)
6. **`StopTimerAndAdjustVars()` gains a node-count parameter** instead of reading
   `m_SearchCount`: `StopTimerAndAdjustVars(size_t node_count)`. Legacy callers pass
   `m_SearchCount`; AIPerplex passes `td_.nodes_searched`. Avoids dual-counter drift.

## Files Changed

| File | Change |
|---|---|
| `StratEngine/ThreadData.h` | **New** — struct + inline state-maintenance methods |
| `StratEngine/AIPerplex.h` | Remove migrated members/helpers; add `ThreadData td_`; new signatures |
| `StratEngine/AIPerplex.cpp` | Thread `td` through search; td-aware game-state/draw handling |
| `StratEngine/PlayerAI.h` | `StopTimerAndAdjustVars(size_t node_count)` |
| `StratEngine/PlayerAI.cpp` | Same, `m_TotalCount += node_count` |
| `StratEngine/AIBasic.cpp`, `AIAgent.cpp`, `ABIterative.cpp` | Pass `m_SearchCount` to `StopTimerAndAdjustVars` |
| `StratEngine/Archived/AITrans.cpp`, `ABIterTrans.cpp` | Same (only if compiled into the project — check vcxproj) |
| `StratEngine/StratEngine.vcxproj` + `.filters` | Add `ThreadData.h` (`ClInclude` + `Filter`) |
| `StratChessTests/SearchTests.cpp` | Fixture accessors updated (`last_move_was_null_` → `td_.last_move_was_null`, etc.) |
| `Docs/Roadmap.md` | Status update on completion |

## Step-by-Step Changes

### Step 1 — Introduce `ThreadData.h`; migrate move-ordering state (buildable checkpoint)

New `StratEngine/ThreadData.h` (includes: `Board.h`, `PVTable.h`, `Move.h`, `<vector>`,
`<cstdint>`; add to vcxproj + filters):

```cpp
struct ThreadData {
    static constexpr int MAX_KILLERS = 2;          // moves here from AIPerplex
    static constexpr int32_t HISTORY_MAX = 16'384; // moves here from AIPerplex

    Board board;                       // thread-local copy (wired in Step 3)
    int64_t nodes_searched = 0;        // replaces m_SearchCount inside AIPerplex
    PVTable pv_table;                  // absorbs GetMove()'s local (Step 2)
    std::vector<GameInfo> info_seq;    // replaces m_infoSeq inside AIPerplex (Step 3)
    int thread_id = 0;                 // future: helper-thread identification

    Move killers[MAX_PLY][MAX_KILLERS];
    bool last_move_was_null[MAX_PLY]{};
    int32_t history[2][64][64];

    // methods: clear_killers/store_killer/clear_null_move_flags/clear_history/
    // age_history/update_history — bodies moved verbatim from AIPerplex.cpp:718-765;
    // info-seq + draw helpers added in Step 3
};
```

- `ThreadData` requires `Board` default-constructible (it is — plain value type post-PR #67);
  `td_.board` is copy-assigned per search in Step 3.
- `AIPerplex` gains member `ThreadData td_;` (direct member — same footprint as today's inline
  arrays, ~32 KB history + Board); constructor calls `td_.clear_killers(); td_.clear_history();`.
- Delete `killers_`, `history_`, `last_move_was_null_`, `MAX_KILLERS`, `HISTORY_MAX` and the
  six helper methods from `AIPerplex`; mechanical rename at all use sites
  (`AIPerplex.cpp:136-143, 276?, 339-344, 365-368, 392, 735-765, 935`).
- `should_try_null_move()` reads `last_move_was_null` — for this step it can read `td_.` as a
  member; signature changes to take `const ThreadData&` in Step 2.
- Update `SearchTests.cpp` fixture accessors (`ai->last_move_was_null_[ply]` →
  `ai->td_.last_move_was_null[ply]`; keep the friend-based access pattern).
- **Build + `run-tests` green before proceeding.**

### Step 2 — Thread `ThreadData&` through the call chain (buildable checkpoint)

New signatures (replace the `PVTable& pv_table` parameter; keep `tt` explicit):

`ThreadData&` is always the **first** parameter wherever it is added:

```cpp
SearchResult iterative_deepening(ThreadData& td, int max_depth, TranspositionTable& tt);
int search_with_aspiration(ThreadData& td, int depth, int seed_score, TranspositionTable& tt);
int pvs(ThreadData& td, int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt);
int quiescence(ThreadData& td, int alpha, int beta, int depth_q, int ply, TranspositionTable& tt);
bool should_try_null_move(const ThreadData& td, int depth, int beta, int ply, bool is_pv_node, bool in_check) const;
bool handle_empty_move_emergency(ThreadData& td, SearchState& state);
int adjustScoreForGameState(ThreadData& td, bool moveFound, int ply, int best_value);
```

- `GetMove()` deletes the local `PVTable pv_table` and passes `td_`; log helpers keep taking
  `const PVTable&` — call sites pass `td.pv_table`.
- `m_SearchCount` uses inside AIPerplex (`GetMove:121`, `iterative_deepening:144,161`,
  `pvs:376`) → `td.nodes_searched`. AIPerplex stops relying on base
  `InitMoveVariables()` (which resets `m_SearchCount`/`m_infoSeq`/`m_BestMove`) — replaced by a
  private `init_search(const GameInfo& info)` that resets `td_` per-search state
  (`nodes_searched = 0`, `info_seq.clear() + emplace_back(info)`, `pv_table = PVTable{}` — check
  whether `PVTable` has a `clear()`; if not, copy-assign a fresh one).
- `StopTimerAndAdjustVars(size_t node_count)`: update `PlayerAI.h/cpp` + all live call sites
  (AIBasic, AIAgent, ABIterative pass `m_SearchCount`; AIPerplex passes
  `static_cast<size_t>(td_.nodes_searched)`; Archived files only if compiled).
- **Build + `run-tests` green before proceeding.**

### Step 3 — Board copy + info_seq migration (buildable checkpoint; the risky step)

- `init_search()` adds `td_.board = m_Board;` (copy-assign). All board access inside the search
  call chain switches `m_Board` → `td.board` (`AIPerplex.cpp:295, 332, 340-343, 353, 363-365,
  378, 435, 502-505, 540-545, 571, 586-588, 595-606, 612, 942-943, 963-1005`).
- `info_seq`: `GetLastBoardInfo(ply)` calls (`pvs:284`, `quiescence:568`) →
  `td.get_last_info(ply)`; `AddMoveToSeq`/`AddNullMoveToSeq` (`341, 381, 609`) →
  `td.add_move_to_seq(move, ply)` / `td.add_null_move_to_seq(ply)`. Method bodies move verbatim
  from `PlayerAI.cpp:197-247` (they use `td.board.GetPiece(move.to())` /
  `td.board.GetGameInfo()`); preserve the null-move `lastMove` caveat comment
  (`PlayerAI.cpp:238-243`).
- `checkDraws(info, ply)` (`pvs:287`) → `td.check_draws(info, ply)` (body from `PlayerAI.h:199`,
  using `td.board.is_repetition`).
- `UpdateGameState(ply, state)` calls (`adjustScoreForGameState`, `AIPerplex.cpp:505-518`) → a
  td-aware equivalent: same conditional logic (`ply == 0`, only on change) but targeting
  `td.board.SetGameState()` + `td.info_seq[0].gameState`. Base method untouched.
- **Game-state propagation back to the real board** (`GetMove()`, after the search returns):
  mirror the same only-if-changed conditional — if `td_.info_seq[0].gameState !=
  m_Board.GetGameInfo().gameState`, call `m_Board.SetGameState(...)`. **Implementer: read
  `Board::SetGameState` first** and confirm it is a plain state write with no other side
  effects; the propagation must reproduce exactly what ply-0 `UpdateGameState` did on `m_Board`
  today, including on the empty-move/game-over path (`GetMove:102-111`).
- `CheckGameOver(info, false)` (`GetMove:125`) reads base `m_infoSeq[0]`, which AIPerplex no
  longer populates → replace with inline equivalent reading `td_.info_seq.at(0)` and firing
  `EGameStateChanged` on non-`STILL_PLAYING`. Base method untouched.
- `GetMove()`'s remaining `m_Board` reads (`info = m_Board.GetGameInfo()` at 103/124,
  `GetEffectiveMovPiece` at 126) stay on `m_Board` — correct because search do/undo nets to
  zero and game state has just been propagated back.
- **Build + full `run-tests` + `[tactical_full]` green before proceeding.**

### Step 4 — Validation + docs

- Full validation per plan below; then update `Docs/Roadmap.md` (item → done, Near-Term
  Sequence step 2 checked off) and `CLAUDE.md` Key Source Files (mention `ThreadData.h`).

## Validation Plan

The PR #67 way — deterministic refactor, zero drift expected; any drift is a bug in the
refactor or in `Board` copy semantics:

1. **Baseline capture (BEFORE any change)**: build current `master` Release; run a fixed-depth
   self-play (`game_settings.json`: both players `"type": 6`, depth low enough that the time
   budget never binds — depth 5 with the default 15 s limit is safe) from `StratChessEvolved/`;
   save all `GetMove complete: ... nodes=...` lines as the reference node-count sequence.
   Fixed-depth is what makes node counts deterministic — do NOT use a time-bound config.
2. **After each buildable checkpoint**: `.\build.ps1 run-tests` (fast tier).
3. **After Step 3**: rebuild main exe; rerun the identical self-play config; diff the
   `nodes=`/`depth=`/`move=` sequence against the baseline — must be byte-identical.
4. **Perft**: `[perft]` fast tier + deep perft (`cd Tests && ../x64/Release/StratChessEvolved.exe
   perft test`).
5. **Extended tiers + exe tactical suite**: `Scripts\Validate-PrePR.ps1` (code change → full
   gate applies).
6. **Reviewer**: dispatch `search-reviewer` (pvs/qsearch touched) before opening the PR.
7. Verify `game_settings.json` is restored to the starting position before committing.

## Key Correctness Properties

1. **Byte-identical node counts**: fixed-depth self-play produces the identical
   move/depth/nodes sequence before and after — the single strongest invariant; subsumes "no
   ELO regression" for this change.
2. **Search purity on the copy**: after `GetMove()` returns, `m_Board` differs from its
   pre-call state only in game state explicitly propagated back (checkmate/stalemate/draw
   detection at ply 0) — never in position, hash, or history.
3. **History persistence**: `td_.history` survives across `GetMove()` calls (aged per depth
   iteration, cleared only at construction) — identical lifecycle to today's `history_`.
4. **Per-search reset equivalence**: killers, null-move flags, `info_seq`, `pv_table`,
   `nodes_searched` are reset at exactly the same points as today (search start), no more, no
   less.
5. **Legacy AIs untouched behaviorally**: `AIBasic`/`AIAgent`/`ABIterative` still compile and
   pass self-play (`"type": 3` spot check); the only diff they see is the
   `StopTimerAndAdjustVars` parameter carrying the same value as before.
6. **Perf-stats continuity**: `SimplePerfStats.txt` columns carry the same node counts as
   before (parameter, not member, now feeds them).
7. **No new warnings**: Level4 + `/WX` clean on both projects.
