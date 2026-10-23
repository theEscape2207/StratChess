#include "StdAfx.h"
#include "IterationPolicy.h"
#include "defines.h"

namespace Engine {

	IterationAssessment assess_iteration(const IterationSample& sample, const IterationState& previous,
	                                     const IterationThresholds& thresholds) noexcept
	{
		const IterationMetrics metrics{
		    sample.depth, sample.current_move, sample.current_score, sample.nodes_searched, sample.pv_length,
		    sample.interrupted, sample.current_move != previous.last_iteration_move,
		    sample.current_score - previous.best_score,
		    // Main-node counts never realistically approach 2^53 (double precision loss).
		    previous.nodes_at_completed_depth > 0
		        ? static_cast<double>(sample.nodes_searched) / static_cast<double>(previous.nodes_at_completed_depth)
		        : 1.0};

		RejectionReason reason = RejectionReason::NONE;
		if (sample.interrupted) {
			if (sample.current_move.is_null() || sample.nodes_searched < thresholds.min_nodes_threshold)
				reason = RejectionReason::INCOMPLETE;
			else if (previous.depth_completed > 0 && previous.nodes_at_completed_depth > 0 &&
			         metrics.completion_ratio < thresholds.min_completion_ratio)
				reason = RejectionReason::TOO_FEW_NODES;
			else if (sample.pv_length < std::max(1, static_cast<int>(sample.depth * thresholds.min_pv_ratio)) &&
			         previous.depth_completed > 0)
				reason = RejectionReason::SHORT_PV;
			else if (metrics.move_changed && previous.depth_completed > 0)
				reason = RejectionReason::MOVE_CHANGED;
		}

		if (reason != RejectionReason::NONE)
			return {IterationDisposition::REJECTED, reason, metrics, previous};

		auto next = previous;
		next.best_move = sample.current_move;
		next.best_score = sample.current_score;
		next.depth_completed = sample.depth;
		next.nodes_at_completed_depth = sample.nodes_searched;
		next.search_was_stable = !metrics.move_changed;
		if (!sample.interrupted) {
			next.move_run = metrics.move_changed ? 1 : previous.move_run + 1;
			next.last_iteration_move = sample.current_move;
		}
		return {sample.interrupted ? IterationDisposition::ACCEPTED_INTERRUPTED : IterationDisposition::COMPLETED,
		        RejectionReason::NONE, metrics, next};
	}

	IterationContinuation continue_iteration(const IterationAssessment& completed, bool soft_limit_reached) noexcept
	{
		assert(completed.decision == IterationDisposition::COMPLETED);
		auto next = completed.next_state;
		if (soft_limit_reached) {
			if (!completed.metrics.move_changed || next.extra_depth_used)
				return {IterationStopReason::SOFT_LIMIT, next};
			next.extra_depth_used = true;
		}
		if (std::abs(next.best_score) >= GameValues::Mate_Threshold)
			return {IterationStopReason::MATE, next};
		return {IterationStopReason::NONE, next};
	}

	int soft_limit_percent(int move_run, int floor_percent) noexcept
	{
		constexpr int LAST_FULL_BUDGET_RUN = 2;
		constexpr int FLOOR_RUN = 6;
		assert(floor_percent >= 0 && floor_percent <= 100);
		if (move_run <= LAST_FULL_BUDGET_RUN)
			return 100;
		return 100 - (100 - floor_percent) * (std::min(move_run, FLOOR_RUN) - LAST_FULL_BUDGET_RUN) /
		                 (FLOOR_RUN - LAST_FULL_BUDGET_RUN);
	}

} // namespace Engine
