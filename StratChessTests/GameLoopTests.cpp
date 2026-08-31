// GameLoopTests.cpp — Catch2 tests for Game::Run()'s outcome handling.
//
// Run() is where a game actually ends: it commits the mover's move, adopts the state that mover
// reported, adjudicates the fifty-move rule on the position that results, and decides whether to
// keep going. None of that is reachable from a search test, and self-play cannot reach it either —
// it cannot deterministically arrive at 99 -> 100, and it can never produce a resignation.
//
// The players here are scripted: each returns a prepared SearchResult per call. That makes every
// outcome path a deterministic three-line test.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "Game.h"
#include "IPlayer.h"
#include "Board.h"
#include "MoveFactory.h"
#include "SearchResult.h"
#include "Utils/Logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
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

	SearchResult quiet_rook_move() { return {.best_move = MoveFactory::MakeMove(b2, b3, MoveType::QUIET)}; }

	// Collects everything logged while it is alive. Run()'s state message goes to the default
	// logger and nowhere else, so reading it back is the only way to see which channel the
	// printed score came from.
	class CapturingSink final : public spdlog::sinks::base_sink<std::mutex> {
	  public:
		std::string text()
		{
			std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
			return text_;
		}

	  protected:
		void sink_it_(const spdlog::details::log_msg& msg) override
		{
			spdlog::memory_buf_t formatted;
			base_sink<std::mutex>::formatter_->format(msg, formatted);
			text_.append(formatted.data(), formatted.size());
		}

		void flush_() override {}

	  private:
		std::string text_;
	};

	// Attaches a CapturingSink to the default logger and takes it off again on scope exit —
	// the suite-wide listener in StratChessTests.cpp owns that sink list, so a test that left
	// its own sink behind would keep capturing every later test's output.
	class ScopedLogCapture {
	  public:
		ScopedLogCapture() : sink_(std::make_shared<CapturingSink>())
		{
			spdlog::default_logger()->sinks().push_back(sink_);
		}

		~ScopedLogCapture()
		{
			auto& sinks = spdlog::default_logger()->sinks();
			sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
		}

		std::string text() const { return sink_->text(); }

		ScopedLogCapture(const ScopedLogCapture&) = delete;
		ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
		ScopedLogCapture(ScopedLogCapture&&) = delete;
		ScopedLogCapture& operator=(ScopedLogCapture&&) = delete;

	  private:
		std::shared_ptr<CapturingSink> sink_;
	};

	// Runs the real Game -> PerfStats logger path. The logger is deliberately created here rather
	// than reached through a test-only Game getter: Game's production boundary is responsible for
	// choosing whether an existing perf logger receives the completed move's telemetry.
	class ScopedPerfLogCapture {
	  public:
		ScopedPerfLogCapture()
		    : perf_(Engine::Logger::EnsurePerfLogger("logs/GameLoopPerfStats.txt")),
		      sink_(std::make_shared<CapturingSink>())
		{
			REQUIRE(perf_);
			sink_->set_pattern("%v");
			perf_->sinks().push_back(sink_);
		}

		~ScopedPerfLogCapture()
		{
			if (!perf_)
				return;
			auto& sinks = perf_->sinks();
			sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
		}

		std::string text() const { return sink_->text(); }

		ScopedPerfLogCapture(const ScopedPerfLogCapture&) = delete;
		ScopedPerfLogCapture& operator=(const ScopedPerfLogCapture&) = delete;
		ScopedPerfLogCapture(ScopedPerfLogCapture&&) = delete;
		ScopedPerfLogCapture& operator=(ScopedPerfLogCapture&&) = delete;

	  private:
		std::shared_ptr<spdlog::logger> perf_;
		std::shared_ptr<CapturingSink> sink_;
	};

	std::vector<std::string> fields_in_last_line(const std::string& text)
	{
		const size_t line_start = text.find_last_of('\n', text.size() - 2);
		std::istringstream line(line_start == std::string::npos ? text : text.substr(line_start + 1));
		std::vector<std::string> fields;
		std::string field;
		while (line >> field)
			fields.push_back(field);
		return fields;
	}

	bool contains(const std::string& haystack, const std::string& needle)
	{
		return haystack.find(needle) != std::string::npos;
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
	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60",
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

TEST_CASE("Game: a resignation ends the game", "[game]")
{
	// A resignation reaches Game as a returned state now that the state-changed event is gone.
	// Self-play can never produce this one. Both sides are covered because the state names the
	// resigning side, and only the mover can resign.
	const GameStates resigned = GENERATE(GameStates::WHITE_RESIGNED, GameStates::BLACK_RESIGNED);
	INFO("resigned state: " << static_cast<int>(resigned));

	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60",
	                                   scripted({SearchResult{.game_state = resigned}}), silent());

	game->Run();

	CHECK(Game::TestAccess::State(*game) == resigned);
	CHECK(Game::TestAccess::MovesPlayed(*game) == 0);
}

TEST_CASE("Game: an already-high clock cannot overwrite an outcome the mover reported", "[game]")
{
	// The precondition on the fifty-move check. A position loaded from a FEN whose clock is past
	// the limit must not turn a mate, a stalemate or a resignation into a draw.
	const GameStates reported =
	    GENERATE(GameStates::WHITE_WON, GameStates::BLACK_WON, GameStates::DRAW_PAT, GameStates::WHITE_RESIGNED,
	             GameStates::BLACK_RESIGNED);
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

TEST_CASE("Game: the printed score is the mover's returned score", "[game]")
{
	// The state message reads the score out of the SearchResult that GetMove() returned.
	constexpr int REPORTED_SCORE = 4242;

	auto white = scripted(
	    {SearchResult{.best_move = MoveFactory::MakeMove(b2, b3, MoveType::QUIET), .best_score = REPORTED_SCORE}});

	const ScopedLogCapture capture;
	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60", std::move(white), silent());

	game->Run();

	const std::string log = capture.text();
	CHECK(contains(log, "Score: " + std::to_string(REPORTED_SCORE)));
}

TEST_CASE("Game: a mate score in the mover's result is reported as a mate, not a number", "[game]")
{
	// The other branch of the same message, fed from the same channel: the distance to mate it
	// prints is computed from the returned score.
	const SearchResult mate_in_three{.best_move = MoveFactory::MakeMove(b2, b3, MoveType::QUIET),
	                                 .best_score = GameValues::Mate - 5};

	const ScopedLogCapture capture;
	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60", scripted({mate_in_three}), silent());

	game->Run();

	CHECK(contains(capture.text(), "Check mate in 3 moves"));
}

TEST_CASE("Game: perf stats retain six current-and-cumulative result fields", "[game_loop]")
{
	// Catches move stats that remain in PlayerAiBase, a Game loop that never accumulates returned
	// telemetry, and any regression to a per-player rather than combined-player total.
	const SearchResult first{.best_move = MoveFactory::MakeMove(b2, b3, MoveType::QUIET),
	                         .nodes_searched = 13,
	                         .qnodes_searched = 5,
	                         .elapsed = std::chrono::milliseconds(2)};
	const SearchResult second{.game_state = GameStates::BLACK_WON,
	                          .nodes_searched = 7,
	                          .qnodes_searched = 11,
	                          .elapsed = std::chrono::milliseconds(3)};

	const ScopedPerfLogCapture capture;
	auto game = Game::TestAccess::Make("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60", scripted({first}), scripted({second}));
	game->Run();

	const std::vector<std::string> fields = fields_in_last_line(capture.text());
	REQUIRE(fields.size() == 6);
	CHECK(fields[0] == std::to_string(second.nodes_searched + second.qnodes_searched));
	CHECK(fields[1] == std::to_string(second.elapsed.count()));
	CHECK(fields[2] == "6");
	CHECK(fields[3] == std::to_string(first.nodes_searched + first.qnodes_searched + second.nodes_searched +
	                                  second.qnodes_searched));
	CHECK(fields[4] == std::to_string(first.elapsed.count() + second.elapsed.count()));
	CHECK(fields[5] == "7");
}
