// PlayerHumanTests.cpp — Catch2 tests for PlayerHuman's non-interactive paths.
//
// PlayerHuman::GetMove() normally blocks on std::cin, which no automated test can drive. Its
// terminal paths never get that far: a position with no legal move returns before the prompt,
// which makes them the one part of this player a test can reach — and the part that matters, since
// a returned SearchResult is now the only way a game ends.

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "GameState.h"
#include "PlayerBase.h"
#include "SearchLimits.h"
#include "SearchResult.h"
#include "defines.h"

#include <memory>

namespace {

	std::unique_ptr<PlayerBase> human(Board& board)
	{
		// Depth is a don't-care: Create() passes it to the AIs and drops it for a human.
		return PlayerBase::Create(PlayerBase::ePlayerTypes::HUMAN, 1, board);
	}

} // namespace

TEST_CASE("PlayerHuman - a mated position returns no move and names the winner", "[player_human]")
{
	// Black to move and mated: Ra8 covers the back rank, f7/g7/h7 block every escape. No legal
	// move exists, so GetMove() returns without ever reading std::cin.
	Board board("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
	const auto player = human(board);

	const SearchResult result = player->GetMove(SearchLimits{});

	CHECK(result.best_move.is_null());
	CHECK(result.game_state == GameStates::WHITE_WON);
}

TEST_CASE("PlayerHuman - a stalemated position returns no move and DRAW_PAT", "[player_human]")
{
	// Black to move, not in check, every king move covered: Qf7 takes g8, g7 and h7.
	Board board("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
	const auto player = human(board);

	const SearchResult result = player->GetMove(SearchLimits{});

	CHECK(result.best_move.is_null());
	CHECK(result.game_state == GameStates::DRAW_PAT);
}

TEST_CASE("PlayerHuman - the mated side reports a losing score", "[player_human]")
{
	// The score travels in the result too, which is what Game prints. Checked separately from
	// the state so a result that carried the state alone does not pass as complete.
	Board board("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
	const auto player = human(board);

	CHECK(player->GetMove(SearchLimits{}).best_score == -GameValues::Mate);
}
