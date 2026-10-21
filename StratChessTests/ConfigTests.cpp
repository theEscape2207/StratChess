#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "Config.h"
#include "Board.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

	// Config's own fallback-to-defaults path reports on stderr (deliberately, for a real
	// user running the CLI). The test exercising it only asserts REQUIRE_NOTHROW, so
	// without this the message leaks into every passing test run's console output.
	class CerrRedirect {
	  public:
		CerrRedirect() : old_(std::cerr.rdbuf(buffer_.rdbuf())) {}
		~CerrRedirect()
		{
			try {
				std::cerr.rdbuf(old_);
			} catch (...) { // NOLINT(bugprone-empty-catch) - restoring cerr in a destructor
			}
		}

		CerrRedirect(const CerrRedirect&) = delete;
		CerrRedirect& operator=(const CerrRedirect&) = delete;

	  private:
		std::ostringstream buffer_;
		std::streambuf* old_;
	};

	// Writes a settings document to a uniquely named temp file and removes it
	// afterwards, so the cases cannot collide when the suite runs concurrently.
	class TempConfig {
	  public:
		explicit TempConfig(std::string_view contents)
		    : path_(std::filesystem::temp_directory_path() /
		            ("strat_cfg_" + std::to_string(++counter_) + "_" +
		             std::to_string(
		                 static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())) +
		             ".json"))
		{
			std::ofstream out(path_);
			out << contents;
		}
		~TempConfig()
		{
			std::error_code ec;
			std::filesystem::remove(path_, ec);
		}

		TempConfig(const TempConfig&) = delete;
		TempConfig& operator=(const TempConfig&) = delete;

		[[nodiscard]] std::string path() const { return path_.string(); }

	  private:
		std::filesystem::path path_;
		static inline int counter_ = 0;
	};

	Config MakeReader() { return Config(); }

} // namespace

TEST_CASE("Config: a well-formed document parses", "[config]")
{
	// Positive control. Without it the negative cases below could all pass by
	// throwing for some unrelated reason.
	TempConfig cfg(R"({
        "game": { "players": {
            "white": { "type": 1, "search_limits": { "depth": 7 } },
            "black": { "type": 1, "search_limits": { "depth": 3 } }
        } }
    })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));

	REQUIRE(reader.GetPlayerFromConfig(true).search_limits.depth == 7);
	REQUIRE(reader.GetPlayerFromConfig(false).search_limits.depth == 3);
}

TEST_CASE("Config: every search_limits key and threads reach PlayerConfig", "[config]")
{
	TempConfig cfg(R"({
        "game": { "players": {
            "white": { "type": 1, "search_limits": { "movetime": 2500, "infinite": true }, "threads": 3 },
            "black": { "type": 1, "search_limits": {
                "clock": { "remaining": 60000, "increment": 500, "moves_to_go": 20 } } }
        } }
    })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));

	const auto white = reader.GetPlayerFromConfig(true);
	CHECK(white.search_limits.movetime == std::chrono::milliseconds(2500));
	CHECK(white.search_limits.infinite);
	CHECK_FALSE(white.search_limits.clock.has_value());
	CHECK(white.threads == 3u);

	const auto black = reader.GetPlayerFromConfig(false);
	REQUIRE(black.search_limits.clock.has_value());
	CHECK(black.search_limits.clock->remaining == std::chrono::milliseconds(60000));
	CHECK(black.search_limits.clock->increment == std::chrono::milliseconds(500));
	CHECK(black.search_limits.clock->moves_to_go == 20);
	CHECK_FALSE(black.search_limits.movetime.has_value());
	CHECK_FALSE(black.search_limits.infinite);
	CHECK_FALSE(black.threads.has_value());
}

TEST_CASE("Config: a Search player needs a search_limits block, a Human player does not", "[config]")
{
	Board board;

	SECTION("Search player without the block is rejected naming the player and the key")
	{
		TempConfig cfg(R"({ "game": { "players": {
            "white": { "type": 0 },
            "black": { "type": 1, "threads": 2 } } } })");
		Config reader = MakeReader();
		REQUIRE_THROWS_WITH(reader.ReadConfigFile(cfg.path(), board),
		                    Catch::Matchers::ContainsSubstring("players.black") &&
		                        Catch::Matchers::ContainsSubstring("search_limits"));
	}
	SECTION("an absent type is a Search player")
	{
		TempConfig cfg(R"({ "game": { "players": { "white": {}, "black": { "type": 0 } } } })");
		Config reader = MakeReader();
		REQUIRE_THROWS_WITH(reader.ReadConfigFile(cfg.path(), board),
		                    Catch::Matchers::ContainsSubstring("players.white"));
	}
	SECTION("Human players without the block are accepted")
	{
		TempConfig cfg(R"({ "game": { "players": { "white": { "type": 0 }, "black": { "type": 0 } } } })");
		Config reader = MakeReader();
		REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));
		CHECK(reader.GetPlayerFromConfig(true).type == 0u);
	}
}

TEST_CASE("Config: every search_tuning key reaches SearchTuning", "[config]")
{
	TempConfig cfg(R"({
        "game": { "players": {
            "white": { "type": 1, "search_limits": {}, "search_tuning": {
                "min_nodes_threshold": 17,
                "min_completion_ratio": 0.21,
                "min_pv_ratio": 0.45,
                "delta_pruning_margin": 211,
                "aspiration_initial_delta": 61,
                "aspiration_max_retries": 7,
                "aspiration_enabled": false,
                "lmr_min_depth": 5,
                "lmr_min_move_index": 6,
                "lmr_enabled": false,
                "null_move_enabled": false,
                "null_move_reduction": 4,
                "null_move_min_depth": 7,
                "see_pruning_enabled": false,
                "singular_extensions_enabled": false,
                "reverse_futility_enabled": false,
                "reverse_futility_max_depth": 5,
                "reverse_futility_margin": 150,
                "frontier_futility_enabled": false,
                "frontier_futility_margin": 250,
                "late_move_pruning_enabled": false,
                "contempt": 25
            } },
            "black": { "type": 1, "search_limits": {} }
        } }
    })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));

	const auto tuning = reader.GetPlayerFromConfig(true).search_tuning;
	REQUIRE(tuning.has_value());
	CHECK(tuning->min_nodes_threshold == 17);
	CHECK(tuning->min_completion_ratio == 0.21);
	CHECK(tuning->min_pv_ratio == 0.45);
	CHECK(tuning->delta_pruning_margin == 211);
	CHECK(tuning->aspiration_initial_delta == 61);
	CHECK(tuning->aspiration_max_retries == 7);
	CHECK_FALSE(tuning->aspiration_enabled);
	CHECK(tuning->lmr_min_depth == 5);
	CHECK(tuning->lmr_min_move_index == 6);
	CHECK_FALSE(tuning->lmr_enabled);
	CHECK_FALSE(tuning->null_move_enabled);
	CHECK(tuning->null_move_reduction == 4);
	CHECK(tuning->null_move_min_depth == 7);
	CHECK_FALSE(tuning->see_pruning_enabled);
	CHECK_FALSE(tuning->singular_extensions_enabled);
	CHECK_FALSE(tuning->reverse_futility_enabled);
	CHECK(tuning->reverse_futility_max_depth == 5);
	CHECK(tuning->reverse_futility_margin == 150);
	CHECK_FALSE(tuning->frontier_futility_enabled);
	CHECK(tuning->frontier_futility_margin == 250);
	CHECK_FALSE(tuning->late_move_pruning_enabled);
	CHECK(tuning->contempt == 25);

	// An absent block stays absent; the defaults then come from SearchTuning itself.
	CHECK_FALSE(reader.GetPlayerFromConfig(false).search_tuning.has_value());
}

TEST_CASE("Config: an empty search_tuning block yields the defaults", "[config]")
{
	TempConfig cfg(
	    R"({ "game": { "players": { "white": { "type": 1, "search_limits": {}, "search_tuning": {} }, "black": { "type": 1, "search_limits": {} } } } })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));

	REQUIRE(reader.GetPlayerFromConfig(true).search_tuning == SearchTuning{});
}

TEST_CASE("Config: an invalid search_tuning value is rejected naming the field", "[config]")
{
	TempConfig cfg(R"({ "game": { "players": {
        "white": { "type": 1, "search_limits": {}, "search_tuning": { "lmr_enabled": false, "reverse_futility_margin": 1001 } },
        "black": { "type": 1, "search_limits": {} } } } })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_THROWS_WITH(reader.ReadConfigFile(cfg.path(), board),
	                    Catch::Matchers::ContainsSubstring("search_tuning.reverse_futility_margin"));
}

TEST_CASE("Config: truncated JSON is reported, not silently accepted", "[config]")
{
	TempConfig cfg(R"({ "game": { "players": )");

	Board board;
	Config reader = MakeReader();
	REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board), nlohmann::json::parse_error);
}

TEST_CASE("Config: a missing key is an error, not undefined behaviour", "[config]")
{
	// These read through a CONST json&, where nlohmann's operator[] on an absent
	// key is UB rather than an error: Release carried on and threw something
	// unrelated later, while a Debug build asserted inside the library. .at()
	// makes every one of them a diagnosable out_of_range naming the key.
	Board board;

	SECTION("no \"game\"")
	{
		TempConfig cfg(R"({ "nothing": 1 })");
		Config reader = MakeReader();
		REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board), nlohmann::json::out_of_range);
	}
	SECTION("no \"players\"")
	{
		TempConfig cfg(R"({ "game": { "FEN": "" } })");
		Config reader = MakeReader();
		REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board), nlohmann::json::out_of_range);
	}
	SECTION("no \"black\"")
	{
		TempConfig cfg(R"({ "game": { "players": { "white": { "type": 1, "search_limits": {} } } } })");
		Config reader = MakeReader();
		REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board), nlohmann::json::out_of_range);
	}
}

TEST_CASE("Config: a key of the wrong type is reported", "[config]")
{
	// .value() tolerates a MISSING key but still throws on a present key of the
	// wrong type, which is the case this pins.
	TempConfig cfg(R"({
        "game": { "players": {
            "white": { "type": 1, "search_limits": { "depth": "five" } },
            "black": { "type": 1, "search_limits": {} }
        } }
    })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board), nlohmann::json::type_error);
}

TEST_CASE("Config: a non-positive search depth is rejected naming the key", "[config]")
{
	Board board;
	for (const char* depth : {"0", "-1"}) {
		TempConfig cfg(std::string(R"({ "game": { "players": {
            "white": { "type": 1, "search_limits": { "depth": )") +
		               depth + R"( } }, "black": { "type": 1, "search_limits": {} } } } })");
		Config reader = MakeReader();
		REQUIRE_THROWS_WITH(reader.ReadConfigFile(cfg.path(), board),
		                    Catch::Matchers::ContainsSubstring("search_limits.depth must be positive"));
	}
}

TEST_CASE("Config: comments are accepted", "[config]")
{
	// game_settings.json ships heavily commented, so ignore_comments must stay
	// on -- a regression here would break the shipped file, not just a test.
	TempConfig cfg(R"({
        /* block comment */
        "game": { "players": {
            "white": { "type": 1, "search_limits": { "depth": 5 } },
            "black": { "type": 1, "search_limits": {} }
        } }
    })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));
	REQUIRE(reader.GetPlayerFromConfig(true).search_limits.depth == 5);
}

TEST_CASE("Config: a missing file falls back to defaults without throwing", "[config]")
{
	const auto missing = (std::filesystem::temp_directory_path() / "strat_cfg_definitely_absent_178.json").string();
	REQUIRE_FALSE(std::filesystem::exists(missing));

	Board board;
	Config reader = MakeReader();
	// Reported on stderr, then defaults stand -- deliberately not an exception,
	// since running with defaults is a usable outcome and the message says so.
	CerrRedirect redirect;
	REQUIRE_NOTHROW(reader.ReadConfigFile(missing, board));
}

TEST_CASE("Config: a FEN setup is applied to the board", "[config]")
{
	// The FEN path is the one that used to reach back into Game. It now ends at the board,
	// which is what this asserts: the position, and the metadata that comes with it.
	TempConfig cfg(R"({
        "game": {
            "FEN": "r3k2r/8/8/4pP2/8/8/8/R3K2R w KQkq e6 7 12",
            "players": {
                "white": { "type": 1, "search_limits": {} },
                "black": { "type": 1, "search_limits": {} }
            }
        }
    })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));

	REQUIRE(board.GetCurrentColor() == eColor::WHITE);
	REQUIRE(board.GetPiece(e1) == ePiece::WHITE_KING);
	REQUIRE(board.GetPiece(e8) == ePiece::BLACK_KING);
	REQUIRE(board.ep_square() == e6);
	REQUIRE(board.castling_rights() == CastlingRights::ALL);
	REQUIRE(board.halfmove_clock() == 7);
}

TEST_CASE("Config: a malformed FEN falls back to the default board", "[config]")
{
	TempConfig cfg(R"({
        "game": {
            "FEN": "not-a-fen",
            "players": {
                "white": { "type": 1, "search_limits": {} },
                "black": { "type": 1, "search_limits": {} }
            }
        }
    })");

	Board board;
	Config reader = MakeReader();
	REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));

	// Standard opening position, not whatever the board happened to hold.
	REQUIRE(board.GetPiece(e1) == ePiece::WHITE_KING);
	REQUIRE(board.GetPiece(d8) == ePiece::BLACK_QUEEN);
	REQUIRE(board.ep_square() == NO_SQUARE);
}

TEST_CASE("Config: no FEN key starts from the standard opening position", "[config]")
{
	TempConfig cfg(R"({ "game": { "players": { "white": { "type": 0 }, "black": { "type": 0 } } } })");

	Board board("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
	Config reader = MakeReader();
	REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));

	REQUIRE(board.GetPiece(d1) == ePiece::WHITE_QUEEN);
	REQUIRE(board.GetPiece(d8) == ePiece::BLACK_QUEEN);
}
