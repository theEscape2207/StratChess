# Plan: Fix `Board::currentPly_` Overflow in Ply-History Arrays (Issue #53)

## Context

Self-play (AIPerplex vs AIPerplex) crashed reproducibly with an access violation around real-game move 247-249, in two independent runs, in different endgame positions. Reported in https://github.com/theEscape2207/StratChess/issues/53 while validating an unrelated null-move-pruning (NMP) feature — but the bug is **independent of NMP**: this worktree (`master`) has no `DoNullMove`/`UndoNullMove` at all, and the root cause predates that feature.

**Root cause (confirmed by reading the code):**

- `Board::currentPly_` (`StratEngine/Board.h:180`) indexes four fixed-size arrays — `zobrist_history_`, `irreversiblePlyHistory_`, `gameInfoHistory_`, `capturedHistory_` (all `std::array<T, MAX_PLY>`, `MAX_PLY = 256` in `StratEngine/defines.h:96`).
- `DoMove` increments it (`Board.cpp:403`) and writes via `operator[]` with no bounds check; `UndoMove` decrements it (`Board.cpp:421`) and reads back. Search recursion (alpha-beta/PVS, quiescence) calls these in matched pairs that net to zero depth change.
- `currentPly_` is reset to 0 **only** in `clear_board()` (`Board.cpp:60`), which runs at `SetDefaultBoard`/`SetupFromFEN` — i.e. once per new game, not once per move.
- The one call site that commits a move *permanently* — `Game::Run()`'s main loop, `Game.cpp:271` (`rBoard.DoMove(newMove)`) — is never matched by an `UndoMove`. So `currentPly_` grows by 1 for every real move played, forever, for the lifetime of the game.
- Search recursion then adds its own (bounded) depth on top of that ever-growing baseline. Once `baseline + recursion depth > 256`, the four arrays are written out of bounds, corrupting adjacent memory — the observed access violation.
- Grepped `currentPly_` across the engine: it appears **only** in `Board.h`/`Board.cpp`. It has no purpose besides indexing these four arrays — confirming it's safe to redefine its semantics without touching any other subsystem.
- The existing `PlayerAiBase::Quiescent` ply guard (`ply == MAX_PLY-10`, `PlayerAI.cpp:42`) operates on a *different*, search-local `ply` parameter (threaded through `m_infoSeq`, a resizable `std::vector`) — it already bounds search recursion depth to ≤ `MAX_PLY-10`, but does nothing to protect `Board::currentPly_`.

---

## Design Decisions

### Reset-on-commit (chosen over decoupled-counters / ring-buffer / cutoff-fallback)

Since `currentPly_` has no other purpose, the structural fix is to make it represent **only the depth of the current in-flight (unmatched) `DoMove` recursion since the last permanently-committed position** — not "total plies since game start."

- Add `void Board::ResetSearchDepth() noexcept` — sets `currentPly_ = 0`. Public API, called by callers that just committed a move they will never undo.
- Call it immediately after every permanent (non-search) move commit:
  - `Game::Run()` (`Game.cpp:271-272`), right after `rBoard.DoMove(newMove)` succeeds.
  - `UCIHandler.cpp` `position moves ...` replay (`UCIHandler.cpp:95-99`), once after the loop finishes applying all moves (not per-move — nothing is undone between them, so one reset at the end is sufficient and cheaper).
- After this fix, `currentPly_` is bounded by max simultaneous search recursion depth, which is already capped near `MAX_PLY-10` by the existing `Quiescent` guard — fully decoupled from total game length.
- Considered and rejected:
  - **Ring-buffer modulo** (`array[currentPly_ % MAX_PLY]`): smaller diff, but relies on the `Quiescent` margin (~10 slots) never shrinking as search features evolve (e.g. check extensions). Fragile long-term for no benefit over the structural fix.
  - **Decoupled game-ply / search-ply counters with a growable container**: same practical outcome, more surface area (new member, new accessors, more call sites to audit) for no added correctness over reset-on-commit, since `currentPly_` has no other consumer to preserve.
  - **Bounds-check + cutoff fallback at MAX_PLY**: doesn't fix the root cause — `currentPly_` still grows with game length forever; just delays the crash to a longer game and wastes array capacity on game history it doesn't need.

### Defense in depth

- Add `assert(currentPly_ < MAX_PLY)` in `Board::DoMove` immediately before the first array write (`capturedHistory_[currentPly_] = capturedPiece;`, `Board.cpp:260`). Matches existing assert conventions in `Board.cpp` (e.g. `assert(MoveHelper::IsValid(...))`). Free in Release; catches any future regression loudly in Debug/tests — e.g. a new permanent-commit call site that forgets to call `ResetSearchDepth()`.

### Testing

- **Fast unit test** (`StratChessTests/BoardApiTests.cpp`, tag `[board_api]`): simulate a long game by repeatedly calling `DoMove` + `ResetSearchDepth()` in a loop (a simple repeatable legal king/rook shuffle on an open board, no real search needed) for 300+ iterations, asserting no crash and that `currentPly_` stays small throughout (e.g. via a handful of nested `DoMove` calls without intervening resets, to confirm headroom). This stays fast (no search) and runs in the default tier.
- **Manual long self-play validation**: run AIPerplex vs AIPerplex (`"type": 6` both sides) for 300+ real moves per `CLAUDE.md`'s self-play conventions, confirming the original access violation no longer reproduces. Not added as a permanent slow test — too slow/nondeterministic for CI; serves as one-time confirmation for this fix.

---

## Files Changed

| File | Change |
|---|---|
| `StratEngine/Board.h` | Declare `void ResetSearchDepth() noexcept;` (public) |
| `StratEngine/Board.cpp` | Implement `ResetSearchDepth()`; add `assert(currentPly_ < MAX_PLY)` guard in `DoMove` before first array write |
| `StratEngine/Game.cpp` | Call `rBoard.ResetSearchDepth()` after `rBoard.DoMove(newMove)` succeeds (`Game.cpp:271-272`) |
| `StratEngine/UCIHandler.cpp` | Call `Board::Instance().ResetSearchDepth()` once after the `position moves ...` replay loop (`UCIHandler.cpp:95-99`) |
| `StratChessTests/BoardApiTests.cpp` | Add unit test simulating a long game via repeated `DoMove` + `ResetSearchDepth()`, tag `[board_api]` |

---

## Step-by-Step Changes

### Step 1 — `Board.h`: declare `ResetSearchDepth`
Add near other public mutators (e.g. next to `DoMove`/`UndoMove` declarations):
```cpp
// Resets the search-recursion depth counter. Call after committing a move
// that will never be undone (a real game move, or a UCI position replay) —
// the four ply-history arrays only need to span in-flight search recursion,
// not total game length.
void ResetSearchDepth() noexcept;
```

### Step 2 — `Board.cpp`: implement `ResetSearchDepth`, add assert guard
```cpp
void Board::ResetSearchDepth() noexcept
{
    currentPly_ = 0;
}
```
In `DoMove`, immediately before `capturedHistory_[currentPly_] = capturedPiece;` (`Board.cpp:260`):
```cpp
assert(currentPly_ < MAX_PLY);
```

### Step 3 — `Game.cpp`: reset after committing the real move
```cpp
// Foretag traekket paa det virkelige braet
if (!rBoard.DoMove(newMove))
    assert(!"Unexpected illegal move found! Exiting...");
rBoard.ResetSearchDepth();
```

### Step 4 — `UCIHandler.cpp`: reset after replaying `position moves ...`
After the loop that applies all moves from the `position` command (`UCIHandler.cpp:95-99`), add one call:
```cpp
Board::Instance().ResetSearchDepth();
```

### Step 5 — `BoardApiTests.cpp`: regression test
Add a test simulating a long game (no real search) to prove `currentPly_` never grows unbounded:
```cpp
TEST_CASE("Board::ResetSearchDepth keeps undo-stack depth bounded across a long game", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_QUIET); // king/rook only, supports a repeatable shuffle

    for (int i = 0; i < 300; ++i)
    {
        // Repeatable legal shuffle move(s) for the position, committed permanently.
        Move m = /* ... */;
        REQUIRE(board.DoMove(m));
        board.ResetSearchDepth();

        // Simulate a few levels of search recursion on top — must not approach MAX_PLY.
        REQUIRE(board.DoMove(/* reply move */));
        board.UndoMove(/* reply move */);
    }
}
```
(Exact move sequence to be worked out against `FEN_QUIET`'s king/rook shuffle so every half-move pair is legal and repeatable.)

---

## Validation Plan

1. **Build**: `.\build.ps1` (Release|x64) — zero warnings, zero errors (Level4/WX enforced)
2. **Tests**: `.\build.ps1 run-tests` — all existing tags pass, plus new `[board_api]` case
3. **Manual self-play**: run AIPerplex vs AIPerplex (`"type": 6` both sides) in `StratChessEvolved/`, no special FEN, short per-move time budget, let it run past move ~260 — confirm no access violation (previously crashed at move 247-249)
4. **UCI path sanity check**: send a `position startpos moves ...` command with 250+ moves via UCI, then `go depth N` — confirm no crash and a legal `bestmove` is returned

---

## Key Correctness Properties

- `currentPly_` after this fix represents only the depth of in-flight (unmatched) `DoMove` calls since the last permanent commit — never the count of moves played since game start.
- `ResetSearchDepth()` must only be called when the undo stack is flat (no pending `UndoMove` calls owed) — i.e. right after a move that will never be undone. Calling it mid-search-recursion would corrupt the undo stack.
- The `assert(currentPly_ < MAX_PLY)` guard is defense-in-depth only; it does not change Release behavior and must not be relied upon as the primary fix.
- This fix is orthogonal to and required by any future null-move-pruning merge: `DoNullMove`/`UndoNullMove` (not present on this branch) will reuse the same `currentPly_` push/pop pattern as `DoMove`/`UndoMove`, and are automatically protected once this fix lands — no additional changes needed in NMP code for this specific bug.
- No change to evaluation, move ordering, or search algorithms — this is purely a state-management fix in `Board`.
