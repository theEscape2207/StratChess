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
static constexpr const char* FEN_FULL_RIGHTS = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// White rook h7 can capture black rook h8 — strips BLACK_KINGSIDE rights
// Castling rights: only k (BLACK_KINGSIDE)
static constexpr const char* FEN_ROOK_CAPTURE = "4k2r/7R/8/8/8/8/8/4K3 w k - 0 1";

// White pawn e2 for EP and double-push tests; white to move
static constexpr const char* FEN_EP_SETUP = "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1";

// White queen d1, black rook c1, kings on e1/e8 — for material capture test
// White material: queen(900) + king(10000) = 10900
// Black material: rook(500) + king(10000) = 10500
static constexpr const char* FEN_MATERIAL_CAPTURE = "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// White pawn c7, kings on e1/e8 — for material promotion test
// White material before: pawn(100) + king(10000) = 10100
// White material after queen promo: queen(900) + king(10000) = 10900 (delta +800)
static constexpr const char* FEN_MATERIAL_PROMO = "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";

// ── Castling rights ───────────────────────────────────────────────────────────

TEST_CASE("Board - Castling rights stripped after king move; UndoMove restores them",
          "[board_state]")
{
	Board board(FEN_FULL_RIGHTS);

	// Confirm full rights before move
	REQUIRE((board.GetGameInfo().castlingRights & CastlingRights::WHITE_BOTH) ==
	        CastlingRights::WHITE_BOTH);

	// White king e1 → e2 (quiet move, not a castling)
	auto m = MoveFactory::MakeQuiet(e1, e2);
	REQUIRE(board.DoMove(m));

	// Both white castling rights must be gone
	CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_BOTH) ==
	      CastlingRights::NONE);
	// Black rights must be untouched
	CHECK((board.GetGameInfo().castlingRights & CastlingRights::BLACK_BOTH) ==
	      CastlingRights::BLACK_BOTH);

	board.UndoMove(m);

	CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_BOTH) ==
	      CastlingRights::WHITE_BOTH);
}

TEST_CASE("Board - Queenside castling right stripped after rook moves; kingside right intact; "
          "UndoMove restores",
          "[board_state]")
{
	Board board(FEN_FULL_RIGHTS);

	REQUIRE((board.GetGameInfo().castlingRights & CastlingRights::WHITE_QUEENSIDE) != 0);
	REQUIRE((board.GetGameInfo().castlingRights & CastlingRights::WHITE_KINGSIDE) != 0);

	// White rook a1 → a3 (quiet, clears WHITE_QUEENSIDE only)
	auto m = MoveFactory::MakeQuiet(a1, a3);
	REQUIRE(board.DoMove(m));

	CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_QUEENSIDE) == 0);
	CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_KINGSIDE) != 0);

	board.UndoMove(m);

	CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_QUEENSIDE) != 0);
	CHECK((board.GetGameInfo().castlingRights & CastlingRights::WHITE_KINGSIDE) != 0);
}

TEST_CASE("Board - BLACK_KINGSIDE right stripped when rook h8 is captured; UndoMove restores",
          "[board_state]")
{
	// FEN: white rook h7, black rook h8, kings on e1/e8; only k right
	Board board(FEN_ROOK_CAPTURE);

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
	Board board(FEN_EP_SETUP);

	REQUIRE(board.GetGameInfo().epSquare == NO_SQUARE);

	auto push = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
	REQUIRE(board.DoMove(push));

	CHECK(board.GetGameInfo().epSquare == e3);

	board.UndoMove(push);
}

TEST_CASE("Board - EP square cleared after a non-EP follow-up move", "[board_state]")
{
	Board board(FEN_EP_SETUP);

	// White double push sets EP square
	auto push = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
	REQUIRE(board.DoMove(push));
	REQUIRE(board.GetGameInfo().epSquare == e3);

	// Black king e8 → d8 (quiet; clears the EP square)
	auto king_move = MoveFactory::MakeQuiet(e8, d8);
	REQUIRE(board.DoMove(king_move));

	CHECK(board.GetGameInfo().epSquare == NO_SQUARE);

	board.UndoMove(king_move);
	board.UndoMove(push);
}

TEST_CASE("Board - EP square restored to NO_SQUARE after UndoMove of double push", "[board_state]")
{
	Board board(FEN_EP_SETUP);

	auto push = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
	REQUIRE(board.DoMove(push));
	REQUIRE(board.GetGameInfo().epSquare == e3); // sanity

	board.UndoMove(push);

	CHECK(board.GetGameInfo().epSquare == NO_SQUARE);
}

// ── Side-to-move ──────────────────────────────────────────────────────────────

TEST_CASE("Board - Side-to-move flips to BLACK after DoMove; restores to WHITE after UndoMove",
          "[board_state]")
{
	Board board("8/8/3k4/8/8/3K4/8/R7 w - - 0 1"); // white to move

	REQUIRE(board.GetCurrentColor() == WHITE);

	auto m = MoveFactory::MakeQuiet(a1, a2);
	REQUIRE(board.DoMove(m));

	CHECK(board.GetCurrentColor() == BLACK);

	board.UndoMove(m);

	CHECK(board.GetCurrentColor() == WHITE);
}

// ── Material scores ───────────────────────────────────────────────────────────

TEST_CASE("Board - Material score decremented for captured side; UndoMove restores",
          "[board_state]")
{
	Board board(FEN_MATERIAL_CAPTURE);

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

TEST_CASE(
    "Board - Material score updated after promotion (pawn -> queen, delta +800); UndoMove restores",
    "[board_state]")
{
	Board board(FEN_MATERIAL_PROMO);

	const int whiteMatBefore = board.GetMaterialScore(WHITE); // 10100 (pawn + king)

	// Pawn c7 → c8 = Queen (removes pawn +100, adds queen +900, net +800)
	auto promo = MoveFactory::MakePromotion(c7, c8, WHITE_QUEEN);
	REQUIRE(board.DoMove(promo));

	CHECK(board.GetMaterialScore(WHITE) == whiteMatBefore + 800);

	board.UndoMove(promo);

	CHECK(board.GetMaterialScore(WHITE) == whiteMatBefore);
}

// ── Fifty-move counter ────────────────────────────────────────────────────────

TEST_CASE("Board - Fifty-move counter resets to 0 on pawn move; UndoMove restores prior value",
          "[board_state]")
{
	// FEN halfmove clock field pre-sets fiftyCount to 10
	Board board("4k3/8/8/8/8/8/4P3/4K3 w - - 10 1");

	REQUIRE(board.GetGameInfo().fiftyCount == 10);

	// Pawn move resets the counter
	auto pawn = MoveFactory::MakeQuiet(e2, e3);
	REQUIRE(board.DoMove(pawn));

	CHECK(board.GetGameInfo().fiftyCount == 0);

	board.UndoMove(pawn);

	CHECK(board.GetGameInfo().fiftyCount == 10);
}

TEST_CASE("Board - Fifty-move counter increments by 1 on quiet non-pawn move; UndoMove restores",
          "[board_state]")
{
	// FEN halfmove clock pre-set to 5
	Board board("8/8/3k4/8/8/3K4/8/R7 w - - 5 1");

	REQUIRE(board.GetGameInfo().fiftyCount == 5);

	auto m = MoveFactory::MakeQuiet(a1, a2);
	REQUIRE(board.DoMove(m));

	CHECK(board.GetGameInfo().fiftyCount == 6);

	board.UndoMove(m);

	CHECK(board.GetGameInfo().fiftyCount == 5);
}

TEST_CASE("Board - fiftyCount reaches 50 after quiet move from 49; UndoMove restores to 49",
          "[board_state]")
{
	// Note: DRAW_50_MOVES game state is set by the game-loop layer (PlayerAI/PlayerHuman
	// via UpdateBoardInfo), not by Board::DoMove. Board only tracks the counter.
	// FEN halfmove clock pre-set to 49 (one quiet move will reach 50)
	Board board("8/8/3k4/8/8/3K4/8/R7 w - - 49 1");

	REQUIRE(board.GetGameInfo().fiftyCount == 49);

	auto m = MoveFactory::MakeQuiet(a1, a2);
	REQUIRE(board.DoMove(m));

	CHECK(board.GetGameInfo().fiftyCount == 50);

	board.UndoMove(m);

	CHECK(board.GetGameInfo().fiftyCount == 49);
}
