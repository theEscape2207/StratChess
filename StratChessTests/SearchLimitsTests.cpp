#include <catch2/catch_test_macros.hpp>
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
// nodes — orthogonal to timing/depth, not an alternative to them
// ============================================================

TEST_CASE("resolve_limits: nodes passes through to node_limit unchanged", "[limits]")
{
	const auto r = Engine::resolve_limits(SearchLimits::fixed_nodes(20'000), 15'000ms, 15);

	REQUIRE(r.node_limit.has_value());
	REQUIRE(*r.node_limit == 20'000);
}

TEST_CASE("resolve_limits: nodes only — 1h budget, not default_time", "[limits]")
{
	const auto r = Engine::resolve_limits(SearchLimits::fixed_nodes(20'000), 15'000ms, 15);

	// Same stopping-criterion branch as depth/infinite: the clock must not be the
	// thing that stops this search, so it gets the same hours(1) escape valve.
	REQUIRE(r.budget.soft == std::chrono::hours(1));
	REQUIRE(r.budget.hard == std::chrono::hours(1));
	REQUIRE(r.budget.soft != 15'000ms);
}

TEST_CASE("resolve_limits: movetime + nodes — movetime budget, node_limit still set", "[limits]")
{
	SearchLimits limits = SearchLimits::fixed_time(3'000ms);
	limits.nodes = 20'000;
	const auto r = Engine::resolve_limits(limits, 15'000ms, 15);

	REQUIRE(r.budget.soft == 3'000ms);
	REQUIRE(r.budget.hard == 3'000ms);
	REQUIRE(r.node_limit.has_value());
	REQUIRE(*r.node_limit == 20'000);
}

TEST_CASE("resolve_limits: clock + nodes — compute_budget budget, node_limit still set", "[limits]")
{
	SearchLimits limits = SearchLimits::from_clock(60'000ms, 1'000ms, 40);
	limits.nodes = 20'000;
	const auto r = Engine::resolve_limits(limits, 15'000ms, 15);

	const auto expected = Engine::compute_budget(60'000ms, 1'000ms, 40);
	REQUIRE(r.budget.soft == expected.soft);
	REQUIRE(r.budget.hard == expected.hard);
	REQUIRE(r.node_limit.has_value());
	REQUIRE(*r.node_limit == 20'000);
}

TEST_CASE("resolve_limits: no nodes set — node_limit is nullopt", "[limits]")
{
	const auto r = Engine::resolve_limits(SearchLimits::fixed_depth(9), 15'000ms, 15);

	REQUIRE_FALSE(r.node_limit.has_value());
}

TEST_CASE("resolve_limits: a budget of 0 arms the check, it does not mean unlimited", "[limits]")
{
	// Set through the field rather than fixed_nodes(), which asserts against 0 in Debug.
	// "Unlimited" is the field being absent; 0 is a budget the first poll is already past.
	SearchLimits limits;
	limits.nodes = 0;
	const auto r = Engine::resolve_limits(limits, 15'000ms, 15);

	REQUIRE(r.node_limit.has_value());
	REQUIRE(*r.node_limit == 0);
	// And it counts as a stopping criterion, so the clock does not fall back to default_time.
	REQUIRE(r.budget.hard == std::chrono::hours(1));
}

TEST_CASE("resolve_limits: infinite + nodes — the node limit survives, depth stays 50", "[limits]")
{
	// UCI 'go infinite nodes N' is legal input. 'infinite' means no clock, not no limits:
	// the node budget is the only thing that can stop such a search short of 'stop'.
	SearchLimits limits = SearchLimits::infinite_search();
	limits.nodes = 20'000;
	const auto r = Engine::resolve_limits(limits, 15'000ms, 15);

	REQUIRE(r.node_limit.has_value());
	REQUIRE(*r.node_limit == 20'000);
	REQUIRE(r.effective_depth == 50u);
	REQUIRE(r.budget.hard == std::chrono::hours(1));
}

// ============================================================
// Factory field checks
// ============================================================

TEST_CASE("SearchLimits factories populate exactly the intended fields", "[limits]")
{
	SECTION("from_clock")
	{
		const auto l = SearchLimits::from_clock(60'000ms, 1'000ms, 40);
		REQUIRE(l.clock.has_value());
		REQUIRE(l.clock->remaining == 60'000ms);
		REQUIRE(l.clock->increment == 1'000ms);
		REQUIRE(l.clock->moves_to_go == 40);
		REQUIRE_FALSE(l.movetime.has_value());
		REQUIRE_FALSE(l.depth.has_value());
		REQUIRE_FALSE(l.infinite);
	}
	SECTION("fixed_time")
	{
		const auto l = SearchLimits::fixed_time(5'000ms);
		REQUIRE(l.movetime == 5'000ms);
		REQUIRE_FALSE(l.clock.has_value());
		REQUIRE_FALSE(l.depth.has_value());
		REQUIRE_FALSE(l.infinite);
	}
	SECTION("fixed_depth")
	{
		const auto l = SearchLimits::fixed_depth(9);
		REQUIRE(l.depth == 9);
		REQUIRE_FALSE(l.clock.has_value());
		REQUIRE_FALSE(l.movetime.has_value());
		REQUIRE_FALSE(l.infinite);
	}
	SECTION("infinite_search")
	{
		const auto l = SearchLimits::infinite_search();
		REQUIRE(l.infinite);
		REQUIRE_FALSE(l.clock.has_value());
		REQUIRE_FALSE(l.movetime.has_value());
		REQUIRE_FALSE(l.depth.has_value());
	}
}
