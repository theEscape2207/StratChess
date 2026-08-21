#pragma once
#include "SearchLimits.h"
#include "Utils/TimeManager.h"
#include <chrono>
#include <cstdint>

class SearchControl final {
  public:
	SearchControl(unsigned default_depth, std::chrono::milliseconds default_time) noexcept;
	void SetDefaults(unsigned depth, std::chrono::milliseconds time) noexcept;
	void ApplyLimits(const SearchLimits& limits);
	void Stop() noexcept;
	bool StopRequested() const noexcept;
	bool ShouldStopIteration() const noexcept;
	bool NodeLimitReached(int64_t searched_nodes) noexcept;
	bool IsAborted() const noexcept;
	unsigned EffectiveDepth() const noexcept;
	std::chrono::milliseconds Elapsed() const noexcept;

  private:
	chess::TimeManager time_manager_;
	unsigned default_depth_;
	std::chrono::milliseconds default_time_;
	unsigned effective_depth_{0};
	int64_t node_limit_{0};
};
