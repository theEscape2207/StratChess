#include <catch2/catch_test_macros.hpp>
#include "PVIntegrity.h"
#include "Board.h"
#include "Move.h"
#include "defines.h"
#include <vector>

// ============================================================
// pv_replays_legally() — the invariant behind the assertion in
// AIPerplex::emit_iteration_info. Tested directly, without a
// search, because that is the whole point of it being a named
// helper: the assertion fires from inside a search, where the
// input that produced it is no longer reachable.
// ============================================================

namespace {

	Board board_from(const std::string& fen)
	{
		Board board;
		REQUIRE(board.SetupFromFEN(fen));
		return board;
	}

	bool replays(const Board& board, const std::vector<Move>& line)
	{
		return pv_replays_legally(board, std::span<const Move>(line));
	}

	const std::string START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

} // namespace

TEST_CASE("pv_replays_legally: an empty line replays trivially", "[pv]")
{
	const Board board = board_from(START_FEN);

	REQUIRE(pv_replays_legally(board, std::span<const Move>{}));
}

TEST_CASE("pv_replays_legally: a real line from the position replays", "[pv]")
{
	const Board board = board_from(START_FEN);

	const std::vector<Move> line{Move(e2, e4, MoveType::DOUBLE_PAWN_PUSH), Move(e7, e5, MoveType::DOUBLE_PAWN_PUSH),
	                             Move(g1, f3, MoveType::QUIET), Move(b8, c6, MoveType::QUIET)};

	REQUIRE(replays(board, line));
}

TEST_CASE("pv_replays_legally: a move from a square the previous move emptied is rejected", "[pv]")
{
	// The shape a spliced PV takes: the first move is genuine, the rest describes a
	// different position. Here e2 is empty by the time the second move is tried.
	const Board board = board_from(START_FEN);

	const std::vector<Move> line{Move(e2, e4, MoveType::DOUBLE_PAWN_PUSH), Move(e2, e4, MoveType::DOUBLE_PAWN_PUSH)};

	REQUIRE_FALSE(replays(board, line));
}

TEST_CASE("pv_replays_legally: flags are part of the match, not just from/to", "[pv]")
{
	// Move equality compares from/to only, so a membership test written with == would
	// accept every one of these. Each names squares that carry a real move.
	const Board board = board_from(START_FEN);

	SECTION("a double push flagged as a quiet move is not the generated move")
	{
		const std::vector<Move> line{Move(e2, e4, MoveType::QUIET)};
		REQUIRE_FALSE(replays(board, line));
	}
	SECTION("the generated flag replays")
	{
		const std::vector<Move> line{Move(e2, e4, MoveType::DOUBLE_PAWN_PUSH)};
		REQUIRE(replays(board, line));
	}
}

TEST_CASE("pv_replays_legally: the promotion piece has to be the one the position offers", "[pv]")
{
	const Board board = board_from("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");

	SECTION("a7a8=Q is available")
	{
		const std::vector<Move> line{Move(a7, a8, MoveType::PROMOTION_QUEEN)};
		REQUIRE(replays(board, line));
	}
	SECTION("a7a8=Q as a capture is not — a8 is empty")
	{
		const std::vector<Move> line{Move(a7, a8, MoveType::PROMOTION_QUEEN_CAPTURE)};
		REQUIRE_FALSE(replays(board, line));
	}
}

TEST_CASE("pv_replays_legally: a pseudo-legal move that leaves its own king in check is rejected", "[pv]")
{
	// The half a membership test cannot answer: ComputeLegalMoves does generate Re7-a7,
	// because it does not test check. Only DoMove rejects it.
	const Board board = board_from("4k3/4r3/8/8/8/8/8/4RK2 b - - 0 1");

	const std::vector<Move> line{Move(e7, a7, MoveType::QUIET)};

	REQUIRE_FALSE(replays(board, line));
}

TEST_CASE("pv_replays_legally: the caller's board is not modified", "[pv]")
{
	// The assertion runs against the live search root, so replaying has to be a
	// side-effect-free question about it.
	Board board = board_from(START_FEN);

	const std::vector<Move> line{Move(e2, e4, MoveType::DOUBLE_PAWN_PUSH), Move(e7, e5, MoveType::DOUBLE_PAWN_PUSH)};
	REQUIRE(replays(board, line));

	REQUIRE(board.ExtractFEN() == START_FEN);
}
