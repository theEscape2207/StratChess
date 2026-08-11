#pragma once
#include <chrono>

namespace Engine {

/// Computed time budget for a single move search.
/// @invariant usable/2 >= hard >= soft >= 0, where usable = max(remaining - overhead, 0).
///            A drained clock yields a zero budget, never a floored one.
struct TimeBudget {
	std::chrono::milliseconds soft; ///< Target stop time (finish current depth, then stop)
	std::chrono::milliseconds hard; ///< Emergency cutoff (abort mid-search)
};

/// Compute a soft and hard time budget from clock information.
///
/// @param remaining  Time left on the clock for this side (milliseconds).
/// @param increment  Per-move increment added after the move (milliseconds).
/// @param moves_to_go  Moves remaining in the current time control, or 0 if unknown.
/// @returns TimeBudget bounded by the clock: hard never exceeds half of
///          max(remaining - overhead, 0), so spending it can never forfeit.
///          At or below the overhead the budget is zero and the caller is
///          expected to move immediately.
[[nodiscard]] TimeBudget compute_budget(std::chrono::milliseconds remaining, std::chrono::milliseconds increment,
                                        int moves_to_go) noexcept;

} // namespace Engine
