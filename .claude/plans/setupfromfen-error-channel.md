# `Board::SetupFromFEN` error channel (issues #155, #46)

## Goal

Give `Board::SetupFromFEN` a failure channel the caller cannot ignore, and use it at the two call
sites where a malformed FEN currently produces plausible-looking output: `UciHandler::cmd_position`
and `Config::ReadFEN`. Closing that at the UCI site is also what remains of #46.

## Scope limits

- **No new validation rules.** Legality checking (a FEN whose non-mover is in check, issue #45) is
  deliberately out of scope: this PR builds the channel, #45 adds a rule that reports through it.
- **`FenBatch::ClassifyLine` stays.** #155 flags shrinking it to blank/comment filtering as a
  possible follow-on once callers can detect failure. Not part of this change.
- **No behaviour change on any well-formed FEN.** Expected Elo impact: none.

## Why #46 rides along

#46 as filed ("silently defaults to black-to-move when the side-to-move field is missing") no longer
reproduces. #143 added the `parts.size() < 4` floor in `FENParser::ParseFEN`, so a bare piece-placement
string is rejected with `"too few fields in FEN"` and never reaches the side-to-move default;
`cmd_position` passes the FEN through unpadded, so that path is covered.

What survives is a different silent failure with the same symptom: the parse error is logged, nothing
is applied, and the engine answers `bestmove` for a position the GUI never sent. That is #155's UCI
call site. So #46 needs no engine change of its own — it needs a regression test at the UCI level and
two doc corrections, both of which state a consequence that is now wrong.

## Design decisions

### 1. `[[nodiscard]] bool`, not an exception or an out-param

`[[nodiscard]]` under Level4 + `/WX` is what does the real work: every existing call site becomes a
build error until it is handled, so nothing can silently keep today's behaviour. Contract: returns
`true` on success; on failure logs the parse error and leaves the board **untouched** (unchanged from
whatever it held, which for a fresh `Board` is empty).

### 2. `Board(const std::string& fen)` asserts

The constructor has no way to report failure and is used ~145 times across `StratChessTests/`, all
with literal FENs. Converting those to two-step construction is churn with no benefit. Instead the
constructor consumes the result in an `assert`:

```cpp
Board::Board(const std::string& fen) : Board()
{
    [[maybe_unused]] const bool ok = SetupFromFEN(fen);
    assert(ok && "Board(fen): malformed FEN, board left empty");
}
```

A malformed literal FEN in a test is a bug in the test, and Debug builds are where that should
surface. Release behaviour is exactly as today (empty board, error logged). `[[maybe_unused]]` is the
approved suppression for a value used only in an `assert`.

### 3. `cmd_position` must early-return, not just skip the setup

`cmd_position` applies the `moves` list after the position block. If a malformed FEN only skipped the
setup, the move list would then be replayed **onto the stale board** — a worse outcome than doing
nothing. So failure returns from the function immediately, before the `moves` handling.

Per the decision recorded in #155: ignore the command, keep the current board, log at debug level.
UCI has no error channel for this, and silently declining is conventional engine behaviour.
Explicitly *not* resetting to the starting position — that would hand the GUI a legal-looking
`bestmove` for a position it never asked about.

### 4. `Config::ReadFEN` falls back to the default board

A typo in `game_settings.json` currently starts the game from whatever the board already held. The
sibling path directly above it (`Config.cpp:30-35`, FEN key present but empty) already logs and calls
`SetDefaultBoard()`, so failure follows that precedent: log at error level, `SetDefaultBoard()`, and
return **without** `SetCustomGame()` — matching the empty-string path exactly.

### 5. `STARTING_FEN` call sites assert

`UCIHandler.cpp:83` and `:212` pass a compile-time constant that must always parse. An `assert` is the
honest handling; a runtime branch there would be unreachable code.

## Files changed

| File | Change |
|---|---|
| `StratEngine/Board.h` | `void` → `[[nodiscard]] bool SetupFromFEN`, documented contract |
| `StratEngine/Board.cpp` | `return false`/`return true` paths; `Board(fen)` asserts |
| `StratEngine/UCIHandler.cpp` | `cmd_position` early-returns on failure (debug log); two `STARTING_FEN` asserts |
| `StratEngine/Config.cpp` | `ReadFEN` falls back to `SetDefaultBoard()` |
| `StratEngine/Tests/Perft.cpp` | suite runner reports and skips a position that fails to set up |
| `StratChessEvolved/StratChessEvolved.cpp` | 3 sites: FEN self-test counts a failure, perft CLI exits 1, batch eval warns and skips |
| `StratEngine/Utils/FenBatch.h` | header comment: validity is now also detectable from the return value |
| `StratChessTests/MoveFormatterTests.cpp` | 26 sites → `REQUIRE(board.SetupFromFEN(...))` |
| `StratChessTests/EvalTests.cpp` | 2 sites → `REQUIRE(...)` |
| `StratChessTests/UCITests.cpp` | 1 site → `REQUIRE(...)`, stale comment corrected, new `[uci]` cases |
| `CLAUDE.md`, `Docs/TestDesign.md` | correct the #46 consequence (parse error + position not applied, not "plays as Black") |
| `Docs/Changelog.md` | entry |

## Step-by-step

1. `Board.h` / `Board.cpp` — signature, both return paths, constructor assert.
2. Engine + app call sites (UCIHandler, Config, Perft, StratChessEvolved) per the decisions above.
3. Test call sites — mechanical `REQUIRE()` wrap. Each is a coverage improvement: several currently
   assume a FEN parsed without checking.
4. New `[uci]` tests (below).
5. Docs.
6. Build Release **and Debug** (the constructor assert only exists in Debug), run the fast tier.

## Validation plan

`Validate-PrePR.ps1` puts this at **Engine** tier — it changes UCI behaviour on malformed input.

New `[uci]` coverage:

- `position fen <malformed>` leaves the previous board intact (#155's stated requirement).
- `position fen <malformed> moves e2e4` leaves the board intact — the move list is **not** replayed
  onto the stale position (guards decision 3).
- `position fen 6k1/5ppp/8/8/8/8/5PPP/R5K1` (no side-to-move field) is declined and the previous
  position and side to move survive — the #46 regression test.
- `SetupFromFEN` returns `false` on a malformed FEN and leaves the board empty; `true` on a valid one.

No Elo match: error handling only, no change on well-formed input.

## Invariants that must hold afterwards

- No call site ignores the return value (guaranteed by `[[nodiscard]]` + `/WX`).
- A failed `SetupFromFEN` mutates nothing on the board.
- `cmd_position` never applies a `moves` list to a board the requested FEN did not load.
- Every well-formed FEN behaves exactly as before this change.
