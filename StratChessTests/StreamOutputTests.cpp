#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include "Board.h"
#include "Move.h"

TEST_CASE("Board stream output preserves the ASCII grid", "[board][stream_output]")
{
	std::ostringstream out;
	SECTION("Starting position")
	{
		const Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
		out << board;
		CHECK(out.str() == "8  r n b q k b n r\n"
		                   "7  p p p p p p p p\n"
		                   "6  . . . . . . . .\n"
		                   "5  . . . . . . . .\n"
		                   "4  . . . . . . . .\n"
		                   "3  . . . . . . . .\n"
		                   "2  P P P P P P P P\n"
		                   "1  R N B Q K B N R\n"
		                   "\n   A B C D E F G H\n\n");
	}
	SECTION("Sparse position")
	{
		const Board board("4k3/8/8/8/8/8/8/6K1 w - - 0 1");
		out << board;
		CHECK(out.str() == "8  . . . . k . . .\n"
		                   "7  . . . . . . . .\n"
		                   "6  . . . . . . . .\n"
		                   "5  . . . . . . . .\n"
		                   "4  . . . . . . . .\n"
		                   "3  . . . . . . . .\n"
		                   "2  . . . . . . . .\n"
		                   "1  . . . . . . K .\n"
		                   "\n   A B C D E F G H\n\n");
	}
}

TEST_CASE("Move stream output preserves coordinate notation and ignores null moves", "[moves][stream_output]")
{
	std::ostringstream out;
	SECTION("Null move")
	{
		out << Move::EmptyMove();
		CHECK(out.str().empty());
	}
	SECTION("Pawn move")
	{
		out << Move(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
		CHECK(out.str() == "Move: e2-e4\n");
	}
}
