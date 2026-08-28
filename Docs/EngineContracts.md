# Engine Contracts

Non-obvious API contracts in `StratEngine/`. The rest of the layout is discoverable by reading it;
what is here is what reading the signature does **not** tell you.

Read the relevant section before editing that area. `CLAUDE.md` carries only the three tripwires
whose violation is silent.

| Editing… | Read |
|---|---|
| move encoding, comparison or formatting | [Moves](#moves) |
| `Board`, make/unmake, FEN, move generation | [Board and position state](#board-and-position-state) |
| `AIPerplex`, `SearchPlayer`, UCI, time management | [The search service](#the-search-service) |
| `pvs()`, `quiescence()`, `Sort.cpp`, pruning | [Search internals](#search-internals) |
| `game_settings.json` or its consumers | [Configuration](#configuration) |

---

## Moves

- `Move` is a pure 2-byte value (from/to/flags). The moving and captured pieces are **not** stored:
  use `Board::GetEffectiveMovPiece(m)` (pre-move only) and `Board::GetCapturedPiece(m)`. After
  `DoMove`, identify the moved piece with `board.GetPiece(m.to())`.
- **`Move` equality is exact** — it compares the raw 2-byte encoding, flags included. Two moves
  differing only in promotion piece, or a quiet move vs. a capture on the same squares, compare
  unequal.
- Move formatting lives entirely in `MoveFormatter`: `ToCoord` (coordinate-only, no board),
  `ToShort` (piece-prefixed; the `Board` overload appends `+` and reads the board, so never call it
  after a failed or unpaired `DoMove`), `ToUCI`, `ToVerbose`, `FromUCI`.
- Most `MoveHelper` predicates (`IsCapture`, `IsPromote`, `Value`, …) take a `const Move&`;
  `IsPawnMove` is the exception, taking a bare `ePiece`.

## Board and position state

- **`Board` is the sole authority for position metadata**: `ep_square()`, `castling_rights()`,
  `halfmove_clock()`, `fullmove_count()`, `last_move()`. Move generation reads them from the board it
  is given, so nothing can hand it state that disagrees with the position's Zobrist hash. One private
  `PositionState` per ply holds all of it plus the Zobrist hash, the last-irreversible ply and the
  captured piece.
- Sliding-piece attacks use PEXT magic bitboards (`StratEngine/Magic.h`).

## The search service

- `AIPerplex` is a standalone concrete search service: `Search(Board, limits, observer)` receives a
  root and observer per call and returns a `SearchResult` by value. It owns evaluator, TT, tuning and
  composed `SearchControl`; it is not an `IPlayer`, retains no caller Board or Board reference and
  no result cache, and has no compatibility player metadata. Each call copies its supplied root into
  owned `ThreadData` before search. `SearchPlayer { Board&, AIPerplex value }` is the required
  Game adapter; `CreatePlayer` maps config before type erasure. `ISearchEngine` is deliberately
  deferred until a second real implementation needs it.
- `ThreadData&` is the **first parameter of every search method**. The search runs on `td.board`,
  never the game board, and writes nothing back to it: the root verdict leaves via
  `SearchResult::game_state` and no other channel. The TT is a separate shared parameter — Lazy SMP
  helpers each get their own `ThreadData`.
- `SearchResult` carries best move, score, elapsed time, split node counts and the `GameStates` the
  player adjudicated at its own root. It is never `DRAW_50_MOVES`: the fifty-move rule is a fact about
  the committed position, and `Game::Run` adjudicates it. The returned value is the **post-join
  aggregate** and remains the authoritative record after later searches; Game owns combined totals.
- `SearchLimits` carries every per-call constraint (clock/movetime/depth/infinite, all optional);
  `Engine::resolve_limits()` resolves it and composed `SearchControl` arms the timer and owns stop/
  node-limit state. Every `Search(…, limits)` or `GetMove(limits)` call is self-contained — there
  is no pre-call ordering contract. UCI owns its concrete service directly for one session and passes
  a fresh observer per `go`; `ucinewgame` clears per-game state without rebuilding it.
- `Engine::compute_budget(remaining, increment, moves_to_go)` → `TimeBudget{soft, hard}` is pure.
- Verbose logging is opt-in per call site — the `AIPerplex` constructor does not enable it.

## Search internals

- **An aborted frame keeps no results.** `pvs()`/`quiescence()` check `IsAborted()` immediately after
  each recursive call returns **and the board is restored** — that ordering is the invariant, since
  returning before `UndoMove` would leave the board corrupt. So no TT store, PV row, killer or
  history write is reachable from a child that never finished; `best_value` (the best score over the
  children that *did* complete) is a valid lower bound and is what the root reports for an
  interrupted iteration. A write added below that guard is covered by it; one added above it has to
  justify itself the way the two documented exemptions do in comments there:
  - **Node counters** are incremented before the guard and stay incremented — they measure work
    done, not results kept.
  - **The quiescence stand-pat cutoff store** is reached before the node searches anything, so what
    it records owes nothing to a child; an impending abort does not make a static evaluation less
    true.
- Null-move pruning is gated by `tuning_.null_move_enabled` via `should_try_null_move()` (covers
  zugzwang, mate-score contamination, consecutive nulls, PV/in-check, min-depth).
- **Quiescence orders its two move lists differently**, via `AIPerplex::order_quiescence_moves()`.
  Out of check the list is captures and promotions and `SortMovesByValue` sorts it in place; in check
  it is every legal evasion and `MoveSorter::ScoreMoves` writes an order into a `scored_idx` array
  instead, so quiet evasions are ranked by history rather than by `-piece/16` (#320). Quiescence
  passes `Move::EmptyMove()` as the hash move in both phases and must keep doing so.
- **`ScoreMoves` applies one capture-tier policy to both its callers** — main `pvs()` and in-check
  quiescence. `See::see_ge(board, mv, 0)` splits captures into SEE >= 0 (above the killers, with all
  promotions) and SEE < 0 (below the killers, still above every quiet); `MoveHelper::Value()` scores
  within each. Moving the losing tier below the quiets is the tempting change and costs tens of
  percent in nodes: captures are never LMR-reduced, so it only lowers the move number of every quiet
  it steps over.

## Configuration

- `game_settings.json` holds per-player `"search_limits"`. It accepts C-style `/* */` comments via
  nlohmann, but PowerShell's `ConvertFrom-Json` does not.
- Run the exe from `StratChessEvolved/` — both so `game_settings.json` resolves and so logs land in
  `StratChessEvolved/logs/`.
