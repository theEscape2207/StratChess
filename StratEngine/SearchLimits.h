#pragma once
#include "Utils/TimeUtils.h"
#include <chrono>
#include <cstdint>
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
	std::optional<int64_t> nodes;                      // UCI: nodes (see below)
	bool infinite = false;                             // UCI: infinite

	[[nodiscard]] static SearchLimits from_clock(std::chrono::milliseconds remaining,
	                                             std::chrono::milliseconds increment, int moves_to_go) noexcept;
	[[nodiscard]] static SearchLimits fixed_time(std::chrono::milliseconds movetime) noexcept;
	[[nodiscard]] static SearchLimits fixed_depth(int depth) noexcept;
	[[nodiscard]] static SearchLimits fixed_nodes(int64_t nodes) noexcept;
	[[nodiscard]] static SearchLimits infinite_search() noexcept;
};

namespace Engine {

	/// Everything the search needs after limits are resolved against the
	/// engine's configured defaults.
	///
	/// A node limit is an additional stopping criterion rather than an
	/// alternative to the budget: it is observed at the search's existing
	/// 1024-node poll, so the effective stop is the first multiple of 1024 at
	/// or past the limit, and only the main search thread polls — under Lazy
	/// SMP the limit therefore counts thread 0's nodes, exactly as the clock
	/// check already does. At Threads=1 node counting is deterministic, which
	/// makes a node limit the only reproducible way to abort mid-tree.
	struct ResolvedLimits {
		TimeBudget budget; // soft/hard, see Utils/TimeUtils.h
		unsigned effective_depth;
		std::optional<int64_t> node_limit; // nullopt = unlimited
	};

	/// Pure resolution of SearchLimits against configured defaults — no clock
	/// dependency, unit-testable without a player object (same pattern as
	/// compute_budget). Precedence: movetime > clock for timing; depth always
	/// caps depth; empty limits fall back to (default_time, default_depth).
	/// A node limit is orthogonal to all of them — it passes through untouched
	/// and, like a depth cap, on its own leaves the clock effectively unbounded
	/// rather than falling back to default_time.
	[[nodiscard]] ResolvedLimits resolve_limits(const SearchLimits& limits, std::chrono::milliseconds default_time,
	                                            unsigned default_depth) noexcept;

} // namespace Engine
