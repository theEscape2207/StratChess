#pragma once
#include "Utils/TimeUtils.h"
#include <chrono>
#include <optional>

/// Per-move clock state as sent by UCI 'go' (wtime/btime/winc/binc/movestogo).
struct ClockInfo {
	std::chrono::milliseconds remaining{0};
	std::chrono::milliseconds increment{0};
	int moves_to_go = 0; // 0 = unknown
};

/// All per-move search constraints a caller can express in one GetMove() call.
/// Default-constructed SearchLimits{} means "use the engine's configured
/// defaults" (time_limit_, max_depth_).
struct SearchLimits {
	std::optional<ClockInfo> clock;                    // UCI: wtime/btime/winc/binc/movestogo
	std::optional<std::chrono::milliseconds> movetime; // UCI: movetime
	std::optional<int> depth;                          // UCI: depth (per-call cap)
	bool infinite = false;                             // UCI: infinite

	[[nodiscard]] static SearchLimits from_clock(std::chrono::milliseconds remaining,
	                                             std::chrono::milliseconds increment,
	                                             int moves_to_go) noexcept;
	[[nodiscard]] static SearchLimits fixed_time(std::chrono::milliseconds movetime) noexcept;
	[[nodiscard]] static SearchLimits fixed_depth(int depth) noexcept;
	[[nodiscard]] static SearchLimits infinite_search() noexcept;
};

namespace Engine {

/// Everything the search needs after limits are resolved against the
/// engine's configured defaults.
struct ResolvedLimits {
	TimeBudget budget; // soft/hard, see Utils/TimeUtils.h
	unsigned effective_depth;
};

/// Pure resolution of SearchLimits against configured defaults — no clock
/// dependency, unit-testable without a player object (same pattern as
/// compute_budget). Precedence: movetime > clock for timing; depth always
/// caps depth; empty limits fall back to (default_time, default_depth).
[[nodiscard]] ResolvedLimits resolve_limits(const SearchLimits& limits,
                                            std::chrono::milliseconds default_time,
                                            unsigned default_depth) noexcept;

} // namespace Engine
