# Board Test Coverage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 32 new test cases across three new files — `[board_moves]`, `[board_state]`, `[board_api]` — covering all Board move-type round-trips, GameInfo state lifecycle, material scores, side-to-move, and public query APIs.

**Architecture:** Three focused test files, each with its own Catch2 tag and independent FEN constants. No production code changes — every test exercises existing Board functionality. All tests use `Board::Instance()` + `SetupFromFEN()` per the existing test isolation rules (Phase 2 will migrate to non-singleton).

**Tech Stack:** C++20, Catch2 v3 amalgamated, MSBuild via `build.ps1`

---

## File Map

| Action | Path |
|---|---|
| Create | `StratChessTests/BoardMoveTests.cpp` |
| Create | `StratChessTests/BoardStateTests.cpp` |
| Create | `StratChessTests/BoardApiTests.cpp` |
| Modify | `StratChessTests/StratChessTests.vcxproj` — add 3 ClCompile entries |
| Modify | `StratChessTests/StratChessTests.vcxproj.filters` — add 3 filter entries |
| Modify | `Docs/TestDesign.md` — add new tags to run-by-tag list + coverage map |
| Modify | `Docs/Roadmap.md` — update `[board]` item note |

---

## Key Reference Values

```
// Piece values from defines.h g_iPieceValues (indexed by ePieceType >> 1):
PAWN   = 100
KNIGHT = 300
BISHOP = 300
ROOK   = 500
QUEEN  = 900
KING   = 10000

// eSquare layout (a8=0, h8=7, ..., a1=56, h1=63)
// FEN rank 8 = row 0; FEN rank 1 = row 7

// MoveFactory API (MoveFactory.h):
//   MakeQuiet(from, to)
//   MakeCapture(from, to)
//   MakeEnPassant(from, to)
//   MakeMove(from, to, MoveType)        // for castling, double push
//   MakePromotion(from, to, piece, isCapture=false)

// MoveType values (defines.h):
//   QUIET=0, DOUBLE_PAWN_PUSH=1, KING_CASTLE=2, QUEEN_CASTLE=3
//   CAPTURE=4, EP_CAPTURE=5
//   PROMOTION_KNIGHT=8..PROMOTION_QUEEN=11
//   PROMOTION_*_CAPTURE=12..15

// GameInfo fields (GameState.h, accessed via board.GetGameInfo()):
//   .castlingRights  (uint8_t, bit flags from CastlingRights namespace)
//   .epSquare        (eSquare, NO_SQUARE when none)
//   .gameState       (GameStates enum)
//   .fiftyCount      (int, halfmove clock)
//   .fullMoveCount   (int)

// CastlingRights flags:
//   WHITE_KINGSIDE=1, WHITE_QUEENSIDE=2, WHITE_BOTH=3
//   BLACK_KINGSIDE=4, BLACK_QUEENSIDE=8, BLACK_BOTH=12, ALL=15
```

---

## Task 1: Create `BoardMoveTests.cpp` (9 cases, `[board_moves]`)

**Files:**
- Create: `StratChessTests/BoardMoveTests.cpp`
- Modify: `StratChessTests/StratChessTests.vcxproj`
- Modify: `StratChessTests/StratChessTests.vcxproj.filters`

- [ ] **Step 1: Create `StratChessTests/BoardMoveTests.cpp`**

```cpp
// BoardMoveTests.cpp — Catch2 test suite for Board DoMove/UndoMove coverage
// of move types not covered in BoardTests.cpp:
//   queenside castling (white + black), black kingside castling, normal capture
//   (white and black), double pawn push, under-promotions (knight, rook),
//   and capture-promotion round-trip.

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "MoveFactory.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "defines.h"

// ── FEN constants ─────────────────────────────────────────────────────────────

// Kings and rooks with full castling rights, white to move
static constexpr const char* FEN_CASTLING_W =
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// Same position, black to move (for black castling tests)
static constexpr const char* FEN_CASTLING_B =
    "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1";

// White queen d1, black rook c1, kings on e1/e8 — white captures
static constexpr const char* FEN_CAPTURE_W =
    "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// Black rook e5, white rook e4, kings on h8/h1 — black captures
static constexpr const char* FEN_CAPTURE_B =
    "7k/8/8/4r3/4R3/8/8/7K b - - 0 1";

// White pawn e2, kings on e1/e8 — double pawn push
static constexpr const char* FEN_DPUSH =
    "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1";

// White pawn c7, kings on e1/e8, c8 empty — under-promotions
static constexpr const char* FEN_UPROMO =
    "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";

// White pawn c7, black rook b8, kings on e1/h1 — capture-promotion
static constexpr const char* FEN_CAP_PROMO =
    "1r6/2P5/8/8/8/8/8/4K2k w - - 0 1";

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("Board - White queenside castling moves king to c1 and rook to d1; UndoMove restores both", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CASTLING_W);

    REQUIRE(board.GetPiece(e1) == WHITE_KING);
    REQUIRE(board.GetPiece(a1) == WHITE_ROOK);
    REQUIRE(board.GetPiece(c1) == NO_PIECE);
    REQUIRE(board.GetPiece(d1) == NO_PIECE);

    auto castle = MoveFactory::MakeMove(e1, c1, MoveType::QUEEN_CASTLE);
    REQUIRE(board.DoMove(castle));

    CHECK(board.GetPiece(c1) == WHITE_KING);
    CHECK(board.GetPiece(d1) == WHITE_ROOK);
    CHECK(board.GetPiece(e1) == NO_PIECE);
    CHECK(board.GetPiece(a1) == NO_PIECE);

    board.UndoMove(castle);

    CHECK(board.GetPiece(e1) == WHITE_KING);
    CHECK(board.GetPiece(a1) == WHITE_ROOK);
    CHECK(board.GetPiece(c1) == NO_PIECE);
    CHECK(board.GetPiece(d1) == NO_PIECE);
}

TEST_CASE("Board - Black kingside castling moves king to g8 and rook to f8; UndoMove restores both", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CASTLING_B);

    REQUIRE(board.GetPiece(e8) == BLACK_KING);
    REQUIRE(board.GetPiece(h8) == BLACK_ROOK);
    REQUIRE(board.GetPiece(g8) == NO_PIECE);
    REQUIRE(board.GetPiece(f8) == NO_PIECE);

    auto castle = MoveFactory::MakeMove(e8, g8, MoveType::KING_CASTLE);
    REQUIRE(board.DoMove(castle));

    CHECK(board.GetPiece(g8) == BLACK_KING);
    CHECK(board.GetPiece(f8) == BLACK_ROOK);
    CHECK(board.GetPiece(e8) == NO_PIECE);
    CHECK(board.GetPiece(h8) == NO_PIECE);

    board.UndoMove(castle);

    CHECK(board.GetPiece(e8) == BLACK_KING);
    CHECK(board.GetPiece(h8) == BLACK_ROOK);
    CHECK(board.GetPiece(g8) == NO_PIECE);
    CHECK(board.GetPiece(f8) == NO_PIECE);
}

TEST_CASE("Board - Black queenside castling moves king to c8 and rook to d8; UndoMove restores both", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CASTLING_B);

    REQUIRE(board.GetPiece(e8) == BLACK_KING);
    REQUIRE(board.GetPiece(a8) == BLACK_ROOK);
    REQUIRE(board.GetPiece(c8) == NO_PIECE);
    REQUIRE(board.GetPiece(d8) == NO_PIECE);

    auto castle = MoveFactory::MakeMove(e8, c8, MoveType::QUEEN_CASTLE);
    REQUIRE(board.DoMove(castle));

    CHECK(board.GetPiece(c8) == BLACK_KING);
    CHECK(board.GetPiece(d8) == BLACK_ROOK);
    CHECK(board.GetPiece(e8) == NO_PIECE);
    CHECK(board.GetPiece(a8) == NO_PIECE);

    board.UndoMove(castle);

    CHECK(board.GetPiece(e8) == BLACK_KING);
    CHECK(board.GetPiece(a8) == BLACK_ROOK);
    CHECK(board.GetPiece(c8) == NO_PIECE);
    CHECK(board.GetPiece(d8) == NO_PIECE);
}

TEST_CASE("Board - Normal capture (white takes black): captured piece removed; UndoMove restores it", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CAPTURE_W);

    REQUIRE(board.GetPiece(d1) == WHITE_QUEEN);
    REQUIRE(board.GetPiece(c1) == BLACK_ROOK);

    auto cap = MoveFactory::MakeCapture(d1, c1);
    REQUIRE(board.DoMove(cap));

    CHECK(board.GetPiece(c1) == WHITE_QUEEN);
    CHECK(board.GetPiece(d1) == NO_PIECE);

    board.UndoMove(cap);

    CHECK(board.GetPiece(d1) == WHITE_QUEEN);
    CHECK(board.GetPiece(c1) == BLACK_ROOK);
}

TEST_CASE("Board - Normal capture (black takes white): captured piece removed; UndoMove restores it", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CAPTURE_B);

    REQUIRE(board.GetPiece(e5) == BLACK_ROOK);
    REQUIRE(board.GetPiece(e4) == WHITE_ROOK);

    auto cap = MoveFactory::MakeCapture(e5, e4);
    REQUIRE(board.DoMove(cap));

    CHECK(board.GetPiece(e4) == BLACK_ROOK);
    CHECK(board.GetPiece(e5) == NO_PIECE);

    board.UndoMove(cap);

    CHECK(board.GetPiece(e5) == BLACK_ROOK);
    CHECK(board.GetPiece(e4) == WHITE_ROOK);
}

TEST_CASE("Board - Double pawn push moves pawn two squares; UndoMove restores", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_DPUSH);

    REQUIRE(board.GetPiece(e2) == WHITE_PAWN);
    REQUIRE(board.GetPiece(e4) == NO_PIECE);

    auto push = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
    REQUIRE(board.DoMove(push));

    CHECK(board.GetPiece(e4) == WHITE_PAWN);
    CHECK(board.GetPiece(e2) == NO_PIECE);

    board.UndoMove(push);

    CHECK(board.GetPiece(e2) == WHITE_PAWN);
    CHECK(board.GetPiece(e4) == NO_PIECE);
}

TEST_CASE("Board - Under-promotion to knight: knight on c8; UndoMove restores pawn on c7", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_UPROMO);

    REQUIRE(board.GetPiece(c7) == WHITE_PAWN);
    REQUIRE(board.GetPiece(c8) == NO_PIECE);

    auto promo = MoveFactory::MakePromotion(c7, c8, WHITE_KNIGHT);
    REQUIRE(board.DoMove(promo));

    CHECK(board.GetPiece(c8) == WHITE_KNIGHT);
    CHECK(board.GetPiece(c7) == NO_PIECE);

    board.UndoMove(promo);

    CHECK(board.GetPiece(c7) == WHITE_PAWN);
    CHECK(board.GetPiece(c8) == NO_PIECE);
}

TEST_CASE("Board - Under-promotion to rook: rook on c8; UndoMove restores pawn on c7", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_UPROMO);

    REQUIRE(board.GetPiece(c7) == WHITE_PAWN);
    REQUIRE(board.GetPiece(c8) == NO_PIECE);

    auto promo = MoveFactory::MakePromotion(c7, c8, WHITE_ROOK);
    REQUIRE(board.DoMove(promo));

    CHECK(board.GetPiece(c8) == WHITE_ROOK);
    CHECK(board.GetPiece(c7) == NO_PIECE);

    board.UndoMove(promo);

    CHECK(board.GetPiece(c7) == WHITE_PAWN);
    CHECK(board.GetPiece(c8) == NO_PIECE);
}

TEST_CASE("Board - Capture-promotion: white queen on b8, black rook gone; UndoMove restores both", "[board_moves]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CAP_PROMO);

    REQUIRE(board.GetPiece(c7) == WHITE_PAWN);
    REQUIRE(board.GetPiece(b8) == BLACK_ROOK);

    // isCapture=true selects PROMOTION_QUEEN_CAPTURE variant
    auto promo = MoveFactory::MakePromotion(c7, b8, WHITE_QUEEN, /*isCapture=*/true);
    REQUIRE(board.DoMove(promo));

    CHECK(board.GetPiece(b8) == WHITE_QUEEN);
    CHECK(board.GetPiece(c7) == NO_PIECE);

    board.UndoMove(promo);

    CHECK(board.GetPiece(c7) == WHITE_PAWN);
    CHECK(board.GetPiece(b8) == BLACK_ROOK);
}
```

- [ ] **Step 2: Register `BoardMoveTests.cpp` in `StratChessTests.vcxproj`**

In `StratChessTests/StratChessTests.vcxproj`, find the line:
```xml
    <ClCompile Include="BoardTests.cpp" />
```
Add immediately after it:
```xml
    <ClCompile Include="BoardMoveTests.cpp" />
```

- [ ] **Step 3: Register `BoardMoveTests.cpp` in `StratChessTests.vcxproj.filters`**

In `StratChessTests/StratChessTests.vcxproj.filters`, find the block:
```xml
    <ClCompile Include="BoardTests.cpp">
      <Filter>Source Files\Tests</Filter>
    </ClCompile>
```
Add immediately after it:
```xml
    <ClCompile Include="BoardMoveTests.cpp">
      <Filter>Source Files\Tests</Filter>
    </ClCompile>
```

- [ ] **Step 4: Build the test project**

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"
```

Expected: build succeeds, all existing tests still pass, `BoardMoveTests.cpp` compiles without warnings.

- [ ] **Step 5: Run the new tag in isolation and verify all 9 pass**

```
StratChessTests/x64/Release/StratChessTests.exe [board_moves]
```

Expected output (9 cases, all passing):
```
===============================================================================
All tests passed (X assertions in 9 test cases)
```

- [ ] **Step 6: Commit**

```
git add StratChessTests/BoardMoveTests.cpp StratChessTests/StratChessTests.vcxproj StratChessTests/StratChessTests.vcxproj.filters
git commit -m "test(board): add [board_moves] — missing DoMove/UndoMove round-trips

Covers queenside castling (white + black), black kingside castling,
normal capture (both colors), double pawn push, under-promotions
(knight, rook), and capture-promotion round-trip. 9 test cases.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 2: Create `BoardStateTests.cpp` (12 cases, `[board_state]`)

**Files:**
- Create: `StratChessTests/BoardStateTests.cpp`
- Modify: `StratChessTests/StratChessTests.vcxproj`
- Modify: `StratChessTests/StratChessTests.vcxproj.filters`

- [ ] **Step 1: Create `StratChessTests/BoardStateTests.cpp`**

```cpp
// BoardStateTests.cpp — Catch2 test suite for Board GameInfo state lifecycle.
//
// Verifies that castling rights, EP square, game state, fifty-move counter,
// material scores, and side-to-move are correctly updated by DoMove and fully
// restored by UndoMove. These are the fields De-Singleton Board will carry
// per-thread, so correctness here is critical before that refactor.

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "GameState.h"
#include "MoveFactory.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "defines.h"

// ── FEN constants ─────────────────────────────────────────────────────────────

// Full castling rights; white to move
static constexpr const char* FEN_FULL_RIGHTS =
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// White rook h7 can capture black rook h8 — strips BLACK_KINGSIDE rights
// Castling rights: only k (BLACK_KINGSIDE)
static constexpr const char* FEN_ROOK_CAPTURE =
    "4k2r/7R/8/8/8/8/8/4K3 w k - 0 1";

// White pawn e2 for EP and double-push tests; white to move
static constexpr const char* FEN_EP_SETUP =
    "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1";

// Quiet rook for side-to-move and fifty-count tests (fiftyCount=0)
static constexpr const char* FEN_QUIET =
    "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

// White queen d1, black rook c1, kings on e1/e8 — for material capture test
// White material: queen(900) + king(10000) = 10900
// Black material: rook(500) + king(10000) = 10500
static constexpr const char* FEN_MATERIAL_CAPTURE =
    "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// White pawn c7, kings on e1/e8 — for material promotion test
// White material before: pawn(100) + king(10000) = 10100
// White material after queen promo: queen(900) + king(10000) = 10900 (delta +800)
static constexpr const char* FEN_MATERIAL_PROMO =
    "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";

// ── Castling rights ───────────────────────────────────────────────────────────

TEST_CASE("Board - Castling rights stripped after king move; UndoMove restores them", "[board_state]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_FULL_RIGHTS);

    // Confirm full rights before move
    REQUIRE((board.GetGameInfo().castlingRights & CastlingRights::WHITE_BOTH) == CastlingRights::WHITE_BOTH);

    // White king e1 → e2 (quiet move, not a castling)
    auto m = MoveFactory::MakeQuiet(e1, e2);
    REQUIRE(board.DoMove(m));

    // Both white castling rights must be gone
    CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_BOTH) == CastlingRights::NONE);
    // Black rights must be untouched
    CHECK((board.GetGameInfo().castlingRights & CastlingRights::BLACK_BOTH) == CastlingRights::BLACK_BOTH);

    board.UndoMove(m);

    CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_BOTH) == CastlingRights::WHITE_BOTH);
}

TEST_CASE("Board - Queenside castling right stripped after rook moves; kingside right intact; UndoMove restores", "[board_state]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_FULL_RIGHTS);

    REQUIRE((board.GetGameInfo().castlingRights & CastlingRights::WHITE_QUEENSIDE) != 0);
    REQUIRE((board.GetGameInfo().castlingRights & CastlingRights::WHITE_KINGSIDE)  != 0);

    // White rook a1 → a3 (quiet, clears WHITE_QUEENSIDE only)
    auto m = MoveFactory::MakeQuiet(a1, a3);
    REQUIRE(board.DoMove(m));

    CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_QUEENSIDE) == 0);
    CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_KINGSIDE)  != 0);

    board.UndoMove(m);

    CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_QUEENSIDE) != 0);
    CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_KINGSIDE)  != 0);
}

TEST_CASE("Board - BLACK_KINGSIDE right stripped when rook h8 is captured; UndoMove restores", "[board_state]")
{
    Board& board = Board::Instance();
    // FEN: white rook h7, black rook h8, kings on e1/e8; only k right
    board.SetupFromFEN(FEN_ROOK_CAPTURE);

    REQUIRE((board.GetGameInfo().castlingRights & CastlingRights::BLACK_KINGSIDE) != 0);

    // White rook h7 × h8 (captures black rook on its starting square)
    auto m = MoveFactory::MakeCapture(h7, h8);
    REQUIRE(board.DoMove(m));

    CHECK((board.GetGameInfo().castlingRights & CastlingRights::BLACK_KINGSIDE) == 0);

    board.UndoMove(m);

    CHECK((board.GetGameInfo().castlingRights & CastlingRights::BLACK_KINGSIDE) != 0);
}

// ── En-passant square ─────────────────────────────────────────────────────────

TEST_CASE("Board - EP square set to e3 after white double pawn push e2-e4", "[board_state]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP_SETUP);

    REQUIRE(board.GetGameInfo().epSquare == NO_SQUARE);

    auto push = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
    REQUIRE(board.DoMove(push));

    CHECK(board.GetGameInfo().epSquare == e3);

    board.UndoMove(push);
}

TEST_CASE("Board - EP square cleared after a non-EP follow-up move", "[board_state]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP_SETUP);

    // White double push sets EP square
    auto push = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
    REQUIRE(board.DoMove(push));
    REQUIRE(board.GetGameInfo().epSquare == e3);

    // Black king e8 → d8 (quiet; clears the EP square for this side)
    auto king_move = MoveFactory::MakeQuiet(e8, d8);
    REQUIRE(board.DoMove(king_move));

    CHECK(board.GetGameInfo().epSquare == NO_SQUARE);

    board.UndoMove(king_move);
    board.UndoMove(push);
}

TEST_CASE("Board - EP square restored to NO_SQUARE after UndoMove of double push", "[board_state]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP_SETUP);

    auto push = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
    REQUIRE(board.DoMove(push));
    REQUIRE(board.GetGameInfo().epSquare == e3); // sanity

    board.UndoMove(push);

    CHECK(board.GetGameInfo().epSquare == NO_SQUARE);
}

// ── Side-to-move ──────────────────────────────────────────────────────────────

TEST_CASE("Board - Side-to-move flips to BLACK after DoMove; restores to WHITE after UndoMove", "[board_state]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_QUIET); // white to move

    REQUIRE(board.GetCurrentColor() == WHITE);

    auto m = MoveFactory::MakeQuiet(a1, a2);
    REQUIRE(board.DoMove(m));

    CHECK(board.GetCurrentColor() == BLACK);

    board.UndoMove(m);

    CHECK(board.GetCurrentColor() == WHITE);
}

// ── Material scores ───────────────────────────────────────────────────────────

TEST_CASE("Board - Material score decremented for captured side; UndoMove restores", "[board_state]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_MATERIAL_CAPTURE);

    const int blackMatBefore = board.GetMaterialScore(BLACK); // 10500
    const int whiteMatBefore = board.GetMaterialScore(WHITE); // 10900

    // White queen d1 captures black rook c1 (rook value = 500)
    auto cap = MoveFactory::MakeCapture(d1, c1);
    REQUIRE(board.DoMove(cap));

    CHECK(board.GetMaterialScore(BLACK) == blackMatBefore - 500);
    CHECK(board.GetMaterialScore(WHITE) == whiteMatBefore); // queen moved, not lost

    board.UndoMove(cap);

    CHECK(board.GetMaterialScore(BLACK) == blackMatBefore);
    CHECK(board.GetMaterialScore(WHITE) == whiteMatBefore);
}

TEST_CASE("Board - Material score updated after promotion (pawn -> queen, delta +800); UndoMove restores", "[board_state]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_MATERIAL_PROMO);

    const int whiteMatBefore = board.GetMaterialScore(WHITE); // 10100 (pawn + king)

    // Pawn c7 → c8 = Queen (removes pawn +100, adds queen +900, net +800)
    auto promo = MoveFactory::MakePromotion(c7, c8, WHITE_QUEEN);
    REQUIRE(board.DoMove(promo));

    CHECK(board.GetMaterialScore(WHITE) == whiteMatBefore + 800);

    board.UndoMove(promo);

    CHECK(board.GetMaterialScore(WHITE) == whiteMatBefore);
}

// ── Fifty-move counter ────────────────────────────────────────────────────────

TEST_CASE("Board - Fifty-move counter resets to 0 on pawn move; UndoMove restores prior value", "[board_state]")
{
    Board& board = Board::Instance();
    // FEN halfmove clock field pre-sets fiftyCount to 10
    board.SetupFromFEN("4k3/8/8/8/8/8/4P3/4K3 w - - 10 1");

    REQUIRE(board.GetGameInfo().fiftyCount == 10);

    // Pawn move resets the counter
    auto pawn = MoveFactory::MakeQuiet(e2, e3);
    REQUIRE(board.DoMove(pawn));

    CHECK(board.GetGameInfo().fiftyCount == 0);

    board.UndoMove(pawn);

    CHECK(board.GetGameInfo().fiftyCount == 10);
}

TEST_CASE("Board - Fifty-move counter increments by 1 on quiet non-pawn move; UndoMove restores", "[board_state]")
{
    Board& board = Board::Instance();
    // FEN halfmove clock pre-set to 5
    board.SetupFromFEN("8/8/3k4/8/8/3K4/8/R7 w - - 5 1");

    REQUIRE(board.GetGameInfo().fiftyCount == 5);

    auto m = MoveFactory::MakeQuiet(a1, a2);
    REQUIRE(board.DoMove(m));

    CHECK(board.GetGameInfo().fiftyCount == 6);

    board.UndoMove(m);

    CHECK(board.GetGameInfo().fiftyCount == 5);
}

TEST_CASE("Board - DRAW_50_MOVES triggered when fiftyCount reaches 50; UndoMove restores STILL_PLAYING", "[board_state]")
{
    Board& board = Board::Instance();
    // FEN halfmove clock pre-set to 49 (one quiet move will reach 50)
    board.SetupFromFEN("8/8/3k4/8/8/3K4/8/R7 w - - 49 1");

    REQUIRE(board.GetGameInfo().fiftyCount == 49);
    REQUIRE(board.GetGameInfo().gameState == GameStates::STILL_PLAYING);

    auto m = MoveFactory::MakeQuiet(a1, a2);
    REQUIRE(board.DoMove(m));

    CHECK(board.GetGameInfo().gameState == GameStates::DRAW_50_MOVES);

    board.UndoMove(m);

    CHECK(board.GetGameInfo().gameState == GameStates::STILL_PLAYING);
    CHECK(board.GetGameInfo().fiftyCount == 49);
}
```

- [ ] **Step 2: Register in `StratChessTests.vcxproj`**

Find:
```xml
    <ClCompile Include="BoardMoveTests.cpp" />
```
Add immediately after:
```xml
    <ClCompile Include="BoardStateTests.cpp" />
```

- [ ] **Step 3: Register in `StratChessTests.vcxproj.filters`**

Find:
```xml
    <ClCompile Include="BoardMoveTests.cpp">
      <Filter>Source Files\Tests</Filter>
    </ClCompile>
```
Add immediately after:
```xml
    <ClCompile Include="BoardStateTests.cpp">
      <Filter>Source Files\Tests</Filter>
    </ClCompile>
```

- [ ] **Step 4: Build**

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"
```

Expected: clean build, all prior tests still pass.

- [ ] **Step 5: Run the new tag in isolation and verify all 12 pass**

```
StratChessTests/x64/Release/StratChessTests.exe [board_state]
```

Expected:
```
===============================================================================
All tests passed (X assertions in 12 test cases)
```

- [ ] **Step 6: Commit**

```
git add StratChessTests/BoardStateTests.cpp StratChessTests/StratChessTests.vcxproj StratChessTests/StratChessTests.vcxproj.filters
git commit -m "test(board): add [board_state] — GameInfo lifecycle tests

Covers castling rights (king move, rook move, rook capture), EP square
lifecycle (set/cleared/restored), side-to-move flip, material score after
capture/promotion, and fifty-move counter including draw trigger.
12 test cases.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 3: Create `BoardApiTests.cpp` (11 cases, `[board_api]`)

**Files:**
- Create: `StratChessTests/BoardApiTests.cpp`
- Modify: `StratChessTests/StratChessTests.vcxproj`
- Modify: `StratChessTests/StratChessTests.vcxproj.filters`

- [ ] **Step 1: Create `StratChessTests/BoardApiTests.cpp`**

```cpp
// BoardApiTests.cpp — Catch2 test suite for Board public query APIs and
// FEN round-trip serialization.
//
// Covers GetCapturedPiece, GetEffectiveMovPiece (both must be called BEFORE
// DoMove — the API contract documented in Board.h), and ExtractFEN round-trips
// for all FEN fields (side-to-move, castling, EP square, halfmove clock,
// fullmove number).

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "MoveFactory.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "defines.h"
#include <string>

// ── FEN constants ─────────────────────────────────────────────────────────────

// Quiet rook position — no captures possible
static constexpr const char* FEN_QUIET =
    "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

// White queen d1, black rook c1, kings on e1/e8
static constexpr const char* FEN_CAPTURE =
    "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// White pawn d5, black pawn e5, EP square e6 (black just pushed e7-e5)
static constexpr const char* FEN_EP =
    "8/8/8/3Pp3/8/8/8/4K2k w - e6 0 1";

// White pawn c7, kings on e1/e8 — promotion API test
static constexpr const char* FEN_PROMO =
    "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";

// Standard starting position
static constexpr const char* FEN_START =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Active EP square — black pushed d7-d5; white pawn on e5, black pawn on d5.
// EP square d6 available for white to capture. White to move.
static constexpr const char* FEN_EP_ACTIVE =
    "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1";

// Partial castling rights: white can castle kingside (K), black queenside (q)
static constexpr const char* FEN_PARTIAL_RIGHTS =
    "r3k3/8/8/8/8/8/8/4K2R w Kq - 0 1";

// Black to move, halfmove=17, fullmove=34 — tests all numeric/side fields
static constexpr const char* FEN_BLACK_TO_MOVE =
    "4k3/8/8/8/8/8/8/R3K3 b - - 17 34";

// ── GetCapturedPiece ──────────────────────────────────────────────────────────

TEST_CASE("Board::GetCapturedPiece returns NO_PIECE for a quiet move", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_QUIET);

    auto m = MoveFactory::MakeQuiet(a1, h1);
    // Must be called before DoMove — the API contract
    REQUIRE(board.GetCapturedPiece(m) == NO_PIECE);
}

TEST_CASE("Board::GetCapturedPiece returns BLACK_ROOK for white queen captures black rook", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CAPTURE);

    auto m = MoveFactory::MakeCapture(d1, c1);
    REQUIRE(board.GetCapturedPiece(m) == BLACK_ROOK);
}

TEST_CASE("Board::GetCapturedPiece returns BLACK_PAWN for EP capture (pawn is not on destination square)", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP);

    // EP: white d5 captures to e6; the black pawn is on e5 (not e6)
    auto m = MoveFactory::MakeEnPassant(d5, e6);

    // Confirm the destination is empty (the API should NOT look at m.to())
    REQUIRE(board.GetPiece(e6) == NO_PIECE);
    REQUIRE(board.GetPiece(e5) == BLACK_PAWN);

    CHECK(board.GetCapturedPiece(m) == BLACK_PAWN);
}

// ── GetEffectiveMovPiece ──────────────────────────────────────────────────────

TEST_CASE("Board::GetEffectiveMovPiece returns the piece on from-square for a quiet move", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_QUIET);

    auto m = MoveFactory::MakeQuiet(a1, h1);
    REQUIRE(board.GetEffectiveMovPiece(m) == WHITE_ROOK);
}

TEST_CASE("Board::GetEffectiveMovPiece returns the promoted piece, not the pawn", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_PROMO);

    // Pawn is still on c7; GetEffectiveMovPiece must return WHITE_QUEEN (the promoted piece)
    auto m = MoveFactory::MakePromotion(c7, c8, WHITE_QUEEN);
    REQUIRE(board.GetEffectiveMovPiece(m) == WHITE_QUEEN);
}

// ── ExtractFEN round-trips ────────────────────────────────────────────────────

TEST_CASE("Board::ExtractFEN round-trips the standard starting position", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_START);
    CHECK(board.ExtractFEN() == std::string(FEN_START));
}

TEST_CASE("Board::ExtractFEN preserves an active EP square", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP_ACTIVE);
    const std::string fen = board.ExtractFEN();
    // The EP field is the 4th space-delimited token; must contain "d6"
    CHECK(fen.find("d6") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves partial castling rights (Kq)", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_PARTIAL_RIGHTS);
    const std::string fen = board.ExtractFEN();
    // Castling field must be exactly "Kq" (white kingside + black queenside only)
    CHECK(fen.find(" Kq ") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves black to move", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_BLACK_TO_MOVE);
    const std::string fen = board.ExtractFEN();
    // Active color field must be 'b'
    CHECK(fen.find(" b ") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves halfmove clock (17)", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_BLACK_TO_MOVE);
    CHECK(board.ExtractFEN() == std::string(FEN_BLACK_TO_MOVE));
}

TEST_CASE("Board::ExtractFEN preserves fullmove number (34)", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_BLACK_TO_MOVE);
    const std::string fen = board.ExtractFEN();
    // The fullmove number is the last space-delimited token
    CHECK(fen.substr(fen.rfind(' ') + 1) == "34");
}
```

- [ ] **Step 2: Register in `StratChessTests.vcxproj`**

Find:
```xml
    <ClCompile Include="BoardStateTests.cpp" />
```
Add immediately after:
```xml
    <ClCompile Include="BoardApiTests.cpp" />
```

- [ ] **Step 3: Register in `StratChessTests.vcxproj.filters`**

Find:
```xml
    <ClCompile Include="BoardStateTests.cpp">
      <Filter>Source Files\Tests</Filter>
    </ClCompile>
```
Add immediately after:
```xml
    <ClCompile Include="BoardApiTests.cpp">
      <Filter>Source Files\Tests</Filter>
    </ClCompile>
```

- [ ] **Step 4: Build**

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"
```

Expected: clean build, all prior tests still pass.

- [ ] **Step 5: Run the new tag in isolation and verify all 11 pass**

```
StratChessTests/x64/Release/StratChessTests.exe [board_api]
```

Expected:
```
===============================================================================
All tests passed (X assertions in 11 test cases)
```

- [ ] **Step 6: Commit**

```
git add StratChessTests/BoardApiTests.cpp StratChessTests/StratChessTests.vcxproj StratChessTests/StratChessTests.vcxproj.filters
git commit -m "test(board): add [board_api] — GetCapturedPiece, GetEffectiveMovPiece, ExtractFEN

GetCapturedPiece: quiet (NO_PIECE), capture (BLACK_ROOK), EP (BLACK_PAWN
not on destination). GetEffectiveMovPiece: quiet rook, promotion returns
promoted piece not pawn. ExtractFEN round-trips: starting pos, EP square,
partial castling, black to move, halfmove clock, fullmove number. 11 cases.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 4: Update `TestDesign.md` and `Roadmap.md`

**Files:**
- Modify: `Docs/TestDesign.md`
- Modify: `Docs/Roadmap.md`

- [ ] **Step 1: Add new tags to the run-by-tag section in `TestDesign.md`**

Find in `Docs/TestDesign.md` (inside the run-by-tag code block):
```
x64/Release/StratChessTests.exe [time_mgr]
```
Add after it:
```
x64/Release/StratChessTests.exe [board]
x64/Release/StratChessTests.exe [board_moves]
x64/Release/StratChessTests.exe [board_state]
x64/Release/StratChessTests.exe [board_api]
```

- [ ] **Step 2: Add three rows to the coverage map in `TestDesign.md`**

Find the row:
```markdown
| Board DoMove/UndoMove completeness | `[board]` | ✅ Phase 1 | `BoardTests.cpp` |
```
Add immediately after it:
```markdown
| Board move-type round-trips (all types) | `[board_moves]` | ✅ Phase 1 | `BoardMoveTests.cpp` |
| Board GameInfo state lifecycle | `[board_state]` | ✅ Phase 1 | `BoardStateTests.cpp` |
| Board public query APIs + FEN round-trip | `[board_api]` | ✅ Phase 1 | `BoardApiTests.cpp` |
```

- [ ] **Step 3: Update the `[board]` Phase 1 section note in `TestDesign.md`**

Find:
```markdown
**Status**: ✅ **Done.** Move layout Phases 3 & 4 landed in March 2026; all 5 cases (en passant, castling, promotion generation, promotion round-trip, Zobrist hash cycle) implemented and passing (6 test cases, 33 assertions).
```
Update to:
```markdown
**Status**: ✅ **Done.** Move layout Phases 3 & 4 landed in March 2026 (6 test cases, 33 assertions). Extended March 2026: `BoardMoveTests.cpp` (9 cases, `[board_moves]`), `BoardStateTests.cpp` (12 cases, `[board_state]`), `BoardApiTests.cpp` (11 cases, `[board_api]`) add full move-type, GameInfo state, and API coverage.
```

- [ ] **Step 4: Update `Roadmap.md` `[board]` item to remove the ⚠️ overdue note**

Find in `Docs/Roadmap.md`:
```markdown
  - `[board]` — `DoMove`/`UndoMove` completeness — ⚠️ **overdue**: Move layout Phases 3 & 4 already landed; do now, before De-Singleton
```
Replace with:
```markdown
  - `[board]` — `DoMove`/`UndoMove` completeness + full move-type, GameInfo, and API coverage — ✅ done (`BoardTests.cpp`, `BoardMoveTests.cpp`, `BoardStateTests.cpp`, `BoardApiTests.cpp`)
```

- [ ] **Step 5: Commit**

```
git add Docs/TestDesign.md Docs/Roadmap.md
git commit -m "docs: update TestDesign.md and Roadmap.md for expanded board test coverage

Add [board_moves], [board_state], [board_api] tags to coverage map and
run-by-tag reference. Remove stale 'overdue' warning from Roadmap [board] item.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 5: Final verification

- [ ] **Step 1: Run the complete fast suite**

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"
```

Expected: all tests pass. Count should be 338 (existing) + new assertions from 32 cases ≈ 370+.

- [ ] **Step 2: Run all four board tags together to confirm isolation**

```
StratChessTests/x64/Release/StratChessTests.exe [board],[board_moves],[board_state],[board_api]
```

Expected: 38 test cases, all passing.
