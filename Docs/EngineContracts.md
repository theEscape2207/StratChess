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
- **`StartAsync(root, limits, observer, on_done)` is UCI's launch path.** It stops and joins any
  previous launch, then arms the stop handshake, copies the root and starts the thread; join-before-arm
  is load-bearing, because an unwinding `Search()` would otherwise clear the fresh arm. A `Stop()` made
  after it returns is never lost, even before `Search()` initialises. `IsSearching()` turns false
  when `Search()` returns, before `on_done` runs, so a client that sends `position` the instant it reads
  `bestmove` is accepted. `on_done` must not call `StartAsync`, `Wait`, `StopAndWait`, `SetHash`,
  `SetThreads`, `SetTuning`, `StartNewGame` or destroy the service (Debug-asserted). Those methods
  come from one controlling thread; only `Stop()` and `IsSearching()` are callable from any thread.
- `Engine::compute_budget(remaining, increment, moves_to_go)` → `TimeBudget{soft, hard}` is pure.
- Verbose logging is opt-in per call site — the `AIPerplex` constructor does not enable it.

## Search internals

- **An aborted frame keeps no results.** The guard is **per move iteration, not per recursive call**:
  `pvs()` may run a reduced null-window search, a full-depth re-search and a PV re-search for a
  single move before reaching `UndoMove` and the one `IsAborted()` check that follows it. The
  invariant is that the board is restored and `IsAborted()` checked after that whole sequence and
  before any persistent write — the ordering against `UndoMove` matters because returning first
  would leave the board corrupt. So no TT store, PV row, killer or history write is reachable from a
  child that never finished; `best_value` (the best score over the
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
- **Late move pruning returns entry alpha from a completed fail-low and stores nothing.** A depth-2
  null-window frame that skipped a late quiet move and then failed low returns `original_alpha`,
  fail-hard, and writes no TT entry — neither UPPER nor EXACT — because the bound would rest on moves
  it never searched. It is a selective result, not a proof: it neither makes an ancestor exact nor
  removes earlier entries, and an ancestor may still cut off on it and store normally. A *searched*
  cutoff after a skip stores LOWER as usual; an aborted frame takes the unwind guard first. Skips
  advance the legal-move index and need make/unmake, so checking moves and immediate repetition or
  fifty-move draws are never skipped. A quiet move that stalemates the opponent is not detected and
  can be skipped.
- **A drawn score is context the Zobrist key does not carry.** With `contempt` non-zero, the score
  of a draw — repetition, fifty-move, stalemate, or a position the evaluator settles as drawn —
  depends on the root colour and on the contempt value, and propagates into parent entries through the terminal store in
  `pvs()` and the bare-king store in `quiescence()`. The key holds neither, so `Search()` keeps the
  `(root_color, contempt)` pair its table was filled under and clears the table when the incoming pair
  differs *and* either side of the change is non-zero. Both halves matter: only a contempt search can
  tint an entry or misread an untinted one, so a process left at the shipped default of 0 must never
  clear — that would be a behaviour change where nothing was ever tinted. The sign comes from
  `td.board.GetCurrentColor()` against `root_color_`, which is the contract itself. Ply parity is
  equivalent today — every construct that advances a ply also flips the side to move, null moves
  included — but that is an unstated invariant of the search rather than a property of the draw
  score, and parity would invert silently if it ever stopped holding. Abort and time-limit unwind
  values stay at
  `GameValues::Draw`: they are fabricated, not game results.
- **Every draw the search can report carries contempt, including the ones the evaluator settles.**
  A liquidation into a dead ending and a repetition are both draws; tinting only one would make the
  engine prefer the draw it can never come back from — a gradient pointing the wrong way, not
  merely an inconsistent score. `Evaluate()`'s `endgame_scale == 0` early-out returns
  `dead_draw_score_[side to move]` rather than the constant `GameValues::Draw`, and `Search()` sets
  that pair once through `AIPerplex::publish_draw_scores()`, beside `root_color_`. A position whose scale
  is non-zero never reaches that line, so contempt cannot shift anything the evaluator does not
  already call drawn — tinting an evaluation that merely landed on zero would put a step in the
  middle of the scale.
- **`Evaluator` is no longer literally stateless, and the weakened contract is what search relies
  on.** `dead_draw_score_` is written only by `SetDrawScores()` before any helper thread exists and
  is read-only for the rest of the search, exactly as `AIPerplex::tuning_` is. Calling it mid-search
  would be a data race, so `Evaluator::SetDrawScores()` is **private with `AIPerplex` its only
  friend** — the single-writer rule is enforced by the compiler rather than asserted in a comment. Every other `Evaluator` in the process — the UCI `eval`
  command's, the batch scorer's, every test's — is its own instance that nobody configures, so it
  keeps answering `GameValues::Draw`. Putting the value here rather than guarding each `Evaluate()`
  call was a cost decision, and it was measured: three different per-evaluation guards each cost
  ~1% nps at a default that tints nothing, while reading it on a branch already being taken is free.
- **At `contempt > 0` a static evaluation is no longer a function of the position alone** — it
  depends on `root_color_`. Its consumers are reverse futility, frontier futility and the quiescence
  stand-pat; all shift by at most `contempt`, none caches a static eval across a root-colour change,
  and the TT stores scores rather than static evals. Recorded because "the evaluation depends only
  on the board" was previously true engine-wide, which makes it the kind of assumption a later
  change inherits without checking.
  A cost, also only at `contempt > 0`: the clear fires on every root-colour flip, so a GUI
  analysing both sides, or the tactical runner sweeping colours, discards the table each search.
  That is the guard working, not a TT bug.
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
- **`SearchTuning` is declared once, in `StratEngine/SearchTuning.def`.** Each entry carries the
  field's type, default, accepted domain, JSON binding, UCI name and build availability; the struct,
  `SearchTuningSchema::Validate` and the JSON reader are generated from it, and cross-field
  constraints (the aspiration window's doubling) are written out in `SearchTuningSchema.cpp`. A new
  field of an existing type is one entry plus its tests. Validation is all-or-nothing and applies at
  `AIPerplex` construction as well as to `game_settings.json`, which rejects an out-of-domain value
  naming the field; a compiled-out feature (singular extensions in the shipping build) may be set
  false but never true.
- **UCI reaches `SearchTuning` only through the catalogue's UCI names.** `uci` advertises them
  (`SingularExtensions` only in a build compiling it in) and `setoption` applies one through
  `AIPerplex::SetTuning`, which validates the whole tuning and **clears the TT when the tuning
  changes** — stored scores came from the old pruning. It is idle-only like `SetHash`, so UCI refuses
  it mid-search. An applied value is echoed as `info string <Name> <value>`, so a match's protocol
  log shows what each engine ran; an invalid value prints an `info string` and changes nothing, TT
  included; an unknown name stays silent. `ucinewgame` keeps the tuning. Any field without a UCI name is still
  reachable only through `game_settings.json` in `game` mode, or by rebuilding with a new default —
  and `Run-Bench.ps1`, `Compare-SearchEquivalence.ps1` and every match harness drive UCI.
