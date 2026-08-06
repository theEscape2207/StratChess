#include <catch_amalgamated.hpp>
#include "Config.h"
#include "Board.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Writes a settings document to a uniquely named temp file and removes it
// afterwards, so the cases cannot collide when the suite runs concurrently.
class TempConfig {
public:
    explicit TempConfig(std::string_view contents)
        : path_(std::filesystem::temp_directory_path() /
                ("strat_cfg_" + std::to_string(++counter_) + "_" +
                 std::to_string(
                     static_cast<unsigned long long>(
                         std::chrono::steady_clock::now().time_since_epoch().count())) +
                 ".json"))
    {
        std::ofstream out(path_);
        out << contents;
    }
    ~TempConfig() { std::error_code ec; std::filesystem::remove(path_, ec); }

    TempConfig(const TempConfig&) = delete;
    TempConfig& operator=(const TempConfig&) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// Config dereferences its Game* only on the FEN path (ReadFEN -> SetCustomGame).
// None of the documents below declare a FEN setup, so nullptr is safe here and
// keeps these cases free of a whole Game.
Config MakeReader() { return Config(nullptr); }

} // namespace

TEST_CASE("Config: a well-formed document parses", "[config]")
{
    // Positive control. Without it the negative cases below could all pass by
    // throwing for some unrelated reason.
    TempConfig cfg(R"({
        "game": { "players": {
            "white": { "type": 6, "search_limits": { "depth": 7 } },
            "black": { "type": 6, "search_limits": { "depth": 3 } }
        } }
    })");

    Board board;
    Config reader = MakeReader();
    REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));

    REQUIRE(reader.GetPlayerFromConfig(true).search_limits.depth == 7);
    REQUIRE(reader.GetPlayerFromConfig(false).search_limits.depth == 3);
}

TEST_CASE("Config: truncated JSON is reported, not silently accepted", "[config]")
{
    TempConfig cfg(R"({ "game": { "players": )");

    Board board;
    Config reader = MakeReader();
    REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board),
                      nlohmann::json::parse_error);
}

TEST_CASE("Config: a missing key is an error, not undefined behaviour", "[config]")
{
    // These read through a CONST json&, where nlohmann's operator[] on an absent
    // key is UB rather than an error: Release carried on and threw something
    // unrelated later, while a Debug build asserted inside the library. .at()
    // makes every one of them a diagnosable out_of_range naming the key.
    Board board;

    SECTION("no \"game\"") {
        TempConfig cfg(R"({ "nothing": 1 })");
        Config reader = MakeReader();
        REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board),
                          nlohmann::json::out_of_range);
    }
    SECTION("no \"players\"") {
        TempConfig cfg(R"({ "game": { "setup": "default" } })");
        Config reader = MakeReader();
        REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board),
                          nlohmann::json::out_of_range);
    }
    SECTION("no \"black\"") {
        TempConfig cfg(R"({ "game": { "players": { "white": { "type": 6 } } } })");
        Config reader = MakeReader();
        REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board),
                          nlohmann::json::out_of_range);
    }
}

TEST_CASE("Config: a key of the wrong type is reported", "[config]")
{
    // .value() tolerates a MISSING key but still throws on a present key of the
    // wrong type, which is the case issue #178 predicted and this pins.
    TempConfig cfg(R"({
        "game": { "players": {
            "white": { "type": 6, "search_limits": { "depth": "five" } },
            "black": { "type": 6 }
        } }
    })");

    Board board;
    Config reader = MakeReader();
    REQUIRE_THROWS_AS(reader.ReadConfigFile(cfg.path(), board),
                      nlohmann::json::type_error);
}

TEST_CASE("Config: comments are accepted", "[config]")
{
    // game_settings.json ships heavily commented, so ignore_comments must stay
    // on -- a regression here would break the shipped file, not just a test.
    TempConfig cfg(R"({
        /* block comment */
        "game": { "players": {
            "white": { "type": 6, "search_limits": { "depth": 5 } },
            "black": { "type": 6 }
        } }
    })");

    Board board;
    Config reader = MakeReader();
    REQUIRE_NOTHROW(reader.ReadConfigFile(cfg.path(), board));
    REQUIRE(reader.GetPlayerFromConfig(true).search_limits.depth == 5);
}

TEST_CASE("Config: a missing file falls back to defaults without throwing", "[config]")
{
    const auto missing = (std::filesystem::temp_directory_path() /
                          "strat_cfg_definitely_absent_178.json").string();
    REQUIRE_FALSE(std::filesystem::exists(missing));

    Board board;
    Config reader = MakeReader();
    // Reported on stderr, then defaults stand -- deliberately not an exception,
    // since running with defaults is a usable outcome and the message says so.
    REQUIRE_NOTHROW(reader.ReadConfigFile(missing, board));
}
