#include <catch_amalgamated.hpp>
#include "SearchLimits.h"
#include "Utils/TimeUtils.h"

using namespace std::chrono_literals;

// ============================================================
// resolve_limits() — one test per resolution-table row.
// Budgets must be *identical* to what the pre-refactor setter
// path (SetClockInfo / SetTimeLimit / SetMaxDepth) produced.
// ============================================================

TEST_CASE("resolve_limits: clock set — compute_budget budgets, configured depth", "[limits]")
{
    const auto limits = SearchLimits::from_clock(60'000ms, 1'000ms, 40);
    const auto r = Engine::resolve_limits(limits, 15'000ms, 15);

    const auto expected = Engine::compute_budget(60'000ms, 1'000ms, 40);
    REQUIRE(r.budget.soft == expected.soft);
    REQUIRE(r.budget.hard == expected.hard);
    REQUIRE(r.effective_depth == 15u);
}

TEST_CASE("resolve_limits: movetime set — soft == hard == movetime", "[limits]")
{
    const auto r = Engine::resolve_limits(SearchLimits::fixed_time(5'000ms), 15'000ms, 15);

    REQUIRE(r.budget.soft == 5'000ms);
    REQUIRE(r.budget.hard == 5'000ms);
    REQUIRE(r.effective_depth == 15u);
}

TEST_CASE("resolve_limits: depth only — 1h budget, that depth", "[limits]")
{
    const auto r = Engine::resolve_limits(SearchLimits::fixed_depth(9), 15'000ms, 15);

    REQUIRE(r.budget.soft == std::chrono::hours(1));
    REQUIRE(r.budget.hard == std::chrono::hours(1));
    REQUIRE(r.effective_depth == 9u);
}

TEST_CASE("resolve_limits: infinite — 1h budget, depth 50", "[limits]")
{
    const auto r = Engine::resolve_limits(SearchLimits::infinite_search(), 15'000ms, 15);

    REQUIRE(r.budget.soft == std::chrono::hours(1));
    REQUIRE(r.budget.hard == std::chrono::hours(1));
    REQUIRE(r.effective_depth == 50u);
}

TEST_CASE("resolve_limits: empty limits — configured defaults", "[limits]")
{
    const auto r = Engine::resolve_limits(SearchLimits{}, 15'000ms, 15);

    REQUIRE(r.budget.soft == 15'000ms);
    REQUIRE(r.budget.hard == 15'000ms);
    REQUIRE(r.effective_depth == 15u);
}

// ============================================================
// Precedence when several fields are set (defensive — UCI only
// combines depth with time fields)
// ============================================================

TEST_CASE("resolve_limits: movetime + depth — movetime budget, that depth", "[limits]")
{
    SearchLimits limits = SearchLimits::fixed_time(3'000ms);
    limits.depth = 7;
    const auto r = Engine::resolve_limits(limits, 15'000ms, 15);

    REQUIRE(r.budget.soft == 3'000ms);
    REQUIRE(r.budget.hard == 3'000ms);
    REQUIRE(r.effective_depth == 7u);
}

TEST_CASE("resolve_limits: clock + depth — clock budget, that depth", "[limits]")
{
    SearchLimits limits = SearchLimits::from_clock(120'000ms, 2'000ms, 0);
    limits.depth = 12;
    const auto r = Engine::resolve_limits(limits, 15'000ms, 15);

    const auto expected = Engine::compute_budget(120'000ms, 2'000ms, 0);
    REQUIRE(r.budget.soft == expected.soft);
    REQUIRE(r.budget.hard == expected.hard);
    REQUIRE(r.effective_depth == 12u);
}

TEST_CASE("resolve_limits: movetime wins over clock for timing", "[limits]")
{
    SearchLimits limits = SearchLimits::from_clock(60'000ms, 1'000ms, 40);
    limits.movetime = 2'500ms;
    const auto r = Engine::resolve_limits(limits, 15'000ms, 15);

    REQUIRE(r.budget.soft == 2'500ms);
    REQUIRE(r.budget.hard == 2'500ms);
}

// ============================================================
// Factory field checks
// ============================================================

TEST_CASE("SearchLimits factories populate exactly the intended fields", "[limits]")
{
    SECTION("from_clock") {
        const auto l = SearchLimits::from_clock(60'000ms, 1'000ms, 40);
        REQUIRE(l.clock.has_value());
        REQUIRE(l.clock->remaining == 60'000ms);
        REQUIRE(l.clock->increment == 1'000ms);
        REQUIRE(l.clock->moves_to_go == 40);
        REQUIRE_FALSE(l.movetime.has_value());
        REQUIRE_FALSE(l.depth.has_value());
        REQUIRE_FALSE(l.infinite);
    }
    SECTION("fixed_time") {
        const auto l = SearchLimits::fixed_time(5'000ms);
        REQUIRE(l.movetime == 5'000ms);
        REQUIRE_FALSE(l.clock.has_value());
        REQUIRE_FALSE(l.depth.has_value());
        REQUIRE_FALSE(l.infinite);
    }
    SECTION("fixed_depth") {
        const auto l = SearchLimits::fixed_depth(9);
        REQUIRE(l.depth == 9);
        REQUIRE_FALSE(l.clock.has_value());
        REQUIRE_FALSE(l.movetime.has_value());
        REQUIRE_FALSE(l.infinite);
    }
    SECTION("infinite_search") {
        const auto l = SearchLimits::infinite_search();
        REQUIRE(l.infinite);
        REQUIRE_FALSE(l.clock.has_value());
        REQUIRE_FALSE(l.movetime.has_value());
        REQUIRE_FALSE(l.depth.has_value());
    }
}
