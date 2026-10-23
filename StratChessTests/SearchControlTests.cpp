#include <catch2/catch_test_macros.hpp>
#include "SearchControl.h"
#include "SearchLimits.h"
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("SearchControl resolves empty limits from its configured defaults", "[search_control]")
{
	SearchControl control(7, 2500ms);

	control.ApplyLimits(SearchLimits{});

	CHECK(control.EffectiveDepth() == 7);
	CHECK_FALSE(control.IsAborted());
}

TEST_CASE("SearchControl elapsed time resets for each applied-limits session", "[search_control]")
{
	SearchControl control(6, 15000ms);
	control.ApplyLimits(SearchLimits::fixed_depth(4));

	const auto first_deadline = std::chrono::steady_clock::now() + 1s;
	while (control.Elapsed() < 5ms && std::chrono::steady_clock::now() < first_deadline)
		std::this_thread::yield();
	const auto first_elapsed = control.Elapsed();
	REQUIRE(first_elapsed >= 5ms);

	control.Stop();
	REQUIRE(control.IsAborted());
	control.ApplyLimits(SearchLimits::fixed_depth(3));

	CHECK_FALSE(control.IsAborted());
	CHECK(control.EffectiveDepth() == 3);
	CHECK(control.Elapsed() < first_elapsed);
}

TEST_CASE("SearchControl exposes the clock soft stop before latching the hard stop", "[search_control]")
{
	SearchControl control(6, 15000ms);
	// usable=60s, horizon=60 => soft=1s and hard=1.5s. The 500 ms
	// separation keeps the assertion tolerant of ordinary CI scheduling jitter.
	control.ApplyLimits(SearchLimits::from_clock(60050ms, 0ms, 60));

	const auto deadline = std::chrono::steady_clock::now() + 3s;
	while (!control.ShouldStopIteration() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	REQUIRE(control.ShouldStopIteration());
	CHECK_FALSE(control.IsAborted());

	while (!control.StopRequested() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	REQUIRE(control.StopRequested());
	CHECK(control.IsAborted());
}

TEST_CASE("SearchControl scales the soft limit for clock budgets only", "[search_control]")
{
	SearchControl control(6, 15000ms);
	// Clock: soft = 1 s (as above), so 10% fires at 100 ms, 900 ms before the full limit.
	control.ApplyLimits(SearchLimits::from_clock(60050ms, 0ms, 60));
	const auto deadline = std::chrono::steady_clock::now() + 3s;
	while (!control.ShouldStopIteration(10) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	REQUIRE(control.ShouldStopIteration(10));
	CHECK_FALSE(control.ShouldStopIteration());

	// Movetime: soft == hard == 1 s, and 10% must not shorten it.
	control.ApplyLimits(SearchLimits::fixed_time(1000ms));
	while (control.Elapsed() < 150ms && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	REQUIRE(control.Elapsed() >= 150ms);
	CHECK_FALSE(control.ShouldStopIteration(10));
}

TEST_CASE("SearchControl resets a latched stop when new limits are applied", "[search_control]")
{
	SearchControl control(6, 15000ms);
	control.ApplyLimits(SearchLimits::fixed_depth(4));
	control.Stop();
	REQUIRE(control.IsAborted());

	control.ApplyLimits(SearchLimits::fixed_depth(3));
	CHECK_FALSE(control.IsAborted());
	CHECK(control.EffectiveDepth() == 3);
}

TEST_CASE("SearchControl latches the node budget at the first reached total", "[search_control]")
{
	SearchControl control(6, 15000ms);
	control.ApplyLimits(SearchLimits::fixed_nodes(2048));
	CHECK_FALSE(control.NodeLimitReached(2047));
	CHECK(control.NodeLimitReached(2048));
	CHECK(control.IsAborted());
}

TEST_CASE("SearchControl latches an explicit zero-node budget at the first poll", "[search_control]")
{
	SearchControl control(6, 15000ms);
	SearchLimits limits;
	limits.nodes = 0;
	control.ApplyLimits(limits);

	CHECK(control.NodeLimitReached(0));
	CHECK(control.IsAborted());
}
