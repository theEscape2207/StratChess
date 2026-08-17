#include "StdAfx.h"
#include "SearchLimits.h"

SearchLimits SearchLimits::from_clock(std::chrono::milliseconds remaining, std::chrono::milliseconds increment,
                                      int moves_to_go) noexcept
{
	SearchLimits limits;
	limits.clock = ClockInfo{remaining, increment, moves_to_go};
	return limits;
}

SearchLimits SearchLimits::fixed_time(std::chrono::milliseconds movetime) noexcept
{
	SearchLimits limits;
	limits.movetime = movetime;
	return limits;
}

SearchLimits SearchLimits::fixed_depth(int depth) noexcept
{
	SearchLimits limits;
	limits.depth = depth;
	return limits;
}

SearchLimits SearchLimits::fixed_nodes(int64_t nodes) noexcept
{
	SearchLimits limits;
	limits.nodes = nodes;
	return limits;
}

SearchLimits SearchLimits::infinite_search() noexcept
{
	SearchLimits limits;
	limits.infinite = true;
	return limits;
}

namespace Engine {

	ResolvedLimits resolve_limits(const SearchLimits& limits, std::chrono::milliseconds default_time,
	                              unsigned default_depth) noexcept
	{
		ResolvedLimits r{};
		r.effective_depth =
		    limits.depth ? static_cast<unsigned>(*limits.depth) : (limits.infinite ? 50u : default_depth);
		r.node_limit = limits.nodes;
		if (limits.movetime) { // movetime wins over clock (defensive)
			r.budget = {*limits.movetime, *limits.movetime};
		} else if (limits.clock) {
			r.budget = compute_budget(limits.clock->remaining, limits.clock->increment, limits.clock->moves_to_go);
		} else if (limits.infinite || limits.depth || limits.nodes) {
			// A depth cap, a node limit or UCI 'stop' is the stopping criterion;
			// hours(1) avoids potential overflow from milliseconds::max() in
			// comparisons.
			r.budget = {std::chrono::hours(1), std::chrono::hours(1)};
		} else {
			r.budget = {default_time, default_time};
		}
		return r;
	}

} // namespace Engine
