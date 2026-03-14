// UCITests.cpp — Catch2 [uci] tests for UciHandler::parse_go()
//
// parse_go() is a pure static function: no board state, no AI, no spdlog.
// All tests are deterministic and require zero setup.

#include <catch_amalgamated.hpp>
#include "UCIHandler.h"

using P = UciHandler::GoParams;

// ---------------------------------------------------------------------------
// parse_go — standard clock params
// ---------------------------------------------------------------------------

TEST_CASE("parse_go: wtime/btime/winc/binc", "[uci]")
{
    auto p = UciHandler::parse_go("go wtime 120000 btime 90000 winc 2000 binc 1000");
    REQUIRE(p.wtime     == 120000);
    REQUIRE(p.btime     == 90000);
    REQUIRE(p.winc      == 2000);
    REQUIRE(p.binc      == 1000);
    REQUIRE(p.movestogo == 0);
    REQUIRE(p.depth     == 0);
    REQUIRE(p.movetime  == 0);
    REQUIRE(p.infinite  == false);
}

TEST_CASE("parse_go: movestogo", "[uci]")
{
    auto p = UciHandler::parse_go("go wtime 60000 btime 60000 movestogo 20");
    REQUIRE(p.wtime     == 60000);
    REQUIRE(p.movestogo == 20);
}

TEST_CASE("parse_go: movetime", "[uci]")
{
    auto p = UciHandler::parse_go("go movetime 5000");
    REQUIRE(p.movetime == 5000);
    REQUIRE(p.wtime    == 0);
    REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: depth", "[uci]")
{
    auto p = UciHandler::parse_go("go depth 8");
    REQUIRE(p.depth    == 8);
    REQUIRE(p.movetime == 0);
    REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: infinite", "[uci]")
{
    auto p = UciHandler::parse_go("go infinite");
    REQUIRE(p.infinite == true);
    REQUIRE(p.wtime    == 0);
    REQUIRE(p.movetime == 0);
    REQUIRE(p.depth    == 0);
}

TEST_CASE("parse_go: depth + infinite", "[uci]")
{
    // Analysis mode: fixed depth, no time pressure
    auto p = UciHandler::parse_go("go infinite depth 10");
    REQUIRE(p.infinite == true);
    REQUIRE(p.depth    == 10);
}

// ---------------------------------------------------------------------------
// parse_go — robustness
// ---------------------------------------------------------------------------

TEST_CASE("parse_go: unknown tokens are silently skipped", "[uci]")
{
    // GUI may send tokens the engine doesn't know; must not crash or misparse.
    auto p = UciHandler::parse_go("go wtime 5000 ponder searchmoves e2e4 btime 4000");
    REQUIRE(p.wtime == 5000);
    REQUIRE(p.btime == 4000);
    // Unknown tokens ('ponder', 'searchmoves', 'e2e4') produce no field changes
    REQUIRE(p.depth    == 0);
    REQUIRE(p.movetime == 0);
    REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: bare 'go' with no params — all fields default", "[uci]")
{
    auto p = UciHandler::parse_go("go");
    REQUIRE(p.wtime     == 0);
    REQUIRE(p.btime     == 0);
    REQUIRE(p.winc      == 0);
    REQUIRE(p.binc      == 0);
    REQUIRE(p.movestogo == 0);
    REQUIRE(p.depth     == 0);
    REQUIRE(p.movetime  == 0);
    REQUIRE(p.infinite  == false);
}

TEST_CASE("parse_go: params in non-standard order", "[uci]")
{
    // UCI spec does not guarantee ordering — engine must handle any order.
    auto p = UciHandler::parse_go("go binc 500 btime 30000 movestogo 10 winc 1000 wtime 45000");
    REQUIRE(p.wtime     == 45000);
    REQUIRE(p.btime     == 30000);
    REQUIRE(p.winc      == 1000);
    REQUIRE(p.binc      == 500);
    REQUIRE(p.movestogo == 10);
}
