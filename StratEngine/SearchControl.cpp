#include "StdAfx.h"
#include "SearchControl.h"

SearchControl::SearchControl(unsigned default_depth, std::chrono::milliseconds default_time) noexcept
    : default_depth_(default_depth), default_time_(default_time), effective_depth_(default_depth)
{}

void SearchControl::SetDefaults(unsigned depth, std::chrono::milliseconds time) noexcept
{
	default_depth_ = depth;
	default_time_ = time;
}

void SearchControl::ApplyLimits(const SearchLimits& limits)
{
	const auto resolved = Engine::resolve_limits(limits, default_time_, default_depth_);
	time_manager_.start(resolved.budget.soft, resolved.budget.hard);
	effective_depth_ = resolved.effective_depth;
	node_limit_ = resolved.node_limit;
}

void SearchControl::Stop() noexcept { time_manager_.stop(); }

bool SearchControl::StopRequested() const noexcept { return time_manager_.should_stop_search(); }

bool SearchControl::ShouldStopIteration() const noexcept { return time_manager_.should_stop_iteration(); }

bool SearchControl::NodeLimitReached(int64_t searched_nodes) noexcept
{
	if (!node_limit_ || searched_nodes < *node_limit_)
		return false;
	Stop();
	return true;
}

bool SearchControl::IsAborted() const noexcept { return time_manager_.is_aborted(); }

unsigned SearchControl::EffectiveDepth() const noexcept { return effective_depth_; }

std::chrono::milliseconds SearchControl::Elapsed() const noexcept { return time_manager_.elapsed(); }
