// FiftyMoveRuleTests.cpp — Catch2 [fifty_move] tests for the fifty-move rule.
//
// The rule is 100 halfmoves. Board::halfmove_clock() counts halfmoves (loaded from the FEN
// halfmove field, incremented once per DoMove), so every threshold here is in halfmoves.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "AIPerplex.h"
#include "Board.h"
#include "GameState.h"
#include "MoveFactory.h"
#include "PlayerBase.h"
#include "SearchLimits.h"
#include "TacticalTestHelpers.h"
#include "defines.h"
#include <string>

namespace {

	// A middlegame position with ample material, so a healthy search reports a real move
	// and a non-zero node count. Only the halfmove field varies across the cases.
	std::string fen_with_clock(int halfmoves)
	{
		return "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - " + std::to_string(halfmoves) + " 4";
	}

} // namespace

// ============================================================================
// Clock advance and reset, driven through the board that owns the counter
// ============================================================================

TEST_CASE("Board: a quiet move below 100 advances the halfmove clock", "[fifty_move]")
{
	const int start = GENERATE(49, 50, 98);
	INFO("starting halfmove clock: " << start);

	Board board(fen_with_clock(start));
	REQUIRE(board.halfmove_clock() == start);

	REQUIRE(board.DoMove(MoveFactory::MakeQuiet(f3, g5)));

	CHECK(board.halfmove_clock() == start + 1);
}

TEST_CASE("Board: a quiet move takes the halfmove clock from 99 to the 100 threshold", "[fifty_move]")
{
	Board board(fen_with_clock(99));

	REQUIRE(board.DoMove(MoveFactory::MakeQuiet(f3, g5)));

	// The board reports the fact; adjudicating the draw from it is Game::Run's job.
	CHECK(board.halfmove_clock() == HALFMOVE_CLOCK_LIMIT);
}

TEST_CASE("Board: a capture resets the halfmove clock", "[fifty_move]")
{
	Board board(fen_with_clock(99));

	REQUIRE(board.DoMove(MoveFactory::MakeMove(c4, f7, MoveType::CAPTURE)));

	CHECK(board.halfmove_clock() == 0);
}

TEST_CASE("Board: a pawn move resets the halfmove clock", "[fifty_move]")
{
	Board board(fen_with_clock(99));

	REQUIRE(board.DoMove(MoveFactory::MakeQuiet(d2, d3)));

	CHECK(board.halfmove_clock() == 0);
}

TEST_CASE("Board: UndoMove restores the halfmove clock across the threshold", "[fifty_move]")
{
	Board board(fen_with_clock(99));
	const auto m = MoveFactory::MakeQuiet(f3, g5);

	REQUIRE(board.DoMove(m));
	REQUIRE(board.halfmove_clock() == HALFMOVE_CLOCK_LIMIT);
	board.UndoMove(m);

	CHECK(board.halfmove_clock() == 99);
}

// ============================================================================
// The root must still search — a high clock is not a reason to return no move
// ============================================================================

TEST_CASE("Search from a high-but-legal halfmove clock still searches and returns a move", "[fifty_move]")
{
	const int clock = GENERATE(4, 49, 50, 60, 99);
	INFO("halfmove clock: " << clock);

	Board board(fen_with_clock(clock));
	auto ai = make_tactical_engine(4);

	REQUIRE(board.halfmove_clock() == clock);

	const SearchResult result = ai->Search(board, SearchLimits::fixed_depth(4));
	const Move move = result.best_move;

	CHECK_FALSE(move.is_null());
	CHECK(result.nodes_searched > 0);
}

// At or past the threshold the game is drawn, but a UCI engine is not the arbiter of that
// and must still answer with a legal move rather than nothing.
//
// The position deliberately keeps a root capture available (Nxe5, Bxf7), so the search has to
// cope with the clock resetting at ply 0 while it is already at the limit. Keep one when
// changing FENs.
TEST_CASE("Search at or past the fifty-move threshold still returns a legal move", "[fifty_move]")
{
	const int clock = GENERATE(100, 120);
	INFO("halfmove clock: " << clock);

	Board board(fen_with_clock(clock));
	auto ai = make_tactical_engine(4);

	const Move move = ai->Search(board, SearchLimits::fixed_depth(4)).best_move;

	REQUIRE_FALSE(move.is_null());

	Board replay(fen_with_clock(clock));
	CHECK(replay.IsLegalMove(move));
}

// The search adjudicates its own root and nothing else. A fifty-move draw is a fact about
// the position that exists only once the caller commits the move, so the returned
// game_state stays STILL_PLAYING and the clock crosses the limit on the caller's board.
// Turning that into DRAW_50_MOVES is Game::Run()'s job.
//
// KR vs K has no pawn move or capture available, so whatever the search picks pushes the
// clock to the limit — the one case where the move and the draw arrive together.
TEST_CASE("The move that reaches the fifty-move threshold is returned, not withheld", "[fifty_move]")
{
	Board board("4k3/8/8/8/8/8/1R6/4K3 w - - 99 60");
	auto ai = make_tactical_engine(4);

	REQUIRE(board.halfmove_clock() == 99);

	const SearchResult result = ai->Search(board, SearchLimits::fixed_depth(4));

	REQUIRE_FALSE(result.best_move.is_null());
	CHECK(result.game_state == GameStates::STILL_PLAYING);

	// Replayed on a fresh board so the check stays independent of anything the search
	// may come to write back through the board it holds by reference.
	Board replay("4k3/8/8/8/8/8/1R6/4K3 w - - 99 60");
	REQUIRE(replay.IsLegalMove(result.best_move));
	REQUIRE(replay.DoMove(result.best_move));
	CHECK(replay.halfmove_clock() == HALFMOVE_CLOCK_LIMIT);
}
