# Engine Contracts

Non-obvious API contracts in `StratEngine/`: what neither the signature nor the header comment tells
you. Read the relevant section before editing that area. `CLAUDE.md` repeats the aborted-frame rule;
change both together.

| Editing… | Read |
|---|---|
| move encoding or formatting | [Moves](#moves) |
| `AIPerplex`, `SearchPlayer`, UCI, time management | [The search service](#the-search-service) |
| `pvs()`, `quiescence()`, `Sort.cpp`, pruning, contempt | [Search internals](#search-internals) |
| `game_settings.json`, `SearchTuning` or its consumers | [Configuration](#configuration) |

---

## Moves

- `Move` is a pure 2-byte value (from/to/flags). The moving and captured pieces are **not** stored:
  use `Board::GetEffectiveMovPiece(m)` (pre-move only) and `Board::GetCapturedPiece(m)`. After
  `DoMove`, identify the moved piece with `board.GetPiece(m.to())`. Which board state each formatter
  expects is in `MoveFormatter.h`.

## The search service

- `ThreadData&` is the **first parameter of every search method**. The search runs on `td.board`,
  never the game board, and writes nothing back to it: the root verdict leaves via
  `SearchResult::game_state` and no other channel. The TT is a separate shared parameter — Lazy SMP
  helpers each get their own `ThreadData`.
- **A search board's TT prefetch target names a live table only inside `Search()`.** `Board` prefetches
  the child's bucket through an opaque `PrefetchTarget` that a copy carries, so the search binds it
  after copying the root (main and each helper board) and resets it to the dummy after the helpers
  join, on every exit. A stale prefetch never faults and changes no result, so only the
  `[tt_prefetch]` tests catch a missing bind or reset; `SetHash` may free the table once `Search()`
  returns.
- `SearchResult` carries best move, score, elapsed time, split node counts and the `GameStates` the
  player adjudicated at its own root. It is never `DRAW_50_MOVES`: the fifty-move rule is a fact about
  the committed position, and `Game::Run` adjudicates it. The returned value is the **post-join
  aggregate** and remains the authoritative record after later searches; Game owns combined totals.
- **`StartAsync(root, limits, observer, on_done)` is UCI's launch path.** It stops and joins any
  previous launch, then arms the stop handshake, copies the root and starts the thread; join-before-arm
  is load-bearing, because an unwinding `Search()` would otherwise clear the fresh arm. A `Stop()` made
  after it returns is never lost, even before `Search()` initialises. `IsSearching()` turns false
  when `Search()` returns, before `on_done` runs, so a client that sends `position` the instant it reads
  `bestmove` is accepted. `on_done` must not call `StartAsync`, `Wait`, `StopAndWait`, `SetHash`,
  `SetThreads`, `SetTuning`, `StartNewGame` or destroy the service (Debug-asserted). Those methods
  come from one controlling thread; only `Stop()` and `IsSearching()` are callable from any thread.

## Search internals

- **An aborted frame keeps no results.** The guard is **per move iteration, not per recursive call**:
  `pvs()` may run a reduced null-window search, a full-depth re-search and a PV re-search for one
  move before reaching `UndoMove` and the one `IsAborted()` check that follows it. The board is
  restored first — returning earlier would leave it corrupt — and `IsAborted()` checked before any
  persistent write, so no TT store, PV row, killer or history write is reachable from a child that
  never finished. `best_value`, the best score over the children that *did* complete, is a valid
  lower bound and is what the root reports for an interrupted iteration. A write added above the
  guard has to justify itself the way the two exemptions do in comments there:
  - **Node counters** are incremented before the guard and stay incremented — they measure work
    done, not results kept.
  - **The quiescence stand-pat cutoff store** is reached before the node searches anything, so what
    it records owes nothing to a child.
- **A drawn score is context the Zobrist key does not carry.** With `contempt` non-zero, the score
  of a draw — repetition, fifty-move, stalemate, or a position the evaluator settles as drawn —
  depends on the root colour and the contempt value, and reaches parent entries through the terminal
  store in `pvs()` and the bare-king store in `quiescence()`. So `Search()` keeps the
  `(root_color, contempt)` pair its table was filled under and clears the table when the incoming
  pair differs *and* either side of the change is non-zero; a process at the default of 0 never
  clears. The sign comes from `td.board.GetCurrentColor()` against `root_color_`, not from ply
  parity, which matches only because every ply-advancing construct, null moves included, flips the
  side to move. Abort and time-limit unwind values stay `GameValues::Draw`: they are fabricated, not
  game results.
- **Every draw the search can report carries contempt, including the ones the evaluator settles.**
  Tinting only repetitions would steer the engine into the dead ending it can never come back from.
  `Evaluate()`'s `endgame_scale == 0` early-out returns `dead_draw_score_[side to move]`, which
  `Search()` sets once through `AIPerplex::publish_draw_scores()`, beside `root_color_`. A position
  with a non-zero scale never reaches that line, so contempt shifts nothing the evaluator does not
  already call drawn.
- **`dead_draw_score_` has one writer, before any helper thread exists.** `Evaluator::SetDrawScores()`
  is private with `AIPerplex` its only friend, and the call sits above the helper-spawn block in
  `Search()`; below it, it is a data race the `[contempt][smp]` test exposes under tsan. Every other
  `Evaluator` — the UCI `eval` command's, the batch scorer's, every test's — is unconfigured and
  answers `GameValues::Draw`. Guarding each `Evaluate()` call instead costs ~1% nps.
- **At `contempt > 0` a static evaluation depends on `root_color_`, not on the position alone.** Its
  consumers — reverse futility, frontier futility and the quiescence stand-pat — shift by at most
  `contempt`; none caches a static eval across a root-colour change, and the TT stores scores, not
  static evals. The TT clear fires on every root-colour flip, so a GUI analysing both sides, or the
  tactical runner sweeping colours, discards the table each search: the guard working, not a TT bug.
- **A probed TT entry may belong to another position** — through a key collision, or a slot whose
  two words racing Lazy SMP stores mixed. Its `best_move` is only a hint, matched against moves the
  engine generated. A change that searches the hash move before generating (a staged move generator)
  must check it is pseudo-legal in this position first.
- **`ScoreMoves` applies one capture-tier policy to both its callers** — main `pvs()` and in-check
  quiescence. `See::see_ge(board, mv, 0)` splits captures into SEE >= 0 (above the killers, with all
  promotions) and SEE < 0 (below the killers, still above every quiet); `MoveHelper::Value()` scores
  within each. Moving the losing tier below the quiets is the tempting change and costs tens of
  percent in nodes: captures are never LMR-reduced, so it only lowers the move number of every quiet
  it steps over.
- **`pvs()` orders its move list lazily.** `MoveSorter::ScoreMovesBestFirst` orders only
  `scored_idx[0]`; the loop calls `OrderRemaining(…, 1, n)` at the top of its body when `si == 1`,
  before any `continue`. Until then nothing may read `scored_idx[i]` for `i ≥ 1` — a new reader before
  the loop, or above that call, sees an unordered tail and changes the tree silently. Every entry
  point shares one comparator, so the order equals `ScoreMoves`' full sort.

## Configuration

- `game_settings.json` holds per-player `"search_limits"`. It accepts C-style `/* */` comments via
  nlohmann, but PowerShell's `ConvertFrom-Json` does not.
- Run the exe from `StratChessEvolved/` — both so `game_settings.json` resolves and so logs land in
  `StratChessEvolved/logs/`.
- **`SearchTuning` is declared once, in `StratEngine/SearchTuning.def`.** Each entry carries the
  field's type, default, accepted domain, JSON binding, UCI name and build availability; the struct,
  `SearchTuningSchema::Validate` and the JSON reader are generated from it, and cross-field
  constraints (the aspiration window's doubling) are written out in `SearchTuningSchema.cpp`. A new
  field of an existing type is one entry plus its tests. Validation is all-or-nothing and applies at
  `AIPerplex` construction as well as to `game_settings.json`, which rejects an out-of-domain value
  naming the field; a field the catalogue marks unavailable (a compiled-out feature) may hold its
  default but never change, so such a feature's switch may be set false but never true.
- **UCI reaches `SearchTuning` only through the catalogue's UCI names.** `uci` advertises them
  (the same set in every build) and `setoption` applies one through
  `AIPerplex::SetTuning`, which validates the whole tuning and **clears the TT when the tuning
  changes** — stored scores came from the old pruning. It is idle-only like `SetHash`, so UCI refuses
  it mid-search. An applied value is echoed as `info string <Name> <value>`, so a match's protocol
  log shows what each engine ran; an invalid value prints an `info string` and changes nothing, TT
  included; an unknown name stays silent. `ucinewgame` keeps the tuning. Any field without a UCI name
  is still reachable only through `game_settings.json` in `game` mode, or by rebuilding with a new
  default — and `Run-Bench.ps1`, `Compare-SearchEquivalence.ps1` and every match harness drive UCI.
