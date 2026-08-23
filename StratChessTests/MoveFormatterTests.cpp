// MoveFormatterTests.cpp — Catch2 test suite for MoveFormatter
//
// Tests ToUCI, ToShort, ToVerbose, and FromUCI.
// All ToShort / ToVerbose tests set up the board in the POST-DoMove state via FEN,
// then call the formatter with a Move whose from/to/flags match the played move.
// This mirrors the production call site in Game::PrintBoardAndMove.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveFactory.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "defines.h"

// ── ToUCI ─────────────────────────────────────────────────────────────────────
// ToUCI needs no board context; it converts from/to squares and promotion flags.

TEST_CASE("MoveFormatter::ToUCI - quiet and capture moves", "[formatter]")
{
	SECTION("Quiet pawn push e2-e4")
	{
		auto m = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
		CHECK(MoveFormatter::ToUCI(m) == "e2e4");
	}
	SECTION("Quiet knight move g1-f3")
	{
		auto m = MoveFactory::MakeQuiet(g1, f3);
		CHECK(MoveFormatter::ToUCI(m) == "g1f3");
	}
	SECTION("Capture c5xd6")
	{
		auto m = MoveFactory::MakeCapture(c5, d6);
		CHECK(MoveFormatter::ToUCI(m) == "c5d6");
	}
	SECTION("En passant f5xe6")
	{
		auto m = MoveFactory::MakeEnPassant(f5, e6);
		CHECK(MoveFormatter::ToUCI(m) == "f5e6");
	}
	SECTION("Kingside castle e1-g1")
	{
		auto m = MoveFactory::MakeMove(e1, g1, MoveType::KING_CASTLE);
		CHECK(MoveFormatter::ToUCI(m) == "e1g1");
	}
	SECTION("Queenside castle e1-c1")
	{
		auto m = MoveFactory::MakeMove(e1, c1, MoveType::QUEEN_CASTLE);
		CHECK(MoveFormatter::ToUCI(m) == "e1c1");
	}
	SECTION("Black kingside castle e8-g8")
	{
		auto m = MoveFactory::MakeMove(e8, g8, MoveType::KING_CASTLE);
		CHECK(MoveFormatter::ToUCI(m) == "e8g8");
	}
	SECTION("Black queenside castle e8-c8")
	{
		auto m = MoveFactory::MakeMove(e8, c8, MoveType::QUEEN_CASTLE);
		CHECK(MoveFormatter::ToUCI(m) == "e8c8");
	}
}

TEST_CASE("MoveFormatter::ToUCI - all promotion types (quiet and capture)", "[formatter]")
{
	// Quiet promotions
	SECTION("White promotes to queen b7-b8")
	{
		auto m = MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false);
		CHECK(MoveFormatter::ToUCI(m) == "b7b8q");
	}
	SECTION("White promotes to rook b7-b8")
	{
		auto m = MoveFactory::MakePromotion(b7, b8, WHITE_ROOK, false);
		CHECK(MoveFormatter::ToUCI(m) == "b7b8r");
	}
	SECTION("White promotes to bishop b7-b8")
	{
		auto m = MoveFactory::MakePromotion(b7, b8, WHITE_BISHOP, false);
		CHECK(MoveFormatter::ToUCI(m) == "b7b8b");
	}
	SECTION("White promotes to knight b7-b8")
	{
		auto m = MoveFactory::MakePromotion(b7, b8, WHITE_KNIGHT, false);
		CHECK(MoveFormatter::ToUCI(m) == "b7b8n");
	}
	// Promotion-captures (the bug in the old move_to_string — these had no suffix)
	SECTION("White promotes to queen with capture b7xa8")
	{
		auto m = MoveFactory::MakePromotion(b7, a8, WHITE_QUEEN, true);
		CHECK(MoveFormatter::ToUCI(m) == "b7a8q");
	}
	SECTION("White promotes to rook with capture b7xa8")
	{
		auto m = MoveFactory::MakePromotion(b7, a8, WHITE_ROOK, true);
		CHECK(MoveFormatter::ToUCI(m) == "b7a8r");
	}
	SECTION("White promotes to bishop with capture b7xa8")
	{
		auto m = MoveFactory::MakePromotion(b7, a8, WHITE_BISHOP, true);
		CHECK(MoveFormatter::ToUCI(m) == "b7a8b");
	}
	SECTION("White promotes to knight with capture b7xa8")
	{
		auto m = MoveFactory::MakePromotion(b7, a8, WHITE_KNIGHT, true);
		CHECK(MoveFormatter::ToUCI(m) == "b7a8n");
	}
	// Black promotions
	SECTION("Black promotes to queen b2-b1")
	{
		auto m = MoveFactory::MakePromotion(b2, b1, BLACK_QUEEN, false);
		CHECK(MoveFormatter::ToUCI(m) == "b2b1q");
	}
	SECTION("Black promotes to knight with capture b2xa1")
	{
		auto m = MoveFactory::MakePromotion(b2, a1, BLACK_KNIGHT, true);
		CHECK(MoveFormatter::ToUCI(m) == "b2a1n");
	}
}

// ── ToShort ───────────────────────────────────────────────────────────────────
// Board is set to the POST-DoMove state via FEN.

TEST_CASE("MoveFormatter::ToShort - basic moves", "[formatter]")
{
	Board board;

	SECTION("White pawn push e2-e4 — no check")
	{
		// Starting position after 1.e4 (white pawn now on e4, black to move, no check)
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1"));
		auto m = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
		CHECK(MoveFormatter::ToShort(m, board) == "Pe2-e4");
	}
	SECTION("White queen capture — no check")
	{
		// White queen on d1 has just captured on d7; black king on h8 (not attacked by Qd7)
		// FEN is the POST-DoMove state: queen sits on d7, black to move, no check.
		// Rank-7 layout: pppQppp1 = a7=p, b7=p, c7=p, d7=Q, e7=p, f7=p, g7=p, h7=empty
		REQUIRE(board.SetupFromFEN("r1b4k/pppQppp1/2n5/8/8/8/PPPP1PPP/RNB1K1NR b - - 0 1"));
		auto m = MoveFactory::MakeCapture(d1, d7);
		CHECK(MoveFormatter::ToShort(m, board) == "Qd1xd7");
	}
	SECTION("White rook gives check")
	{
		// White rook on g7, black king on g8 (in check), white king on h1
		// The move Rg1xg7 was just played (rook captured on g7, gives check to g8)
		REQUIRE(board.SetupFromFEN("6k1/6R1/8/8/8/8/8/7K b - - 0 1"));
		auto m = MoveFactory::MakeCapture(g1, g7);
		CHECK(MoveFormatter::ToShort(m, board) == "Rg1xg7+");
	}
	SECTION("White kingside castle — no check")
	{
		// After 0-0: white king on g1, white rook on f1; black to move, no check
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQ1RK1 b kq - 1 1"));
		auto m = MoveFactory::MakeMove(e1, g1, MoveType::KING_CASTLE);
		CHECK(MoveFormatter::ToShort(m, board) == "0-0");
	}
	SECTION("White queenside castle — no check")
	{
		// After 0-0-0: white king on c1, white rook on d1; black to move, no check
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/2KR1BNR b kq - 1 1"));
		auto m = MoveFactory::MakeMove(e1, c1, MoveType::QUEEN_CASTLE);
		CHECK(MoveFormatter::ToShort(m, board) == "0-0-0");
	}
	SECTION("White pawn promotes to queen — no check")
	{
		// White pawn just promoted to queen on b8; black king on a6, no check
		REQUIRE(board.SetupFromFEN("1Q6/8/k7/8/8/8/8/7K b - - 0 1"));
		auto m = MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false);
		CHECK(MoveFormatter::ToShort(m, board) == "Pb7-b8Q");
	}
	SECTION("White pawn promotes to queen — gives check")
	{
		// White pawn promoted to queen on b8; black king on a8 in check from queen on b8
		// (Queen on b8 attacks a8 along rank 8)
		REQUIRE(board.SetupFromFEN("kQ6/8/8/8/8/8/8/7K b - - 0 1"));
		auto m = MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false);
		CHECK(MoveFormatter::ToShort(m, board) == "Pb7-b8Q+");
	}
	SECTION("Black en passant — no check")
	{
		// Black pawn on f4 captures en passant to e3; white king on h1, not in check
		// Position after black plays fxe3ep: black pawn on e3, e4 empty
		REQUIRE(board.SetupFromFEN("7k/8/8/8/8/4p3/8/7K w - - 0 2"));
		auto m = MoveFactory::MakeEnPassant(f4, e3);
		CHECK(MoveFormatter::ToShort(m, board) == "pf4-e3ep");
	}
}

// ── ToCoord ───────────────────────────────────────────────────────────────────
// No board context. This is the format search diagnostics and PV lines emit, so the
// exact spelling is what shows up in logs -- pin it.

TEST_CASE("MoveFormatter::ToCoord - no board context", "[formatter]")
{
	SECTION("Quiet pawn push e2-e4")
	{
		CHECK(MoveFormatter::ToCoord(MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH)) == "e2-e4");
	}
	SECTION("Quiet knight move g1-f3") { CHECK(MoveFormatter::ToCoord(MoveFactory::MakeQuiet(g1, f3)) == "g1-f3"); }
	SECTION("Capture uses 'x'") { CHECK(MoveFormatter::ToCoord(MoveFactory::MakeCapture(c5, d6)) == "c5xd6"); }
	SECTION("En passant carries the 'ep' suffix")
	{
		// Distinct from ToUCI, which renders this as plain "f5e6".
		CHECK(MoveFormatter::ToCoord(MoveFactory::MakeEnPassant(f5, e6)) == "f5-e6ep");
	}
	SECTION("Castling collapses to 0-0 / 0-0-0, losing the squares")
	{
		CHECK(MoveFormatter::ToCoord(MoveFactory::MakeMove(e1, g1, MoveType::KING_CASTLE)) == "0-0");
		CHECK(MoveFormatter::ToCoord(MoveFactory::MakeMove(e1, c1, MoveType::QUEEN_CASTLE)) == "0-0-0");
		CHECK(MoveFormatter::ToCoord(MoveFactory::MakeMove(e8, g8, MoveType::KING_CASTLE)) == "0-0");
	}
	SECTION("Promotion shows the squares but not the promoted piece")
	{
		// Coordinate form has no piece prefix, so it cannot express the promotion choice.
		CHECK(MoveFormatter::ToCoord(MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false)) == "b7-b8");
		CHECK(MoveFormatter::ToCoord(MoveFactory::MakePromotion(b7, b8, WHITE_KNIGHT, false)) == "b7-b8");
	}
	SECTION("Capture-promotion uses 'x'")
	{
		CHECK(MoveFormatter::ToCoord(MoveFactory::MakePromotion(b7, a8, WHITE_QUEEN, true)) == "b7xa8");
	}
}

// ── ToShort (explicit piece) ──────────────────────────────────────────────────
// Same output as the Board overload minus the check annotation.

TEST_CASE("MoveFormatter::ToShort - explicit piece, no board", "[formatter]")
{
	SECTION("White pawn push")
	{
		CHECK(MoveFormatter::ToShort(MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH), WHITE_PAWN) ==
		      "Pe2-e4");
	}
	SECTION("Piece prefix case encodes colour")
	{
		CHECK(MoveFormatter::ToShort(MoveFactory::MakeQuiet(b1, c3), WHITE_KNIGHT) == "Nb1-c3");
		CHECK(MoveFormatter::ToShort(MoveFactory::MakeQuiet(b8, c6), BLACK_KNIGHT) == "nb8-c6");
	}
	SECTION("Capture and en passant")
	{
		CHECK(MoveFormatter::ToShort(MoveFactory::MakeCapture(c1, c7), WHITE_ROOK) == "Rc1xc7");
		CHECK(MoveFormatter::ToShort(MoveFactory::MakeEnPassant(f4, e3), BLACK_PAWN) == "pf4-e3ep");
	}
	SECTION("Castling")
	{
		CHECK(MoveFormatter::ToShort(MoveFactory::MakeMove(e1, g1, MoveType::KING_CASTLE), WHITE_KING) == "0-0");
		CHECK(MoveFormatter::ToShort(MoveFactory::MakeMove(e1, c1, MoveType::QUEEN_CASTLE), WHITE_KING) == "0-0-0");
	}
	SECTION("Promotion prefixes the pawn and suffixes the promoted piece")
	{
		CHECK(MoveFormatter::ToShort(MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false), WHITE_QUEEN) == "Pb7-b8Q");
		CHECK(MoveFormatter::ToShort(MoveFactory::MakePromotion(b2, b1, BLACK_QUEEN, false), BLACK_QUEEN) == "pb2-b1q");
		CHECK(MoveFormatter::ToShort(MoveFactory::MakePromotion(b7, a8, WHITE_ROOK, true), WHITE_ROOK) == "Pb7xa8R");
	}
	SECTION("Agrees with the Board overload when the move gives no check")
	{
		Board board;
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1"));
		auto m = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
		CHECK(MoveFormatter::ToShort(m, board) == MoveFormatter::ToShort(m, board.GetPiece(m.to())));
	}
	SECTION("Board overload adds '+' that the explicit-piece overload does not")
	{
		Board board;
		REQUIRE(board.SetupFromFEN("6k1/6R1/8/8/8/8/8/7K b - - 0 1"));
		auto m = MoveFactory::MakeCapture(g1, g7);
		CHECK(MoveFormatter::ToShort(m, board) == "Rg1xg7+");
		CHECK(MoveFormatter::ToShort(m, WHITE_ROOK) == "Rg1xg7");
	}
}

// ── ToVerbose ─────────────────────────────────────────────────────────────────

TEST_CASE("MoveFormatter::ToVerbose - spot checks", "[formatter]")
{
	Board board;

	SECTION("White pawn quiet push")
	{
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1"));
		auto m = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
		CHECK(MoveFormatter::ToVerbose(m, board) == "White pawn moves e2 to e4");
	}
	SECTION("White rook captures and checks")
	{
		REQUIRE(board.SetupFromFEN("6k1/6R1/8/8/8/8/8/7K b - - 0 1"));
		auto m = MoveFactory::MakeCapture(g1, g7);
		CHECK(MoveFormatter::ToVerbose(m, board) == "White rook captures on g7 and checks!");
	}
	SECTION("White king castles kingside")
	{
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQ1RK1 b kq - 1 1"));
		auto m = MoveFactory::MakeMove(e1, g1, MoveType::KING_CASTLE);
		CHECK(MoveFormatter::ToVerbose(m, board) == "White king castles kingside");
	}
	SECTION("White king castles queenside")
	{
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/2KR1BNR b kq - 1 1"));
		auto m = MoveFactory::MakeMove(e1, c1, MoveType::QUEEN_CASTLE);
		CHECK(MoveFormatter::ToVerbose(m, board) == "White king castles queenside");
	}
	SECTION("White pawn promotes to queen")
	{
		REQUIRE(board.SetupFromFEN("1Q6/8/k7/8/8/8/8/7K b - - 0 1"));
		auto m = MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false);
		CHECK(MoveFormatter::ToVerbose(m, board) == "White pawn promotes to queen on b8");
	}
	SECTION("Black pawn captures and promotes to rook — no check")
	{
		// Black pawn on b2 captured on a1 promoting to rook; white king on h3 (not on rank 1
		// or file a, so not in check from the newly promoted rook on a1)
		REQUIRE(board.SetupFromFEN("7k/8/8/8/8/7K/8/r7 w - - 0 1"));
		auto m = MoveFactory::MakePromotion(b2, a1, BLACK_ROOK, true);
		CHECK(MoveFormatter::ToVerbose(m, board) == "Black pawn captures and promotes to rook on a1");
	}
	SECTION("White pawn en passant")
	{
		// White pawn on d5 captured en passant to e6; black pawn on e5 gone
		REQUIRE(board.SetupFromFEN("8/8/4P3/8/8/8/8/4K2k b - - 0 1"));
		auto m = MoveFactory::MakeEnPassant(d5, e6);
		CHECK(MoveFormatter::ToVerbose(m, board) == "White pawn captures en passant on e6");
	}
}

// ── FromUCI ───────────────────────────────────────────────────────────────────

TEST_CASE("MoveFormatter::FromUCI - basic moves", "[formatter]")
{
	Board board;

	SECTION("Quiet pawn push e2-e4 from starting position")
	{
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
		Move m = MoveFormatter::FromUCI("e2e4", board);
		CHECK(m.from() == e2);
		CHECK(m.to() == e4);
		CHECK(MoveHelper::AsType(m) == MoveType::DOUBLE_PAWN_PUSH);
	}
	SECTION("Quiet knight move g1-f3")
	{
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 1"));
		Move m = MoveFormatter::FromUCI("g1f3", board);
		CHECK(m.from() == g1);
		CHECK(m.to() == f3);
		CHECK(MoveHelper::AsType(m) == MoveType::QUIET);
	}
	SECTION("Capture — piece on destination square")
	{
		// White rook on a1, black pawn on a5
		REQUIRE(board.SetupFromFEN("7k/8/8/p7/8/8/8/R6K w - - 0 1"));
		Move m = MoveFormatter::FromUCI("a1a5", board);
		CHECK(m.from() == a1);
		CHECK(m.to() == a5);
		CHECK(MoveHelper::AsType(m) == MoveType::CAPTURE);
	}
	SECTION("White kingside castle e1-g1")
	{
		REQUIRE(board.SetupFromFEN("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"));
		Move m = MoveFormatter::FromUCI("e1g1", board);
		CHECK(m.from() == e1);
		CHECK(m.to() == g1);
		CHECK(MoveHelper::AsType(m) == MoveType::KING_CASTLE);
	}
	SECTION("White queenside castle e1-c1")
	{
		REQUIRE(board.SetupFromFEN("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"));
		Move m = MoveFormatter::FromUCI("e1c1", board);
		CHECK(m.from() == e1);
		CHECK(m.to() == c1);
		CHECK(MoveHelper::AsType(m) == MoveType::QUEEN_CASTLE);
	}
	SECTION("En passant — pawn diagonal to empty square")
	{
		// White pawn d5, black pawn just moved e7-e5 (EP square e6 is empty)
		REQUIRE(board.SetupFromFEN("8/8/8/3Pp3/8/8/8/4K2k w - e6 0 1"));
		Move m = MoveFormatter::FromUCI("d5e6", board);
		CHECK(m.from() == d5);
		CHECK(m.to() == e6);
		CHECK(MoveHelper::AsType(m) == MoveType::EP_CAPTURE);
	}
	SECTION("White promotes to queen — quiet")
	{
		REQUIRE(board.SetupFromFEN("8/1P6/8/8/8/8/8/4K2k w - - 0 1"));
		Move m = MoveFormatter::FromUCI("b7b8q", board);
		CHECK(m.from() == b7);
		CHECK(m.to() == b8);
		CHECK(MoveHelper::AsType(m) == MoveType::PROMOTION_QUEEN);
	}
	SECTION("White promotes to knight with capture")
	{
		// White pawn on b7, black rook on a8
		REQUIRE(board.SetupFromFEN("r7/1P6/8/8/8/8/8/4K2k w - - 0 1"));
		Move m = MoveFormatter::FromUCI("b7a8n", board);
		CHECK(m.from() == b7);
		CHECK(m.to() == a8);
		CHECK(MoveHelper::AsType(m) == MoveType::PROMOTION_KNIGHT_CAPTURE);
	}
	SECTION("Malformed input returns empty move — too short")
	{
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
		Move m = MoveFormatter::FromUCI("e2", board); // too short
		CHECK(m.is_null());
	}
	SECTION("Malformed input returns empty move — invalid promotion suffix")
	{
		// White pawn on b7; 5-char string but unrecognised suffix 'x'.
		REQUIRE(board.SetupFromFEN("8/1P6/8/8/8/8/8/4K2k w - - 0 1"));
		Move m = MoveFormatter::FromUCI("b7b8x", board); // invalid suffix
		CHECK(m.is_null());
	}
	SECTION("Malformed input returns empty move — too long")
	{
		REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
		Move m = MoveFormatter::FromUCI("e2e4xx", board); // trailing garbage past the 5-char form
		CHECK(m.is_null());
	}
}

// ── Round-trip: ToUCI -> FromUCI ──────────────────────────────────────────────

TEST_CASE("MoveFormatter - ToUCI/FromUCI round-trip for common move types", "[formatter]")
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

	SECTION("Pawn push")
	{
		auto original = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
		auto rt = MoveFormatter::FromUCI(MoveFormatter::ToUCI(original), board);
		CHECK(rt.from() == original.from());
		CHECK(rt.to() == original.to());
		CHECK(MoveHelper::AsType(rt) == MoveType::DOUBLE_PAWN_PUSH);
	}
	SECTION("Promotion queen")
	{
		REQUIRE(board.SetupFromFEN("8/1P6/8/8/8/8/8/4K2k w - - 0 1"));
		auto original = MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false);
		auto rt = MoveFormatter::FromUCI(MoveFormatter::ToUCI(original), board);
		CHECK(rt.from() == original.from());
		CHECK(rt.to() == original.to());
		CHECK(MoveHelper::AsType(rt) == MoveType::PROMOTION_QUEEN);
	}
}
