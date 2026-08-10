# cmd_ucinewgame uses StartNewGame() instead of a full AI rebuild — Design

**Issue:** #266

## Goal

`UciHandler::cmd_ucinewgame()` currently destroys and reconstructs the entire `AIPerplex` (and its
192 MiB TT) on every `ucinewgame`, ~37 ms of dead time per game and, more importantly, a design tax:
every client-configurable option (today just `Threads`) needs a `configured_*` shadow copy on
`UciHandler` solely to survive the rebuild it would otherwise be silently reset by. `StartNewGame()`
already exists (#259/PR #263) but is currently a no-op in practice, since `init_ai()` always hands
back an object whose state is already fresh. Make `StartNewGame()` do the actual reset and stop
rebuilding.

## Scope

**This change will:**

- Replace the `init_ai()` rebuild in `cmd_ucinewgame()` with a persisting `ai_` plus a real
  `AIPerplex::StartNewGame()` that resets every piece of per-game state.
- Add a `ThreadData::reset_for_new_game()` used by `StartNewGame()`.
- Add tests for what must and must not survive `ucinewgame`.

**This change will not:**

- Touch `pvs()`, `quiescence()`, move ordering, or any other search decision — this is a lifecycle
  change only. If a fixed-depth search result moves, that is a bug, not an intended effect.
- Add a UCI `Hash` option or otherwise touch #254 — the shadow-copy tax that issue will no longer need
  is a consequence of this change, not something implemented here.
- Run an Elo match — nothing here changes search decisions (see Validation).

## Decisions

### D1: What `StartNewGame()` resets

Derived from the class definitions, not by re-deriving intent from `init_ai()`'s effects:

| State | Reset? | Why |
|---|---|---|
| `_tt` | Yes, `clear()` | Already did this; only line that was previously load-bearing. |
| `td_` (killers, history, null-move flags, PV, `info_seq`, node counters, board copy) | Yes | History/killers are deliberately aged **within** a game (class comment on `ThreadData`) but must not leak **across** games. |
| `helper_tds_` | Yes, `.clear()` the vector | Lazily resized in `GetMove()` (`if (helper_tds_.size() < threads - 1)`), so clearing it forces fresh `ThreadData` construction (with correct `thread_id`) on the next search — exactly mirrors what the old rebuild did, and frees memory sized for a prior larger `-Threads`. |
| `threads_` | **No** | It's no longer discarded by a rebuild, so there is nothing to restore — the shadow-copy tax this issue exists to remove. `configured_threads_` on `UciHandler` still applies once, lazily, when `ai_` is first constructed (client can `setoption` before the first `ucinewgame`); it is never re-applied afterward. |
| Evaluator | **No** | `EvalManager`/`EvalComplex` are documented stateless and thread-shared (`Eval.h`, Lazy-SMP sharing contract comment) — recreating it changes nothing observable. |
| `tuning_` | **No** | Caller configuration, same reasoning as `threads_` — not accumulated search state. **Changed during implementation**: the first draft reset it to defaults, reasoning it wasn't reachable via UCI; `search-reviewer` caught that `Game::SetPlayerParams()` (`Game.cpp`) applies `game_settings.json`'s `search_tuning` overrides and then unconditionally calls `StartNewGame()` on the same object, so resetting `tuning_` there would have silently discarded every configured override before the first move. |
| `last_result_` | Yes, `= SearchResult{}` | Otherwise `GetLastResult()` could return the previous game's result before the first search of the new one. |
| `max_depth_` / `time_limit_` | **No new code** | Already unconditionally reset by `stop_and_join()`, called immediately before `StartNewGame()` in `cmd_ucinewgame()` — untouched by this change. |

### D2: `ai_` construction becomes lazy-once instead of per-`ucinewgame`

`UciHandler::run()` already calls `init_ai()` once before the command loop, so in the real UCI flow
`ai_` always exists by the first `ucinewgame`. Test fixtures call `cmd_ucinewgame()` directly without
`run()`, so `ai_` can still be null there. `cmd_ucinewgame()` becomes
`if (!ai_) init_ai(); if (ai_) ai_->StartNewGame();` — one real construction per process,
`StartNewGame()` for every game after that (and the first, harmlessly, since a freshly-constructed
object is already clean).

## Assumptions I cannot verify from the code

None — `EvalManager`'s statelessness and `ThreadData`'s per-game-vs-per-move fields are both
documented in-repo (see D1), and `helper_tds_`'s lazy-resize contract is read directly out of
`AIPerplex::GetMove()`.

## Invariants

- Fixed-depth search at `Threads=1`, issued after `ucinewgame`, is byte-identical (same node count,
  same best move) before and after this change — including a *second* game after moves were played
  in a first one, which is the only case that can actually differ (a single fresh game is trivially
  equivalent under either implementation).
- A client-set `Threads` value survives `ucinewgame` (existing tests, unchanged contract).
- A TT entry does not survive `ucinewgame` (existing `[search][tt]` test, now exercised through the
  real `UciHandler` path too, not only direct `AIPerplex` construction).

## Validation

Engine tier (touches `AIPerplex.cpp`/`.h`, `ThreadData.h`, `UCIHandler.cpp`/`.h`) — full build,
extended `[slow]` tests, tactical suite, self-play via `Validate-PrePR.ps1`.

Plus the specific gate this issue calls for: build the pre-change and post-change `StratChessEvolved.exe`
and diff UCI output for `position startpos moves <N plies>` → `ucinewgame` → `position startpos` →
`go depth 6` at `Threads=1`, across both builds. No Elo match — this is a lifecycle change, not a
search-decision change; a moved fixed-depth result would be a bug caught by the equivalence check
above, not a strength question.

`search-reviewer` dispatch required (touches `AIPerplex.cpp`, `ThreadData.h`) per the PR checklist.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 reset table | Source comment on `AIPerplex::StartNewGame()` |
| `helper_tds_.clear()` relies on `GetMove()`'s lazy-resize contract | Source comment at the `.clear()` call |
| Fixed-depth before/after equivalence result | PR body |
| `ai_` lazy-once construction, `configured_threads_`'s narrowed role | Source comment on `UciHandler::init_ai()` |
| `tuning_` is caller configuration, not reset (the `Game.cpp` interaction that makes resetting it wrong) | Source comment on `AIPerplex::StartNewGame()` |
