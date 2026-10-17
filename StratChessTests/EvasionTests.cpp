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
#include "MoveHelper.h"
#include "Tools/Perft.h"
#include <algorithm>
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

	// The oracle: ComputeLegalMoves() filtered by DoMove(), in emission order.
	void LegalByOracle(Board& board, MoveList& out)
	{
		MoveList all;
		MoveGenerator::ComputeLegalMoves(board, all);
		for (const auto& move : all)
			if (board.IsLegalMove(move))
				out.push(move);
	}

	bool SameSequence(const MoveList& a, const MoveList& b)
	{
		return std::equal(a.begin(), a.end(), b.begin(), b.end());
	}

	// Every in-check position within `depth` plies of the board, each compared against the oracle.
	void CompareInCheckPositions(Board& board, int depth, int& compared, int& mismatches)
	{
		if (board.InCheck()) {
			MoveList evasions;
			MoveGenerator::ComputeEvasions(board, evasions);
			MoveList expected;
			LegalByOracle(board, expected);
			++compared;
			if (!SameSequence(evasions, expected)) {
				if (++mismatches <= 5)
					UNSCOPED_INFO("mismatch at " << board.ExtractFEN());
			}
		}
		if (depth == 0)
			return;
		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, moves);
		for (const auto& move : moves) {
			if (!board.DoMove(move))
				continue;
			CompareInCheckPositions(board, depth - 1, compared, mismatches);
			board.UndoMove(move);
		}
	}

	// Every position within `depth` plies of the board, in check or not: ComputeCaptures() then
	// ComputeQuiets() must be ComputeLegalMoves() stably partitioned by capture-or-promotion.
	void ComparePartitions(Board& board, int depth, int& compared, int& mismatches)
	{
		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, moves);
		MoveList expected;
		for (const auto& move : moves)
			if (MoveHelper::IsCapture(move) || MoveHelper::IsPromote(move))
				expected.push(move);
		for (const auto& move : moves)
			if (!MoveHelper::IsCapture(move) && !MoveHelper::IsPromote(move))
				expected.push(move);
		MoveList staged;
		MoveGenerator::ComputeCaptures(board, staged);
		MoveGenerator::ComputeQuiets(board, staged);
		++compared;
		if (!SameSequence(staged, expected) && ++mismatches <= 5)
			UNSCOPED_INFO("mismatch at " << board.ExtractFEN());
		if (depth == 0)
			return;
		for (const auto& move : moves) {
			if (!board.DoMove(move))
				continue;
			ComparePartitions(board, depth - 1, compared, mismatches);
			board.UndoMove(move);
		}
	}

	// The evasions of a hand-built position, checked against the oracle, as UCI strings.
	std::vector<std::string> Evasions(const char* fen)
	{
		INFO("fen = " << fen);
		Board board(fen);
		REQUIRE(board.InCheck());
		MoveList evasions;
		MoveGenerator::ComputeEvasions(board, evasions);
		MoveList expected;
		LegalByOracle(board, expected);
		CHECK(SameSequence(evasions, expected));
		std::vector<std::string> out;
		for (const auto& move : evasions)
			out.push_back(MoveFormatter::ToUCI(move));
		return out;
	}

	bool Contains(const std::vector<std::string>& moves, const char* uci)
	{
		return std::ranges::find(moves, uci) != moves.end();
	}

	// Both colours, pawn captures in both directions, quiet and capture promotions, single and double
	// pushes, every officer type, en passant and castling.
	constexpr const char* kWhiteFen = "r3k2r/1P4P1/8/3pP3/2N5/5B2/P2Q3P/R3K2R w KQkq d6 0 1";
	constexpr const char* kBlackFen = "r3k2r/p2q3p/2n5/8/3Pp3/5b2/1p4p1/R3K2R b KQkq d3 0 1";
} // namespace

// clang-format off
// The pinned emission order: a change here changes search node counts.
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

TEST_CASE("Evasions equal the legal subsequence of the pseudo-legal list", "[movegen][evasion]")
{
	int compared = 0;
	int mismatches = 0;
	for (const auto& position : Testing::Perft::get_test_positions(false)) {
		Board board(position.fen);
		CompareInCheckPositions(board, 3, compared, mismatches);
	}
	INFO("in-check positions compared: " << compared);
	CHECK(mismatches == 0);
	// A walk that reaches few checks proves little.
	CHECK(compared > 1000);
}

TEST_CASE("Captures then quiets partition the pseudo-legal list", "[movegen]")
{
	int compared = 0;
	int mismatches = 0;
	for (const auto& position : Testing::Perft::get_test_positions(false)) {
		Board board(position.fen);
		ComparePartitions(board, 2, compared, mismatches);
	}
	INFO("positions compared: " << compared);
	CHECK(mismatches == 0);
	CHECK(compared > 5'000);
}

TEST_CASE("Evasions: double check leaves king moves only", "[movegen][evasion]")
{
	const auto moves = Evasions("4r1k1/8/8/8/8/3n4/8/4K3 w - - 0 1");
	REQUIRE_FALSE(moves.empty());
	for (const auto& move : moves)
		CHECK(move.starts_with("e1"));
}

TEST_CASE("Evasions: the king may not retreat along the checking line", "[movegen][evasion]")
{
	const auto moves = Evasions("4r1k1/8/8/8/4K3/8/8/8 w - - 0 1");
	REQUIRE_FALSE(moves.empty());
	CHECK_FALSE(Contains(moves, "e4e3"));
}

TEST_CASE("Evasions: the king takes an undefended checker, not a defended one", "[movegen][evasion]")
{
	CHECK(Contains(Evasions("7k/8/8/8/8/8/4q3/4K3 w - - 0 1"), "e1e2"));
	CHECK_FALSE(Contains(Evasions("4r2k/8/8/8/8/8/4q3/4K3 w - - 0 1"), "e1e2"));
}

TEST_CASE("Evasions: a pinned piece does not block", "[movegen][evasion]")
{
	const auto moves = Evasions("4r1k1/8/8/8/1b6/8/4N3/4K3 w - - 0 1");
	REQUIRE_FALSE(moves.empty());
	CHECK_FALSE(Contains(moves, "e2c3"));
}

TEST_CASE("Evasions: a promotion blocks", "[movegen][evasion]")
{
	const auto moves = Evasions("K6r/2P5/8/8/8/8/8/k7 w - - 0 1");
	CHECK(Contains(moves, "c7c8q"));
	CHECK(Contains(moves, "c7c8n"));
}

TEST_CASE("Evasions: en passant takes the checking pawn unless it uncovers the king", "[movegen][evasion]")
{
	CHECK(Contains(Evasions("7k/8/8/3pP3/4K3/8/8/8 w - d6 0 1"), "e5d6"));

	const auto pinned = Evasions("7k/8/2b5/3pP3/4K3/8/8/8 w - d6 0 1");
	REQUIRE_FALSE(pinned.empty());
	CHECK_FALSE(Contains(pinned, "e5d6"));
}
