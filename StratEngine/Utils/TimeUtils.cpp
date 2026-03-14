#include "../StdAfx.h"
#include "TimeUtils.h"

namespace Engine {

TimeBudget compute_budget(
    std::chrono::milliseconds remaining,
    std::chrono::milliseconds increment,
    int moves_to_go) noexcept
{
    using ms = std::chrono::milliseconds;

    constexpr ms overhead{ 50 };
    constexpr ms floor_time{ 100 };

    // Clamp usable time to avoid zero or negative division
    const ms usable = std::max(remaining - overhead, floor_time);

    // Horizon: use provided moves_to_go, otherwise assume 30 moves left
    const int horizon = (moves_to_go > 0) ? moves_to_go : 30;

    // Base allocation: spread time evenly + capture most of the increment
    const ms base{
        static_cast<ms::rep>(usable.count() / horizon
            + increment.count() * 8 / 10)  // integer 80% — avoids C4244 double->long long
    };

    const ms soft = std::max(base, floor_time);

    // Hard limit: cap at 1.5× soft so one move can never dominate the clock.
    // A 3× factor caused the engine to spend the full hard budget on every opening
    // move (because early depths complete well inside soft, depth N+1 then runs
    // until hard fires), leading to time forfeits in sudden-death time controls.
    const ms hard_candidate{ static_cast<ms::rep>(soft.count() * 3 / 2) };
    const ms hard_cap{ static_cast<ms::rep>(usable.count() / 2) };
    const ms hard = std::max(std::min(hard_candidate, hard_cap), soft);

    return TimeBudget{ soft, hard };
}

} // namespace Engine
