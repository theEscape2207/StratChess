// SuitePolicyTests.cpp — Catch2 tests for TacticalTestRunner::evaluate_results()
//
// Roadmap acceptance criterion for the tactical suite: pass 80%+ tactical
// tests, 100% mate tests. evaluate_results enforces: overall pass rate >=
// required threshold AND zero failures in any category starting with "mate".

#include <catch_amalgamated.hpp>
#include "Tests/TacticalTestRunner.h"

using Testing::TacticalResult;
using Testing::TacticalTestRunner;

static TacticalResult make_result(const char* id, const char* category, bool passed)
{
    TacticalResult r;
    r.id = id;
    r.category = category;
    r.passed = passed;
    return r;
}

TEST_CASE("all positions passing yields ok", "[suite_policy]")
{
    std::vector<TacticalResult> results = {
        make_result("M1-001", "mate_in_1", true),
        make_result("WAC-001", "mate_in_2", true),
        make_result("HANG-001", "hanging_piece", true),
    };
    const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
    CHECK(v.ok);
    CHECK(v.passed == 3);
    CHECK(v.total == 3);
    CHECK(v.failed_mate_ids.empty());
}

TEST_CASE("one mate failure fails the suite even above the overall threshold", "[suite_policy]")
{
    // 19/20 = 95% overall — above 90% — but the failed position is a mate.
    std::vector<TacticalResult> results;
    for (int i = 0; i < 19; ++i)
        results.push_back(make_result("T", "tactical_win", true));
    results.push_back(make_result("WAC-004", "mate_in_2", false));

    const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
    CHECK_FALSE(v.ok);
    REQUIRE(v.failed_mate_ids.size() == 1);
    CHECK(v.failed_mate_ids[0] == "WAC-004");
}

TEST_CASE("non-mate failures within the threshold still pass", "[suite_policy]")
{
    // 9/10 = 90%, failure is non-mate -> ok at 0.90 threshold.
    std::vector<TacticalResult> results;
    for (int i = 0; i < 9; ++i)
        results.push_back(make_result("T", "tactical_win", true));
    results.push_back(make_result("T-FAIL", "tactical_win", false));

    const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
    CHECK(v.ok);
    CHECK(v.failed_mate_ids.empty());
}

TEST_CASE("non-mate failures below the threshold fail", "[suite_policy]")
{
    // 8/10 = 80% < 90%.
    std::vector<TacticalResult> results;
    for (int i = 0; i < 8; ++i)
        results.push_back(make_result("T", "tactical_win", true));
    results.push_back(make_result("T-FAIL1", "tactical_win", false));
    results.push_back(make_result("T-FAIL2", "tactical_win", false));

    const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
    CHECK_FALSE(v.ok);
}

TEST_CASE("empty result set fails safe", "[suite_policy]")
{
    const auto v = TacticalTestRunner::evaluate_results({}, 0.90);
    CHECK_FALSE(v.ok);
    CHECK(v.total == 0);
}
