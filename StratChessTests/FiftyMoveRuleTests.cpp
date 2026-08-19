// FiftyMoveRuleTests.cpp — Catch2 [fifty_move] tests for the fifty-move rule.
//
// The rule is 100 halfmoves. GameInfo::halfmoveClock counts halfmoves (loaded from the FEN
// halfmove field, incremented once per DoMove), so every threshold here is in halfmoves.

#include <catch_amalgamated.hpp>
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

	GameInfo info_at_clock(int halfmoves)
	{
		GameInfo info;
		info.Reset();
		info.halfmoveClock = halfmoves;
		return info;
	}

} // namespace

// ============================================================================
// GameInfo threshold — the rule triggers at 100 halfmoves, not 50
// ============================================================================

TEST_CASE("GameInfo: halfmove clock below 100 keeps the game running", "[fifty_move]")
{
	const int start = GENERATE(49, 50, 98);
	INFO("starting halfmove clock: " << start);

	GameInfo info = info_at_clock(start);
	info.UpdateBoardInfo(MoveFactory::MakeMove(g1, f3, MoveType::QUIET), ePiece::WHITE_KNIGHT);

	CHECK(info.halfmoveClock == start + 1);
	CHECK(info.gameState == GameStates::STILL_PLAYING);
}

TEST_CASE("GameInfo: halfmove clock 99 -> 100 draws by the fifty-move rule", "[fifty_move]")
{
	GameInfo info = info_at_clock(99);
	info.UpdateBoardInfo(MoveFactory::MakeMove(g1, f3, MoveType::QUIET), ePiece::WHITE_KNIGHT);

	CHECK(info.halfmoveClock == 100);
	CHECK(info.gameState == GameStates::DRAW_50_MOVES);
}

TEST_CASE("GameInfo: a capture resets the halfmove clock", "[fifty_move]")
{
	GameInfo info = info_at_clock(99);
	info.UpdateBoardInfo(MoveFactory::MakeMove(g1, f3, MoveType::CAPTURE), ePiece::WHITE_KNIGHT);

	CHECK(info.halfmoveClock == 0);
	CHECK(info.gameState == GameStates::STILL_PLAYING);
}

TEST_CASE("GameInfo: a pawn move resets the halfmove clock", "[fifty_move]")
{
	GameInfo info = info_at_clock(99);
	info.UpdateBoardInfo(MoveFactory::MakeMove(e2, e3, MoveType::QUIET), ePiece::WHITE_PAWN);

	CHECK(info.halfmoveClock == 0);
	CHECK(info.gameState == GameStates::STILL_PLAYING);
}

// ============================================================================
// The root must still search — a high clock is not a reason to return no move
// ============================================================================

TEST_CASE("Search from a high-but-legal halfmove clock still searches and returns a move", "[fifty_move]")
{
	const int clock = GENERATE(4, 49, 50, 60, 99);
	INFO("halfmove clock: " << clock);

	Board board(fen_with_clock(clock));
	auto ai = make_tactical_engine(board, 4);

	GameInfo info = board.GetGameInfo();
	REQUIRE(info.halfmoveClock == clock);

	const Move move = ai->GetMove(SearchLimits::fixed_depth(4)).best_move;

	CHECK_FALSE(move.is_null());
	CHECK(as_perplex(ai).GetLastResult().nodes_searched > 0);
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
	auto ai = make_tactical_engine(board, 4);

	const Move move = ai->GetMove(SearchLimits::fixed_depth(4)).best_move;

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
	auto ai = make_tactical_engine(board, 4);

	REQUIRE(board.halfmove_clock() == 99);

	const SearchResult result = ai->GetMove(SearchLimits::fixed_depth(4));

	REQUIRE_FALSE(result.best_move.is_null());
	CHECK(result.game_state == GameStates::STILL_PLAYING);

	// Replayed on a fresh board so the check stays independent of anything the search
	// may come to write back through the board it holds by reference.
	Board replay("4k3/8/8/8/8/8/1R6/4K3 w - - 99 60");
	REQUIRE(replay.IsLegalMove(result.best_move));
	REQUIRE(replay.DoMove(result.best_move));
	CHECK(replay.halfmove_clock() == HALFMOVE_CLOCK_LIMIT);
}
