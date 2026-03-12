#pragma once
#include <chrono>

namespace Engine {

/// Computed time budget for a single move search.
/// @invariant hard >= soft >= 100 ms always holds.
struct TimeBudget {
    std::chrono::milliseconds soft;   ///< Target stop time (finish current depth, then stop)
    std::chrono::milliseconds hard;   ///< Emergency cutoff (abort mid-search)
};

/// Compute a soft and hard time budget from clock information.
///
/// @param remaining  Time left on the clock for this side (milliseconds).
/// @param increment  Per-move increment added after the move (milliseconds).
/// @param moves_to_go  Moves remaining in the current time control, or 0 if unknown.
/// @returns TimeBudget with hard >= soft >= 100 ms.
[[nodiscard]] TimeBudget compute_budget(
    std::chrono::milliseconds remaining,
    std::chrono::milliseconds increment,
    int moves_to_go) noexcept;

} // namespace Engine
