// GameLoopTests.cpp — Catch2 tests for Game::Run()'s outcome handling.
//
// Run() is where a game actually ends: it commits the mover's move, adopts the state that mover
// reported, adjudicates the fifty-move rule on the position that results, and decides whether to
// keep going. None of that is reachable from a search test, and self-play cannot reach it either —
// it cannot deterministically arrive at 99 -> 100, and it can never produce HUMAN_EXITED.
//
// The players here are scripted: each returns a prepared SearchResult per call. That makes every
// outcome path a deterministic three-line test.

#include <catch_amalgamated.hpp>
#include "Game.h"
#include "IPlayer.h"
#include "Board.h"
#include "MoveFactory.h"
#include "SearchResult.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// TestAccess is a nested class, so it reaches Game's private members without a friend
// declaration. It exposes only what a loop test needs: construction, the run, and the outcome.
struct Game::TestAccess {
	static std::unique_ptr<Game> Make(const std::string& fen, std::unique_ptr<IPlayer> white,
	                                  std::unique_ptr<IPlayer> black)
	{
		return std::unique_ptr<Game>(new Game(fen, std::move(white), std::move(black)));
	}

	static GameStates State(const Game& game) noexcept { return game.game_state_; }
	static size_t MovesPlayed(const Game& game) noexcept { return game.m_GameMoves.size(); }
	static const Board& GameBoard(const Game& game) noexcept { return game.board_; }
};

namespace {

	// Returns a prepared result per call. Running off the end returns a null move with the game
	// still playing, which Run() treats as "the human left" — so a test that scripts too few
	// results terminates instead of looping forever.
	class ScriptedPlayer final : public IPlayer {
	  public:
		explicit ScriptedPlayer(std::vector<SearchResult> script) : script_(std::move(script)) {}

		SearchResult GetMove(const SearchLimits&) override
		{
			if (calls_ >= script_.size())
				return SearchResult{};
			return script_[calls_++];
		}

		const char* GetType() const override { return "Scripted"; }
		std::string getDescription() const override { return "scripted test player"; }
		int GetBestScore() const override { return 0; }
		bool IsHuman() const override { return false; }

		size_t calls() const noexcept { return calls_; }

	  private:
		std::vector<SearchResult> script_;
		size_t calls_{0};
	};

	std::unique_ptr<IPlayer> scripted(std::vector<SearchResult> script)
	{
		return std::make_unique<ScriptedPlayer>(std::move(script));
	}

	// A player that is never expected to be asked for a move.
	std::unique_ptr<IPlayer> silent() { return scripted({}); }

	// KR vs K: no pawn move and no capture is available, so any move raises the halfmove clock.
	constexpr const char* KR_VS_K_CLOCK_99 = "4k3/8/8/8/8/8/1R6/4K3 w - - 99 60";
	constexpr const char* KR_VS_K_CLOCK_120 = "4k3/8/8/8/8/8/1R6/4K3 w - - 120 60";

	SearchResult quiet_rook_move()
	{
		return {.best_move = MoveFactory::MakeMove(b2, b3, MoveType::QUIET)};
	}

} // namespace

TEST_CASE("Game: the move that takes the halfmove clock to the limit ends the game as a draw", "[game]")
{
	// The transition the search can no longer see: at 99 the position is still playing, and the
	// draw is a fact about the position only after the move is committed.
	auto game = Game::TestAccess::Make(KR_VS_K_CLOCK_99, scripted({quiet_rook_move()}), silent());

	game->Run();

	CHECK(Game::TestAccess::State(*game) == GameStates::DRAW_50_MOVES);
	CHECK(Game::TestAccess::MovesPlayed(*game) == 1);
	CHECK(Game::TestAccess::GameBoard(*game).halfmove_clock() == HALFMOVE_CLOCK_LIMIT);
}

TEST_CASE("Game: a clock below the limit keeps the game running", "[game]")
{
	// The control for the case above. Without it, a Run() that drew unconditionally would pass.
	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 50 60", scripted({quiet_rook_move()}), silent());

	game->Run();

	// Black is scripted with nothing, so the loop stops there rather than on an adjudication.
	CHECK(Game::TestAccess::State(*game) == GameStates::STILL_PLAYING);
	CHECK(Game::TestAccess::MovesPlayed(*game) == 1);
}

TEST_CASE("Game: a mate reported at the root ends the game", "[game]")
{
	// A search that finds mate at its own root returns no move and says so in game_state.
	auto game = Game::TestAccess::Make(
	    "4k3/8/8/8/8/8/1R6/4K3 w - - 5 60",
	    scripted({SearchResult{.game_state = GameStates::WHITE_WON}}), silent());

	game->Run();

	CHECK(Game::TestAccess::State(*game) == GameStates::WHITE_WON);
	CHECK(Game::TestAccess::MovesPlayed(*game) == 0);
}

TEST_CASE("Game: a stalemate reported at the root ends the game", "[game]")
{
	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60",
	                                   scripted({SearchResult{.game_state = GameStates::DRAW_PAT}}), silent());

	game->Run();

	CHECK(Game::TestAccess::State(*game) == GameStates::DRAW_PAT);
	CHECK(Game::TestAccess::MovesPlayed(*game) == 0);
}

TEST_CASE("Game: a human leaving ends the game", "[game]")
{
	// HUMAN_EXITED reaches Game as a returned state now that the state-changed event is gone.
	// Self-play can never produce this one.
	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60",
	                                   scripted({SearchResult{.game_state = GameStates::HUMAN_EXITED}}), silent());

	game->Run();

	CHECK(Game::TestAccess::State(*game) == GameStates::HUMAN_EXITED);
	CHECK(Game::TestAccess::MovesPlayed(*game) == 0);
}

TEST_CASE("Game: an already-high clock cannot overwrite an outcome the mover reported", "[game]")
{
	// The precondition on the fifty-move check. A position loaded from a FEN whose clock is past
	// the limit must not turn a mate, a stalemate or a human leaving into a draw.
	const GameStates reported = GENERATE(GameStates::WHITE_WON, GameStates::BLACK_WON, GameStates::DRAW_PAT,
	                                     GameStates::HUMAN_EXITED);
	INFO("reported state: " << static_cast<int>(reported));

	auto game = Game::TestAccess::Make(KR_VS_K_CLOCK_120, scripted({SearchResult{.game_state = reported}}), silent());

	game->Run();

	CHECK(Game::TestAccess::State(*game) == reported);
}

TEST_CASE("Game: an already-high clock still draws when the mover reports nothing", "[game]")
{
	// The other half of the precondition: the guard is about precedence, not about suppressing
	// the draw entirely.
	auto game = Game::TestAccess::Make(KR_VS_K_CLOCK_120, scripted({quiet_rook_move()}), silent());

	game->Run();

	CHECK(Game::TestAccess::State(*game) == GameStates::DRAW_50_MOVES);
	CHECK(Game::TestAccess::MovesPlayed(*game) == 1);
}

TEST_CASE("Game: play alternates and each mover's result is the one acted on", "[game]")
{
	// Two plies, so the retained-mover ordering in Run() is exercised: white's move is committed
	// before black is asked, and black's reported state is the one that ends the game.
	auto white = scripted({quiet_rook_move()});
	auto black = scripted({SearchResult{.game_state = GameStates::BLACK_WON}});

	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60", std::move(white), std::move(black));

	game->Run();

	CHECK(Game::TestAccess::State(*game) == GameStates::BLACK_WON);
	CHECK(Game::TestAccess::MovesPlayed(*game) == 1);
}
