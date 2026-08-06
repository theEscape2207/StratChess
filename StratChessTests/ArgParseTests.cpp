#include <catch_amalgamated.hpp>
#include "Utils/ArgParse.h"

#include <limits>
#include <string>

using Engine::parse_int;

TEST_CASE("parse_int: accepts whole integers", "[argparse]")
{
    REQUIRE(parse_int("0") == 0);
    REQUIRE(parse_int("7") == 7);
    REQUIRE(parse_int("-5") == -5);
    REQUIRE(parse_int("+3") == 3);
    REQUIRE(parse_int("0010") == 10);
    REQUIRE(parse_int(std::to_string(std::numeric_limits<int>::max()))
            == std::numeric_limits<int>::max());
    REQUIRE(parse_int(std::to_string(std::numeric_limits<int>::min()))
            == std::numeric_limits<int>::min());
}

TEST_CASE("parse_int: rejects anything that is not exactly an integer", "[argparse]")
{
    for (std::string_view bad : {
             "",           // no argument supplied
             " ",
             "abc",        // issue #178's `perft run abc`
             "1 2",
             "1.5",
             "--3",
             "+",
             "-",
             " 12",        // leading space is not silently skipped
             "12 ",
         })
    {
        CAPTURE(bad);
        REQUIRE_FALSE(parse_int(bad).has_value());
    }
}

TEST_CASE("parse_int: rejects trailing text that std::stoi would accept", "[argparse]")
{
    // std::stoi("12abc") returns 12 and reports nothing. Every guarded call site
    // in the engine inherited that, so a typo'd argument became a valid-looking
    // number. These must be refused, not truncated.
    REQUIRE_FALSE(parse_int("12abc").has_value());
    REQUIRE_FALSE(parse_int("4x").has_value());
    REQUIRE_FALSE(parse_int("10,000").has_value());
}

TEST_CASE("parse_int: rejects values outside int rather than wrapping", "[argparse]")
{
    REQUIRE_FALSE(parse_int("99999999999999").has_value());
    REQUIRE_FALSE(parse_int("-99999999999999").has_value());

    // One past each boundary, which is where a wrap would hide.
    REQUIRE_FALSE(parse_int(std::to_string(
        static_cast<long long>(std::numeric_limits<int>::max()) + 1)).has_value());
    REQUIRE_FALSE(parse_int(std::to_string(
        static_cast<long long>(std::numeric_limits<int>::min()) - 1)).has_value());
}
