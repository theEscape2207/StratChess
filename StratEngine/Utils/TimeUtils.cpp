#include "../StdAfx.h"
#include "TimeUtils.h"

namespace Engine {

	TimeBudget compute_budget(std::chrono::milliseconds remaining, std::chrono::milliseconds increment,
	                          int moves_to_go) noexcept
	{
		using ms = std::chrono::milliseconds;

		constexpr ms overhead{50};
		constexpr ms floor_time{100};

		// Time actually available to spend. Never more than the clock holds, so a
		// drained clock yields a zero budget rather than an unpayable one.
		const ms usable = std::max(remaining - overhead, ms{0});

		// Ceiling for both limits: one move never commits more than half of what is
		// left, which leaves the next move something to spend.
		const ms cap = usable / 2;

		// Minimum worth searching, but it yields to the clock: below roughly 250 ms
		// remaining the emergency floor is what the clock can pay, not a constant.
		const ms floor = std::min(floor_time, cap);

		// Horizon: use provided moves_to_go, otherwise assume 30 moves left
		const int horizon = (moves_to_go > 0) ? moves_to_go : 30;

		// Base allocation: spread time evenly + capture most of the increment
		const ms base{
		    static_cast<ms::rep>(usable.count() / horizon +
		                         increment.count() * 8 / 10) // integer 80% — avoids C4244 double->long long
		};

		const ms soft = std::clamp(base, floor, cap);

		// Hard limit: 1.5× soft so one move can never dominate the clock.
		// A 3× factor caused the engine to spend the full hard budget on every opening
		// move (because early depths complete well inside soft, depth N+1 then runs
		// until hard fires), leading to time forfeits in sudden-death time controls.
		const ms hard_candidate{static_cast<ms::rep>(soft.count() * 3 / 2)};
		const ms hard = std::clamp(hard_candidate, soft, cap);

		return TimeBudget{soft, hard};
	}

} // namespace Engine
