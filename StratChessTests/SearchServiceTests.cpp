// SearchServiceTests.cpp — Catch2 tests for the AIPerplex service surface: what the player
// factory builds and configures, what one Search() call promises about the board and the
// observer it was handed, and what StartNewGame() resets.

#include "SearchTestFixture.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp> // REQUIRE_THROWS_WITH
#include "AIPerplex.h"
#include "Board.h"
#include "MoveFactory.h"
#include "PlayerFactory.h"
#include "PlayerBase.h"
#include "SearchPlayer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

namespace {
	class ScopedFactoryLogCapture {
	  public:
		ScopedFactoryLogCapture() : sink_(std::make_shared<spdlog::sinks::ostream_sink_mt>(output_))
		{
			sink_->set_pattern("%v");
			spdlog::default_logger()->sinks().push_back(sink_);
		}

		~ScopedFactoryLogCapture()
		{
			auto& sinks = spdlog::default_logger()->sinks();
			sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
		}

		std::string text() const { return output_.str(); }

	  private:
		std::ostringstream output_;
		std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
	};
} // namespace

TEST_CASE("SearchPlayer searches the Board's current position on every move", "[player][search_player]")
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	Config::PlayerConfig config;
	config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::AI_PERPLEX);
	config.depth = 1;
	config.eval = static_cast<unsigned>(EvalManager::EvalTypes::COMPLEX);
	auto player = CreatePlayer(config, board, {.verbose_search_logging = false});

	const SearchResult first = player->GetMove(SearchLimits::fixed_depth(1));
	const Move e2e4 = MoveFactory::MakeMove(e2, e4, MoveType::DOUBLE_PAWN_PUSH);
	REQUIRE(board.DoMove(e2e4));
	board.ResetSearchDepth();
	const SearchResult second = player->GetMove(SearchLimits::fixed_depth(1));

	CHECK_FALSE(first.best_move.is_null());
	CHECK_FALSE(second.best_move.is_null());
	CHECK(board.IsLegalMove(second.best_move));
}

TEST_CASE("Player factory creates human and legacy players", "[player][factory]")
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

	Config::PlayerConfig human_config;
	human_config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::HUMAN);
	auto human = CreatePlayer(human_config, board);
	CHECK(human->IsHuman());
	CHECK(std::string(human->GetType()) == "Human");

	Config::PlayerConfig legacy_config;
	legacy_config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::AIAGENT);
	legacy_config.depth = 1;
	legacy_config.eval = static_cast<unsigned>(EvalManager::EvalTypes::SIMPLE);
	auto legacy = CreatePlayer(legacy_config, board);
	const SearchResult result = legacy->GetMove(SearchLimits::fixed_depth(1));
	CHECK_FALSE(legacy->IsHuman());
	CHECK(std::string(legacy->GetType()) == "AI Agent");
	CHECK_FALSE(result.best_move.is_null());
	CHECK(board.IsLegalMove(result.best_move));
}

TEST_CASE("AIPerplex rejects the NONE evaluator at construction", "[search][service_api][factory]")
{
	const AIPerplexConfig config{.evaluator = EvalManager::EvalTypes::NONE};
	REQUIRE_THROWS_WITH(AIPerplex(config), "AIPerplex requires a SIMPLE or COMPLEX evaluator");
}

TEST_CASE("Player factory rejects NONE for an AIPerplex player", "[player][search_player][factory]")
{
	Board board;
	Config::PlayerConfig config;
	config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::AI_PERPLEX);
	config.eval = static_cast<unsigned>(EvalManager::EvalTypes::NONE);

	REQUIRE_THROWS_WITH(CreatePlayer(config, board), "AIPerplex requires a SIMPLE or COMPLEX evaluator");
}

TEST_CASE("Player factory configures the default depth used by empty SearchLimits", "[player][search_player][factory]")
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	Config::PlayerConfig config;
	config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::AI_PERPLEX);
	config.depth = 2;
	config.eval = static_cast<unsigned>(EvalManager::EvalTypes::COMPLEX);
	auto player = CreatePlayer(config, board);

	const SearchResult result = player->GetMove(SearchLimits{});

	REQUIRE_FALSE(result.best_move.is_null());
	REQUIRE(result.depth_completed == 2);
}

TEST_CASE("Player factory warns when search tuning is supplied to a legacy AI", "[player][factory]")
{
	Board board;
	Config::PlayerConfig config;
	config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::AIAGENT);
	config.depth = 1;
	config.eval = static_cast<unsigned>(EvalManager::EvalTypes::COMPLEX);
	config.search_tuning = Config::SearchTuningConfig{};

	const ScopedFactoryLogCapture capture;
	auto player = CreatePlayer(config, board);

	REQUIRE(player != nullptr);
	REQUIRE(capture.text().find("search_tuning in game_settings.json is ignored for player type 3") !=
	        std::string::npos);
}

TEST_CASE("AIPerplex Search uses the board supplied for each call and does not retain observers",
          "[search][service_api]")
{
	Board first_board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	Board second_board("6k1/5ppp/8/8/8/4R3/5PPP/6K1 w - - 0 1");
	AIPerplex ai(AIPerplexConfig{.default_depth = 2, .verbose_logging = false});

	int first_observations = 0;
	const auto first =
	    ai.Search(first_board, SearchLimits::fixed_depth(2), [&](const IterationInfo&) { ++first_observations; });
	const int first_observations_after_first_search = first_observations;
	int second_observations = 0;
	const auto second =
	    ai.Search(second_board, SearchLimits::fixed_depth(2), [&](const IterationInfo&) { ++second_observations; });
	const int second_observations_after_second_search = second_observations;
	CHECK(first_observations == first_observations_after_first_search);
	const auto third = ai.Search(first_board, SearchLimits::fixed_depth(2));

	CHECK_FALSE(first.best_move.is_null());
	CHECK_FALSE(second.best_move.is_null());
	CHECK(first_board.IsLegalMove(first.best_move));
	CHECK(second_board.IsLegalMove(second.best_move));
	CHECK(second.best_move.from() == e3);
	CHECK(second.best_move.to() == e8);
	CHECK(first_observations > 0);
	CHECK(second_observations > 0);
	CHECK(first_observations == first_observations_after_first_search);
	CHECK(second_observations == second_observations_after_second_search);
	CHECK_FALSE(third.best_move.is_null());
}

TEST_CASE("AIPerplex clears launch state when an iteration observer throws", "[search][service_api]")
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	AIPerplex ai(AIPerplexConfig{.default_depth = 2, .threads = 2, .verbose_logging = false});

	const auto failed_search_started = std::chrono::steady_clock::now();
	REQUIRE_THROWS_AS(ai.Search(board, SearchLimits::fixed_depth(50),
	                            [](const IterationInfo&) { throw std::runtime_error("observer failure"); }),
	                  std::runtime_error);
	CHECK(std::chrono::steady_clock::now() - failed_search_started < std::chrono::seconds(2));

	// A stop arriving after the failed call belongs to no search. It must not be
	// remembered by a stale launch handshake and poison the next call.
	ai.Stop();
	const SearchResult recovered = ai.Search(board, SearchLimits::fixed_depth(2));

	CHECK_FALSE(recovered.best_move.is_null());
	CHECK(recovered.depth_completed == 2);
}

TEST_CASE("AIPerplexConfig selects the evaluator used by Search", "[search][service_api]")
{
	Board board("4k3/pp6/8/8/8/P7/P7/4K3 w - - 0 1");
	AIPerplex simple(AIPerplexConfig{.evaluator = EvalManager::EvalTypes::SIMPLE, .default_depth = 1});
	AIPerplex complex(AIPerplexConfig{.evaluator = EvalManager::EvalTypes::COMPLEX, .default_depth = 1});

	const SearchResult simple_result = simple.Search(board, SearchLimits::fixed_depth(1));
	const SearchResult complex_result = complex.Search(board, SearchLimits::fixed_depth(1));

	CHECK_FALSE(simple_result.best_move.is_null());
	CHECK_FALSE(complex_result.best_move.is_null());
	CHECK(simple_result.best_score != complex_result.best_score);
}

TEST_CASE("Player factory maps AIPerplex evaluator tuning threads and logging before erasure",
          "[player][search_player][factory]")
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	Config::PlayerConfig config;
	config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::AI_PERPLEX);
	config.depth = 2;
	config.eval = static_cast<unsigned>(EvalManager::EvalTypes::SIMPLE);
	config.threads = 3;
	config.search_tuning = Config::SearchTuningConfig{.min_nodes_threshold = 17,
	                                                  .min_completion_ratio = 0.21,
	                                                  .min_pv_ratio = 0.45,
	                                                  .score_draw_threshold = 23,
	                                                  .delta_pruning_margin = 211,
	                                                  .aspiration_initial_delta = 61,
	                                                  .aspiration_max_retries = 7,
	                                                  .aspiration_enabled = false};

	auto player = CreatePlayer(config, board, {.verbose_search_logging = true});
	AIPerplex& search = SearchPlayerTestFixture::search(*player);
	const SearchTuning& tuning = AIPerlexTestFixture::tuning(search);

	CHECK(AIPerlexTestFixture::evaluator_type(search) == "Simple");
	CHECK(AIPerlexTestFixture::configured_threads(search) == 3);
	CHECK(AIPerlexTestFixture::verbose_logging(search));
	CHECK(tuning.min_nodes_threshold == 17);
	CHECK(tuning.min_completion_ratio == 0.21);
	CHECK(tuning.min_pv_ratio == 0.45);
	CHECK(tuning.score_draw_threshold == 23);
	CHECK(tuning.delta_pruning_margin == 211);
	CHECK(tuning.aspiration_initial_delta == 61);
	CHECK(tuning.aspiration_max_retries == 7);
	CHECK_FALSE(tuning.aspiration_enabled);
	CHECK(player->getDescription() ==
	      "\n\tEngine type:\tPerplexity Transpositional AlphaBeta\n\tDepth:\t\t2\n\tEvaluation:\tSimple\n");
}

TEST_CASE("Player factory starts the AIPerplex new-game lifecycle before returning", "[player][search_player][factory]")
{
	Board board;
	Config::PlayerConfig config;
	config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::AI_PERPLEX);
	config.eval = static_cast<unsigned>(EvalManager::EvalTypes::COMPLEX);
	auto player = CreatePlayer(config, board);

	CHECK(AIPerlexTestFixture::game_generation(SearchPlayerTestFixture::search(*player)) == 1);
}

TEST_CASE("AIPerplex Stop does not abort the next direct Search", "[search][service_api]")
{
	Board board("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2");
	AIPerplex ai(AIPerplexConfig{.default_depth = 50, .verbose_logging = false});
	std::atomic<bool> accepted_iteration{false};
	SearchResult stopped_result;

	std::jthread search_thread([&] {
		stopped_result = ai.Search(board, SearchLimits::fixed_depth(50), [&](const IterationInfo&) {
			accepted_iteration.store(true, std::memory_order_release);
		});
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!accepted_iteration.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();

	ai.Stop();
	search_thread.join();
	REQUIRE(accepted_iteration.load(std::memory_order_acquire));
	CHECK_FALSE(stopped_result.best_move.is_null());

	const SearchResult next = ai.Search(board, SearchLimits::fixed_depth(2));
	CHECK_FALSE(next.best_move.is_null());
	CHECK(next.depth_completed == 2);
}

TEST_CASE("AIPerplex verbosity configuration is isolated per engine", "[search][service_api]")
{
	AIPerplex quiet(AIPerplexConfig{.verbose_logging = false});
	REQUIRE_FALSE(AIPerlexTestFixture::verbose_logging(quiet));

	AIPerplex verbose(AIPerplexConfig{.verbose_logging = true});
	CHECK_FALSE(AIPerlexTestFixture::verbose_logging(quiet));
	CHECK(AIPerlexTestFixture::verbose_logging(verbose));
}

// ============================================================================
// New-game lifecycle tests
// ============================================================================

TEST_CASE("Search - StartNewGame clears a populated AIPerplex TT", "[search][tt]")
{
	AIPerlexTestFixture fix;
	fix.store_tt_marker();
	REQUIRE(fix.has_tt_marker());

	fix.start_new_game();

	REQUIRE_FALSE(fix.has_tt_marker());
}

TEST_CASE("Search - fullmove-one position does not define TT lifetime", "[search][tt]")
{
	AIPerlexTestFixture fix;
	fix.store_tt_marker();

	fix.search_depth_one();

	REQUIRE(fix.has_tt_marker());
}

TEST_CASE("Search - StartNewGame resets td_ history and killers", "[search]")
{
	AIPerlexTestFixture fix;
	fix.poke_history();
	fix.poke_killer(0);
	REQUIRE_FALSE(fix.history_is_clear());
	REQUIRE(fix.has_killer(0));

	fix.start_new_game();

	REQUIRE(fix.history_is_clear());
	REQUIRE_FALSE(fix.has_killer(0));
}

TEST_CASE("Search - StartNewGame clears helper_tds_", "[search][smp]")
{
	// Lazy SMP helpers are reused across searches within a game (GetMove()
	// only grows helper_tds_, never shrinks it) — StartNewGame() must clear
	// the vector so the next search reconstructs them fresh instead of
	// carrying killers/history over from the previous game.
	AIPerlexTestFixture fix;
	fix.add_fake_helper();
	REQUIRE(fix.helper_count() == 1);

	fix.start_new_game();

	REQUIRE(fix.helper_count() == 0);
}

TEST_CASE("Search - StartNewGame does not reset tuning_", "[search]")
{
	// Regression: Game::SetPlayerParams() applies game_settings.json's
	// search_tuning overrides and then unconditionally calls StartNewGame()
	// on the same object -- if StartNewGame() reset tuning_, every configured
	// override would be silently discarded before the first move is searched.
	AIPerlexTestFixture fix;
	fix.set_null_move_enabled(false);

	fix.start_new_game();

	REQUIRE_FALSE(fix.null_move_enabled());
}
