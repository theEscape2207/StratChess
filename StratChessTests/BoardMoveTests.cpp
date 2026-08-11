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
static constexpr const char* FEN_CASTLING_W = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// Same position, black to move (for black castling tests)
static constexpr const char* FEN_CASTLING_B = "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1";

// White queen d1, black rook c1, kings on e1/e8 — white captures
static constexpr const char* FEN_CAPTURE_W = "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// Black rook e5, white rook e4, kings on h8/h1 — black captures
static constexpr const char* FEN_CAPTURE_B = "7k/8/8/4r3/4R3/8/8/7K b - - 0 1";

// White pawn e2, kings on e1/e8 — double pawn push
static constexpr const char* FEN_DPUSH = "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1";

// White pawn c7, kings on e1/e8, c8 empty — under-promotions
static constexpr const char* FEN_UPROMO = "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";

// White pawn c7, black rook b8, kings on e1/h1 — capture-promotion
static constexpr const char* FEN_CAP_PROMO = "1r6/2P5/8/8/8/8/8/4K2k w - - 0 1";

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE(
    "Board - White queenside castling moves king to c1 and rook to d1; UndoMove restores both",
    "[board_moves]")
{
	Board board(FEN_CASTLING_W);

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

TEST_CASE("Board - Black kingside castling moves king to g8 and rook to f8; UndoMove restores both",
          "[board_moves]")
{
	Board board(FEN_CASTLING_B);

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

TEST_CASE(
    "Board - Black queenside castling moves king to c8 and rook to d8; UndoMove restores both",
    "[board_moves]")
{
	Board board(FEN_CASTLING_B);

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

TEST_CASE(
    "Board - Normal capture (white takes black): captured piece removed; UndoMove restores it",
    "[board_moves]")
{
	Board board(FEN_CAPTURE_W);

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

TEST_CASE(
    "Board - Normal capture (black takes white): captured piece removed; UndoMove restores it",
    "[board_moves]")
{
	Board board(FEN_CAPTURE_B);

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
	Board board(FEN_DPUSH);

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

TEST_CASE("Board - Under-promotion to knight: knight on c8; UndoMove restores pawn on c7",
          "[board_moves]")
{
	Board board(FEN_UPROMO);

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

TEST_CASE("Board - Under-promotion to rook: rook on c8; UndoMove restores pawn on c7",
          "[board_moves]")
{
	Board board(FEN_UPROMO);

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

TEST_CASE("Board - Capture-promotion: white queen on b8, black rook gone; UndoMove restores both",
          "[board_moves]")
{
	Board board(FEN_CAP_PROMO);

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
