#pragma once

#include "Move.h"
#include <cstdint>

namespace Engine {

	// Main-worker observations for one depth. Nodes exclude quiescence and are an iteration delta.
	struct IterationSample {
		int depth;
		Move current_move;
		int current_score;
		int64_t nodes_searched;
		int pv_length;
		bool interrupted;
	};

	// Supplied from search tuning; this module owns no configuration defaults.
	struct IterationThresholds {
		int64_t min_nodes_threshold;
		double min_completion_ratio;
		double min_pv_ratio;
	};

	// Local to one main-thread search, never shared with helpers or retained between searches.
	struct IterationState {
		Move best_move{};
		int best_score = 0;
		int depth_completed = 0;
		int64_t nodes_at_completed_depth = 0;
		Move last_iteration_move{};
		int move_run = 0; // consecutive completed depths ending in last_iteration_move
		bool search_was_stable = true;
		bool extra_depth_used = false;
	};

	struct IterationMetrics {
		int depth;
		Move current_move;
		int current_score;
		int64_t nodes_searched;
		int pv_length;
		bool interrupted;
		bool move_changed;
		int score_delta;
		double completion_ratio;
	};

	enum class IterationDisposition : uint8_t { COMPLETED, ACCEPTED_INTERRUPTED, REJECTED };
	enum class RejectionReason : uint8_t { NONE, INCOMPLETE, TOO_FEW_NODES, SHORT_PV, MOVE_CHANGED };

	struct IterationAssessment {
		IterationDisposition decision;
		RejectionReason reason;
		IterationMetrics metrics;
		IterationState next_state;
	};

	enum class IterationStopReason : uint8_t { NONE, SOFT_LIMIT, MATE };

	struct IterationContinuation {
		IterationStopReason stop_reason;
		IterationState next_state;
	};

	// Completed observations bypass quality checks. Interrupted checks are ordered: incomplete,
	// node ratio, PV length, changed move. REJECTED iff reason != NONE; rejection preserves all state.
	// Ratio is main-node delta / prior nodes, or 1.0 without a positive denominator; PV minimum is
	// max(1, int(depth * min_pv_ratio)). Only completed acceptance updates last_iteration_move and
	// move_run: run + 1 on the same move, else 1. TT-replayed depths count in full: the signal holds
	// on warm-TT game searches too.
	IterationAssessment assess_iteration(const IterationSample& sample, const IterationState& previous,
	                                     const IterationThresholds& thresholds) noexcept;

	// Precondition: completed.decision == COMPLETED, called once for that assessment. The driver
	// applies accepted state and emits the observer BEFORE sampling soft_limit_reached and calling here.
	// A changed move grants one soft-limit extension per search. Soft-limit stop takes precedence
	// over mate; a granted extension is consumed even on mate. Depth/PV length never stop early.
	IterationContinuation continue_iteration(const IterationAssessment& completed, bool soft_limit_reached) noexcept;

	// Percent of the soft limit to spend after move_run completed depths on one root move, in
	// [floor_percent, 100] and never increasing in move_run. Short runs, where the next depth most
	// often changes the move, keep the full budget; that rate falls steadily to a plateau, so the
	// cut is linear to the floor rather than a step. A changed move has run 1, so the changed-move
	// extension always sees 100. In a time scramble the result may cut below compute_budget's soft
	// floor; that only saves clock, as the hard limit is untouched.
	int soft_limit_percent(int move_run, int floor_percent) noexcept;

	// soft_limit_percent scaled by a uniform scale_percent, in [floor·scale/100, scale] and so at
	// least 1 for the tuning minimums of 10 and 10. 100 returns soft_limit_percent exactly.
	int scaled_soft_percent(int move_run, int floor_percent, int scale_percent) noexcept;

} // namespace Engine
