#include <catch_amalgamated.hpp>
#include "SearchControl.h"
#include "SearchLimits.h"
#include <chrono>

using namespace std::chrono_literals;

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
