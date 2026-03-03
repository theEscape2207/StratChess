# Plan: Move class → 16-bit layout — Phase 3 (Remove `MovPiece`)

**Goal**: Remove the `ePiece MovPiece` field from the `Move` struct. Replace board-side reads with `Board::GetEffectiveMovPiece()`. Thread `ePiece movPiece` explicitly to callers that need it.

**Scope**: Phase 3 only. `Content` stays. `static_assert(sizeof(Move)==2)` is Phase 4.

---

## Design Decisions

- `Value(const Move&, ePiece movPiece)` — static method, caller supplies piece
- `Output(ePiece movPiece)` — added overload for piece-aware display; `Output()` (no args) returns coordinate-only form (`e2-e4`) for PV/logging consumers that lack board context
- `operator<<` uses coordinate `Output()` — verbose verbose description goes away until `MoveFormatter` lands
- MoveHelper functions — add `ePiece movPiece` parameter to all that currently read `move.MovPiece`
- `GameState::UpdateBoardInfo` — add `ePiece movPiece` parameter
- `MoveFactory` — remove `movPiece` parameter from factory functions (it's no longer stored)
- `SortMovesByValue` — add `const Board& board` parameter; use lambda instead of `std::greater<>()`
- `operator>(Move, Move)` removed — was the only caller of the now-parameterized `Value()`

---

## Files Changed

1. `StratEngine/Move.h` + `Move.cpp`
2. `StratEngine/Board.h` + `Board.cpp`
3. `StratEngine/MoveHelper.h`
4. `StratEngine/GameState.h`
5. `StratEngine/MoveFactory.h`
6. `StratEngine/MoveGenerator.cpp`
7. `StratEngine/Sort.h` + `Sort.cpp`
8. `StratEngine/AIPerplex.cpp`
9. `StratEngine/PlayerHuman.cpp`
10. `StratChessTests/BoardTests.cpp` (new)
11. `StratChessTests/StratChessTests.vcxproj` + `.vcxproj.filters`

---

## Step-by-Step Changes

### Step 1 — `Board.h`: Add `GetEffectiveMovPiece()`

Add a public method:

```cpp
// Returns the piece semantically moving in this move:
// - For normal moves: the piece currently on the from-square (mailbox lookup).
// - For promotions: the promoted piece derived from the MoveType flags + pawn color.
// Call BEFORE applying the move (from-square must still be occupied).
ePiece GetEffectiveMovPiece(const Move& m) const noexcept;
```

### Step 2 — `Board.cpp`: Implement `GetEffectiveMovPiece()`

```cpp
ePiece Board::GetEffectiveMovPiece(const Move& m) const noexcept
{
    const ePiece onBoard = GetPiece(m.from());
    if (!MoveHelper::IsPromote(m))
        return onBoard;
    // For promotions the from-square still holds a pawn; derive promoted piece from flags.
    const eColor color = PieceHelper::Color(onBoard);
    switch (static_cast<MoveType>(m.flags())) {
        case MoveType::PROMOTION_QUEEN:  return PieceHelper::AsPiece(QUEEN,  color);
        case MoveType::PROMOTION_ROOK:   return PieceHelper::AsPiece(ROOK,   color);
        case MoveType::PROMOTION_BISHOP: return PieceHelper::AsPiece(BISHOP, color);
        case MoveType::PROMOTION_KNIGHT: return PieceHelper::AsPiece(KNIGHT, color);
        default: return onBoard;
    }
}
```

### Step 3 — `Board.cpp`: Update `DoMove()`

At the top of `DoMove()`, compute `movPiece` once:
```cpp
const ePiece movPiece = GetEffectiveMovPiece(m);
```
Replace every `m.MovPiece` read with `movPiece`. Replace `move_piece(m)` calls with `move_piece(movPiece, from, to)` directly.

Remove the private `move_piece(const Move&)` overload — callers now use `move_piece(ePiece, eSquare, eSquare)` explicitly.

Pass `movPiece` to `gameInfo_.UpdateBoardInfo(m, movPiece)`.

### Step 4 — `Board.cpp`: Update `UndoMove()`

Replace:
```cpp
const auto movingPiece = m.MovPiece;
```
With:
```cpp
const ePiece movingPiece = GetPiece(m.to());
```
This is valid because at undo time the piece is still sitting on `to`:
- Non-promotion: `GetPiece(to)` = the moved piece ✓
- Promotion: `GetPiece(to)` = the promoted piece (e.g., WHITE_QUEEN) ✓ — `PieceHelper::AsPawn(promotedPiece)` restores the pawn.

Replace all subsequent `m.MovPiece` uses with `movingPiece`.

### Step 5 — `Move.h`: Remove `MovPiece` field and update signatures

Remove:
```cpp
ePiece MovPiece { NO_PIECE };
```

Remove constructor: `Move(ePiece movPiece)`.

Change full constructor:
```cpp
// Before
Move(eSquare from, eSquare to, MoveType type, ePiece movPiece, ePiece content)
// After
Move(eSquare from, eSquare to, MoveType type, ePiece content) noexcept
```

Remove `SetMove(from, to, moveType, movPiece, takenPiece)` overload (or drop movPiece param).

Update `Clear()` to remove `MovPiece = ePiece::NO_PIECE`.

Change `Value()`:
```cpp
// Before
static int Value(const Move& move) noexcept;
// After
static int Value(const Move& move, ePiece movPiece) noexcept;
```

Remove `operator>(lhs, rhs)` (it called the old `Value()`).

Add `Output(ePiece movPiece)` — piece-prefix form.
Keep `Output()` (no arg) — coordinate-only form (`e2-e4`, `0-0`, etc.).

### Step 6 — `Move.cpp`: Update implementations

`Output()` (no arg): drop the `assert(PieceHelper::IsActual(MovPiece))`, return coordinate form — pieces show as `?` or are simply omitted (e.g., `e2-e4`, `c5xe6`, `0-0`). This is the form used by `PVLine::operator<<` and `operator<<`.

`Output(ePiece movPiece)`: the full piece-prefix form (`Pe2-e4`, `pb7xc8Q`).

`operator<<`: remove the verbose English description (reads MovPiece). Print the move using the coordinate `Output()`. Remove `assert(PieceHelper::IsActual(m.MovPiece))`.

`PVLine::operator<<`: no change (already calls `move.Output()`).

### Step 7 — `MoveHelper.h`: Add `movPiece` parameter

For each function that reads `move.MovPiece`, add `ePiece movPiece` parameter:

| Function (before) | Function (after) |
|---|---|
| `IsMoveType(move, ePieceType)` | `IsMoveType(move, ePiece movPiece, ePieceType)` |
| `IsMovingPiece(move, ePiece)` | `IsMovingPiece(move, ePiece movPiece, ePiece)` |
| `IsPieceMovingFrom(move, ePiece, eSquare)` | `IsPieceMovingFrom(move, ePiece movPiece, ePiece, eSquare)` |
| `IsPawnMove(move)` | `IsPawnMove(move, ePiece movPiece)` |
| `IsKingMove(move)` | `IsKingMove(move, ePiece movPiece)` |
| `GetEnPassantSquare(move)` | `GetEnPassantSquare(move, ePiece movPiece)` |
| `IsValid(move)` | `IsValid(move, ePiece movPiece)` |

`IsPieceCapturedAt`, `IsCapture`, `IsPromote`, `IsEnPassant`, `IsCastling`, `IsEmpty`: no change (don't read MovPiece).

### Step 8 — `GameState.h`: Thread `movPiece`

```cpp
// Before
void UpdateBoardInfo(const Move& move) noexcept;
void UpdateCastlingState(const Move& m) noexcept;
void UpdateFiftyMovesState(const Move& move) noexcept;

// After
void UpdateBoardInfo(const Move& move, ePiece movPiece) noexcept;
void UpdateCastlingState(const Move& m, ePiece movPiece) noexcept;
void UpdateFiftyMovesState(const Move& move, ePiece movPiece) noexcept;
```

`UpdateCastlingState`: replace `PieceHelper::Color(m.MovPiece)` with `PieceHelper::Color(movPiece)`, and `MoveHelper::IsKingMove(m)` with `MoveHelper::IsKingMove(m, movPiece)`.

`UpdateFiftyMovesState`: replace `MoveHelper::IsPawnMove(move)` with `MoveHelper::IsPawnMove(move, movPiece)`.

Callers of `UpdateBoardInfo` (Board::DoMove and PlayerHuman.cpp) supply the resolved `movPiece`.

### Step 9 — `MoveFactory.h`: Remove `movPiece` parameter

Factory functions no longer accept or store `movPiece`:

```cpp
// Before
MakeMove(eSquare from, eSquare to, ePiece movPiece, MoveType, ePiece captured)
MakeQuiet(eSquare from, eSquare to, ePiece movPiece)
MakeCapture(eSquare from, eSquare to, ePiece movPiece, ePiece captured)
MakePromotion(eSquare from, eSquare to, ePiece promotedPiece, ePiece captured)
MakeEnPassant(eSquare from, eSquare to, ePiece movPiece, ePiece captured)

// After
MakeMove(eSquare from, eSquare to, MoveType, ePiece captured = NO_PIECE)
MakeQuiet(eSquare from, eSquare to)
MakeCapture(eSquare from, eSquare to, ePiece captured)
MakePromotion(eSquare from, eSquare to, ePiece promotedPiece, ePiece captured = NO_PIECE)
  // promotedPiece kept for MoveType flag computation only — not stored in Move
MakeEnPassant(eSquare from, eSquare to, ePiece captured)
```

The `Move` constructor call inside each factory drops `movPiece`:
```cpp
// Before: Move m(from, to, moveType, movPiece, captured);
// After:  Move m(from, to, moveType, captured);
```

### Step 10 — `MoveGenerator.cpp`: Update factory call sites

All calls to factory functions drop the `movPiece` argument. E.g.:
```cpp
// Before:
MakeMove(from, to, movPiece, MoveType::CAPTURE, captured)
// After:
MakeMove(from, to, MoveType::CAPTURE, captured)
```

`AddPawnCaptures`: Replace `move.MovPiece` reads (lines 424, 430, 442) with the `color` parameter already available in that function. Asserts update accordingly.

### Step 11 — `Sort.h` + `Sort.cpp`: Add `Board` parameter

```cpp
// Sort.h
static void SortMoves(MoveList&, const Move& lastMove, const Board& board, size_t startIndex = 0);
static void SortMovesIter(MoveList&, const Move& lastMove, const Move* pIterMove, const Board& board);
static void SortMovesByValue(MoveList&, size_t captures, const Board& board, size_t start = 0);
```

`SortMovesByValue` changes from `std::greater<>()` to a lambda:
```cpp
std::sort(moveList.begin() + start, moveList.begin() + (start + captures),
    [&board](const Move& a, const Move& b) {
        return Move::Value(a, board.GetEffectiveMovPiece(a))
             > Move::Value(b, board.GetEffectiveMovPiece(b));
    });
```

Update callers of `SortMoves`/`SortMovesIter` in `AIPerplex.cpp` to pass `Board::Instance()`.

### Step 12 — `AIPerplex.cpp`: Update `Value()` calls and MoveHelper calls

- Line 330: `Move::Value(mv)` → `Move::Value(mv, Board::Instance().GetEffectiveMovPiece(mv))`
- Line 535: `Move::Value(move)` → `Move::Value(move, Board::Instance().GetEffectiveMovPiece(move))`
- Any `MoveHelper::IsPawnMove(m)` etc. → add `movPiece` from `Board::Instance().GetEffectiveMovPiece(m)`
- Pass `board` to `SortMoves`/`SortMovesIter` calls

### Step 13 — `PlayerHuman.cpp`: Update promotion selection

**ParseInput** (lines 141, 147): Remove the `move.MovPiece` assignments. Instead, when the user specifies a promotion suffix, record the `ePieceType` locally. Build the user's move with `SetMove(from, to)` (coordinate-only, no flags). Promotion type preference is handled in the selection loop.

**Promotion selection loop** (lines ~88–100): Replace MovPiece comparison with MoveType flag comparison:
```cpp
// Map user's chosen piece type to a MoveType
auto toPromotionType = [](ePieceType pt) -> MoveType {
    switch (pt) {
        case QUEEN:  return MoveType::PROMOTION_QUEEN;
        case ROOK:   return MoveType::PROMOTION_ROOK;
        case BISHOP: return MoveType::PROMOTION_BISHOP;
        case KNIGHT: return MoveType::PROMOTION_KNIGHT;
        default:     return MoveType::PROMOTION_QUEEN;
    }
};

// Find legal move that matches from/to and the desired promotion type
const MoveType targetType = hasPromoChoice ? toPromotionType(userPieceType) : MoveType::PROMOTION_QUEEN;
auto moveIt = std::find_if(..., [&](const Move& m) {
    return m.IsSameAs(userMove) && MoveHelper::AsType(m) == targetType;
});
// Fallback to queen if not found
if (moveIt == ...) {
    moveIt = std::find_if(..., [&](const Move& m) {
        return m.IsSameAs(userMove) && MoveHelper::AsType(m) == MoveType::PROMOTION_QUEEN;
    });
}
```

### Step 14 — `StratChessTests/BoardTests.cpp` (new file)

Create `[board]` test suite as specified in `Docs/TestDesign.md` Phase 1:

```cpp
TEST_CASE("En passant DoMove/UndoMove restores captured pawn", "[board]") { ... }
TEST_CASE("Castling DoMove/UndoMove restores king and rook", "[board]") { ... }
TEST_CASE("Promotion move generation: pawn b7→b8 yields PROMOTION_QUEEN", "[board]") { ... }
TEST_CASE("Capture-promotion c7xb8 generates PROMOTION_QUEEN with captured piece", "[board]") { ... }
TEST_CASE("Promotion DoMove/UndoMove: pawn becomes queen and is restored", "[board]") { ... }
TEST_CASE("Zobrist hash invariant after DoMove/UndoMove cycle", "[board]") { ... }
```

### Step 15 — Update `.vcxproj` and `.vcxproj.filters`

Add `BoardTests.cpp` to `StratChessTests/StratChessTests.vcxproj` (ClCompile item) and matching `<Filter>` entry in `.vcxproj.filters`.

---

## Validation Plan

| Step | Command | Pass criteria |
|---|---|---|
| Build | MSBuild Release x64 | 0 errors, 0 warnings |
| Unit tests | `StratChessTests.exe` | All pass: [repetition], [moves], [perft], [tt], [eval], [tactical], **[board]** (new) |
| Deep perft | `cd Tests && ../x64/Release/StratChessEvolved.exe perft test` | All positions match expected node counts |
| Self-play | AIPerplex vs AIPerplex (type 6 both sides) | No crashes, normal game progression, reasonable moves |
| Verify FEN | `game_settings.json` | Reset FEN to starting position before commit |

---

## What Changes for Phase 4 (Next)

- Remove `ePiece Content` from Move
- Remove `assert(capturedPiece == m.Content)` in DoMove (capturedHistory_ already the sole source)
- Change `Move::Value(move, movPiece)` to `Move::Value(move, movPiece, content)` — or derive content in Value() from MoveType flags context
- Update `MoveHelper::IsCapture()` (reads Content) and `IsPieceCapturedAt()` (reads Content)
- Add `static_assert(sizeof(Move) == 2)`
- Strip `captured` param from MoveFactory

---

## Key Correctness Properties Preserved

- **Promotion DoMove**: `GetEffectiveMovPiece()` returns promoted piece (from flags) → `remove_piece_from_board(GetPiece(from), from)` removes pawn, `add_piece_to_board(movPiece, to)` places queen ✓
- **Promotion UndoMove**: `GetPiece(to)` returns promoted piece → `remove_piece_from_board(promotedPiece, to)` + `add_piece_to_board(AsPawn(promotedPiece), from)` ✓
- **En-passant GetEnPassantSquare**: `movPiece` provides color → correct EP square direction ✓
- **Castling rights**: `UpdateCastlingState(m, movPiece)` uses `Color(movPiece)` and `IsKingMove(m, movPiece)` ✓
- **Fifty-move rule**: `UpdateFiftyMovesState(m, movPiece)` uses `IsPawnMove(m, movPiece)` ✓
