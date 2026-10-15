// EvasionTests.cpp — Catch2 tests for MoveGenerator::ComputeEvasions() and the generation order it
// shares with ComputeLegalMoves() and ComputeCaptures().
//
// Search is node-identical only while every generator emits its moves in the same order: the
// sorter breaks score ties by generation index. The evasion tests compare against the pseudo-legal
// generator, which shares its helpers, so the pinned sequences below guard that order on their own.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include <string>
#include <vector>

namespace {
	// "uci:flags" per move, in emission order.
	template <typename Generate> std::vector<std::string> Sequence(const char* fen, Generate generate)
	{
		Board board(fen);
		MoveList moves;
		generate(board, moves);
		std::vector<std::string> out;
		for (const auto& move : moves)
			out.push_back(MoveFormatter::ToUCI(move) + ":" + std::to_string(move.flags()));
		return out;
	}

	// Both colours, pawn captures in both directions, quiet and capture promotions, single and double
	// pushes, every officer type, en passant and castling.
	constexpr const char* kWhiteFen = "r3k2r/1P4P1/8/3pP3/2N5/5B2/P2Q3P/R3K2R w KQkq d6 0 1";
	constexpr const char* kBlackFen = "r3k2r/p2q3p/2n5/8/3Pp3/5b2/1p4p1/R3K2R b KQkq d3 0 1";
} // namespace

// clang-format off
// Captured from origin/main 663e2c5, before the generator helpers took masks.
TEST_CASE("Generators keep their emission order", "[movegen]")
{
	CHECK(Sequence(kWhiteFen, MoveGenerator::ComputeLegalMoves) == std::vector<std::string>{
		"b7a8q:15", "b7a8r:14", "b7a8b:13", "b7a8n:12", "e5d6:5", "g7h8q:15", "g7h8r:14", "g7h8b:13", "g7h8n:12",
		"b7b8q:11", "b7b8r:10", "b7b8b:9", "b7b8n:8", "g7g8q:11", "g7g8r:10", "g7g8b:9", "g7g8n:8", "e5e6:0", "a2a4:1",
		"h2h4:1", "a2a3:0", "h2h3:0", "c4b6:0", "c4d6:0", "c4a5:0", "c4a3:0", "c4e3:0", "c4b2:0", "f3d5:4", "f3h5:0",
		"f3e4:0", "f3g4:0", "f3e2:0", "f3g2:0", "f3d1:0", "a1b1:0", "a1c1:0", "a1d1:0", "h1f1:0", "h1g1:0", "d2h6:0",
		"d2a5:0", "d2d5:4", "d2g5:0", "d2b4:0", "d2d4:0", "d2f4:0", "d2c3:0", "d2d3:0", "d2e3:0", "d2b2:0", "d2c2:0",
		"d2e2:0", "d2f2:0", "d2g2:0", "d2c1:0", "d2d1:0", "e1e2:0", "e1f2:0", "e1d1:0", "e1f1:0", "e1g1:2", "e1c1:3"});
	CHECK(Sequence(kWhiteFen, MoveGenerator::ComputeCaptures) == std::vector<std::string>{
		"b7a8q:15", "b7a8r:14", "b7a8b:13", "b7a8n:12", "e5d6:5", "g7h8q:15", "g7h8r:14", "g7h8b:13", "g7h8n:12",
		"b7b8q:11", "b7b8r:10", "b7b8b:9", "b7b8n:8", "g7g8q:11", "g7g8r:10", "g7g8b:9", "g7g8n:8", "f3d5:4", "d2d5:4"});
	CHECK(Sequence(kBlackFen, MoveGenerator::ComputeLegalMoves) == std::vector<std::string>{
		"e4d3:5", "b2a1q:15", "b2a1r:14", "b2a1b:13", "b2a1n:12", "g2h1q:15", "g2h1r:14", "g2h1b:13", "g2h1n:12",
		"b2b1q:11", "b2b1r:10", "b2b1b:9", "b2b1n:8", "g2g1q:11", "g2g1r:10", "g2g1b:9", "g2g1n:8", "a7a6:0", "h7h6:0",
		"a7a5:1", "h7h5:1", "e4e3:0", "c6b8:0", "c6d8:0", "c6e7:0", "c6a5:0", "c6e5:0", "c6b4:0", "c6d4:4", "f3h5:0",
		"f3g4:0", "f3e2:0", "f3d1:0", "a8b8:0", "a8c8:0", "a8d8:0", "h8f8:0", "h8g8:0", "d7c8:0", "d7d8:0", "d7b7:0",
		"d7c7:0", "d7e7:0", "d7f7:0", "d7g7:0", "d7d6:0", "d7e6:0", "d7d5:0", "d7f5:0", "d7d4:4", "d7g4:0", "d7h3:0",
		"e8d8:0", "e8f8:0", "e8e7:0", "e8f7:0", "e8g8:2", "e8c8:3"});
	CHECK(Sequence(kBlackFen, MoveGenerator::ComputeCaptures) == std::vector<std::string>{
		"e4d3:5", "b2a1q:15", "b2a1r:14", "b2a1b:13", "b2a1n:12", "g2h1q:15", "g2h1r:14", "g2h1b:13", "g2h1n:12",
		"b2b1q:11", "b2b1r:10", "b2b1b:9", "b2b1n:8", "g2g1q:11", "g2g1r:10", "g2g1b:9", "g2g1n:8", "c6d4:4", "d7d4:4"});
}
// clang-format on
