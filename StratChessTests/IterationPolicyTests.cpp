// The main-thread iteration policy is exercised with raw observations and retained values.
#include "IterationPolicy.h"
#include "defines.h"
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>

namespace {
	using namespace Engine;

	const Move kMove{0x0341};
	const Move kOtherMove{0x0342};
	const Move kFlaggedMove{0x1341}; // Same squares as kMove, different flags.
	constexpr IterationThresholds kThresholds{100, 0.25, 0.5};

	IterationSample sample(int depth, Move move, int score, int64_t nodes, int pv, bool interrupted = false)
	{
		return {depth, move, score, nodes, pv, interrupted};
	}

	IterationAssessment assess(const IterationSample& observation, const IterationState& previous = {})
	{
		const auto result = assess_iteration(observation, previous, kThresholds);
		CHECK((result.decision == IterationDisposition::REJECTED) == (result.reason != RejectionReason::NONE));
		return result;
	}

	IterationState completed_state(int depth = 3, Move move = kMove, int score = 300, int64_t nodes = 400)
	{
		const auto completed = assess(sample(depth, move, score, nodes, depth), {});
		REQUIRE(completed.decision == IterationDisposition::COMPLETED);
		return completed.next_state;
	}

	void check_same_state(const IterationState& actual, const IterationState& expected)
	{
		CHECK(actual.best_move == expected.best_move);
		CHECK(actual.best_score == expected.best_score);
		CHECK(actual.depth_completed == expected.depth_completed);
		CHECK(actual.nodes_at_completed_depth == expected.nodes_at_completed_depth);
		CHECK(actual.last_iteration_move == expected.last_iteration_move);
		CHECK(actual.search_was_stable == expected.search_was_stable);
		CHECK(actual.extra_depth_used == expected.extra_depth_used);
	}
} // namespace

TEST_CASE("Iteration policy rejects incomplete interrupted observations first", "[search][iteration_policy]")
{
	const auto previous = completed_state();
	const auto empty = assess(sample(4, Move{}, 0, 1, 0, true), previous);
	CHECK(empty.decision == IterationDisposition::REJECTED);
	CHECK(empty.reason == RejectionReason::INCOMPLETE);
	check_same_state(empty.next_state, previous);

	const auto below = assess(sample(4, kMove, 0, 99, 0, true), previous);
	CHECK(below.reason == RejectionReason::INCOMPLETE);
	check_same_state(below.next_state, previous);

	// Equality passes the first gate; the short PV then decides the result.
	const auto at_boundary = assess(sample(4, kMove, 0, 100, 0, true), previous);
	CHECK(at_boundary.reason == RejectionReason::SHORT_PV);
}

TEST_CASE("Iteration policy derives completion ratio and applies its exact boundary", "[search][iteration_policy]")
{
	const auto previous = completed_state(); // 400 main-tree nodes.
	const auto below = assess(sample(4, kMove, 10, 99, 2, true), previous);
	CHECK(below.metrics.completion_ratio == 99.0 / 400.0);
	CHECK(below.reason == RejectionReason::INCOMPLETE); // Node gate has priority.

	const auto at_boundary = assess(sample(4, kMove, 10, 100, 2, true), previous);
	CHECK(at_boundary.metrics.completion_ratio == 0.25);
	CHECK(at_boundary.decision == IterationDisposition::ACCEPTED_INTERRUPTED);
	CHECK(at_boundary.reason == RejectionReason::NONE);
	CHECK(at_boundary.metrics.score_delta == -290);
	CHECK(at_boundary.next_state.nodes_at_completed_depth == 100);

	const IterationThresholds ratio_only{50, 0.25, 0.5};
	const auto under_ratio = assess_iteration(sample(4, kOtherMove, 10, 99, 0, true), previous, ratio_only);
	CHECK(under_ratio.decision == IterationDisposition::REJECTED);
	CHECK(under_ratio.reason == RejectionReason::TOO_FEW_NODES);
	check_same_state(under_ratio.next_state, previous);
}

TEST_CASE("Iteration policy truncates the PV threshold and checks move change last", "[search][iteration_policy]")
{
	const auto previous = completed_state(8, kMove, 50, 400);
	// int(9 * 0.5) is 4; a changed move and short PV both fail, so PV wins.
	const auto short_pv = assess(sample(9, kOtherMove, 60, 100, 3, true), previous);
	CHECK(short_pv.reason == RejectionReason::SHORT_PV);
	const auto at_boundary = assess(sample(9, kOtherMove, 60, 100, 4, true), previous);
	CHECK(at_boundary.reason == RejectionReason::MOVE_CHANGED);
	CHECK(at_boundary.metrics.move_changed);
	CHECK(at_boundary.metrics.score_delta == 10);
	check_same_state(at_boundary.next_state, previous);

	const auto exact_move = assess(sample(9, kMove, 60, 100, 4, true), previous);
	CHECK(exact_move.decision == IterationDisposition::ACCEPTED_INTERRUPTED);
	CHECK_FALSE(exact_move.metrics.move_changed);
	const auto different_flags = assess(sample(9, kFlaggedMove, 60, 100, 4, true), previous);
	CHECK(different_flags.reason == RejectionReason::MOVE_CHANGED);
}

TEST_CASE("Iteration policy requires one PV move even when the configured ratio is zero", "[search][iteration_policy]")
{
	const auto previous = completed_state(1);
	const IterationThresholds floor_thresholds{100, 0.25, 0.0};
	const auto empty_pv = assess_iteration(sample(2, kMove, 20, 100, 0, true), previous, floor_thresholds);
	CHECK(empty_pv.decision == IterationDisposition::REJECTED);
	CHECK(empty_pv.reason == RejectionReason::SHORT_PV);
	check_same_state(empty_pv.next_state, previous);
	const auto one_move = assess_iteration(sample(2, kMove, 20, 100, 1, true), previous, floor_thresholds);
	CHECK(one_move.decision == IterationDisposition::ACCEPTED_INTERRUPTED);
	CHECK(one_move.reason == RejectionReason::NONE);
}

TEST_CASE("Iteration policy accepts a drawn score and preserves the last completed move on interruption",
          "[search][iteration_policy]")
{
	const auto previous = completed_state();
	const auto drawn = assess(sample(4, kMove, 0, 100, 2, true), previous);
	CHECK(drawn.decision == IterationDisposition::ACCEPTED_INTERRUPTED);
	CHECK(drawn.reason == RejectionReason::NONE);
	CHECK(drawn.next_state.best_score == 0);
	CHECK(drawn.next_state.best_move == kMove);
	CHECK(drawn.next_state.depth_completed == 4);
	CHECK(drawn.next_state.last_iteration_move == kMove);
	CHECK(drawn.next_state.search_was_stable);
	CHECK(drawn.metrics.score_delta == -300);

	const auto changed = assess(sample(4, kOtherMove, 0, 100, 2, true), previous);
	CHECK(changed.reason == RejectionReason::MOVE_CHANGED);
}

TEST_CASE("Iteration policy accepts the first interrupted depth without a previous denominator",
          "[search][iteration_policy]")
{
	const auto first = assess(sample(1, kMove, 20, 100, 0, true));
	CHECK(first.decision == IterationDisposition::ACCEPTED_INTERRUPTED);
	CHECK(first.metrics.completion_ratio == 1.0);
	CHECK(first.next_state.best_move == kMove);
	CHECK(first.next_state.depth_completed == 1);
	CHECK(first.next_state.last_iteration_move.is_null());

	const auto zero_nodes = completed_state(1, kMove, 20, 0);
	const auto next = assess(sample(2, kMove, 25, 100, 1, true), zero_nodes);
	CHECK(next.metrics.completion_ratio == 1.0);
	CHECK(next.decision == IterationDisposition::ACCEPTED_INTERRUPTED);
}

TEST_CASE("Iteration policy accepts completed observations regardless of quality gates", "[search][iteration_policy]")
{
	const auto previous = completed_state();
	const auto finished = assess(sample(4, Move{}, 0, 0, 0), previous);
	CHECK(finished.decision == IterationDisposition::COMPLETED);
	CHECK(finished.reason == RejectionReason::NONE);
	CHECK(finished.next_state.best_move.is_null());
	CHECK(finished.next_state.best_score == 0);
	CHECK(finished.next_state.depth_completed == 4);
	CHECK(finished.next_state.nodes_at_completed_depth == 0);
	CHECK(finished.next_state.last_iteration_move.is_null());
	CHECK_FALSE(finished.next_state.search_was_stable);
}

TEST_CASE("Iteration policy changes the last move only on completion and preserves extension state",
          "[search][iteration_policy]")
{
	const auto first = assess(sample(1, kMove, 10, 400, 1));
	const auto second = assess(sample(2, kOtherMove, 30, 400, 2), first.next_state);
	CHECK(second.decision == IterationDisposition::COMPLETED);
	CHECK(second.metrics.move_changed);
	CHECK_FALSE(second.next_state.search_was_stable);
	CHECK(second.next_state.last_iteration_move == kOtherMove);

	const auto extended = continue_iteration(second, true);
	CHECK(extended.stop_reason == IterationStopReason::NONE);
	CHECK(extended.next_state.extra_depth_used);
	const auto interrupted = assess(sample(3, kOtherMove, 40, 100, 2, true), extended.next_state);
	CHECK(interrupted.decision == IterationDisposition::ACCEPTED_INTERRUPTED);
	CHECK(interrupted.next_state.last_iteration_move == kOtherMove);
	CHECK(interrupted.next_state.extra_depth_used);
	const auto rejected = assess(sample(4, kMove, 50, 100, 2, true), interrupted.next_state);
	CHECK(rejected.reason == RejectionReason::MOVE_CHANGED);
	check_same_state(rejected.next_state, interrupted.next_state);
}

TEST_CASE("Iteration continuation allows one changed-move extension after the soft limit", "[search][iteration_policy]")
{
	const auto first = assess(sample(1, kMove, 10, 400, 1));
	const auto unchanged = assess(sample(2, kMove, 20, 400, 2), first.next_state);
	CHECK(continue_iteration(unchanged, false).stop_reason == IterationStopReason::NONE);
	const auto stable_stop = continue_iteration(unchanged, true);
	CHECK(stable_stop.stop_reason == IterationStopReason::SOFT_LIMIT);
	CHECK_FALSE(stable_stop.next_state.extra_depth_used);

	const auto changed = assess(sample(2, kOtherMove, 20, 400, 2), first.next_state);
	const auto no_soft_limit = continue_iteration(changed, false);
	CHECK(no_soft_limit.stop_reason == IterationStopReason::NONE);
	CHECK_FALSE(no_soft_limit.next_state.extra_depth_used);
	const auto extension = continue_iteration(changed, true);
	CHECK(extension.stop_reason == IterationStopReason::NONE);
	CHECK(extension.next_state.extra_depth_used);
	const auto next = assess(sample(3, kFlaggedMove, 30, 400, 3), extension.next_state);
	CHECK(next.metrics.move_changed);
	CHECK(continue_iteration(next, true).stop_reason == IterationStopReason::SOFT_LIMIT);
	CHECK(continue_iteration(next, false).next_state.extra_depth_used);

	const auto fresh = assess(sample(1, kMove, 10, 400, 1));
	CHECK_FALSE(fresh.next_state.extra_depth_used);
}

TEST_CASE("Iteration continuation applies soft-limit precedence and mate boundaries", "[search][iteration_policy]")
{
	const auto first = assess(sample(1, kMove, 0, 400, 1));
	for (const int score : {static_cast<int>(GameValues::Mate_Threshold), -GameValues::Mate_Threshold}) {
		const auto mate = assess(sample(2, kOtherMove, score, 400, 2), first.next_state);
		const auto continuation = continue_iteration(mate, true);
		CHECK(continuation.stop_reason == IterationStopReason::MATE);
		CHECK(continuation.next_state.extra_depth_used);
		CHECK(continue_iteration(mate, false).stop_reason == IterationStopReason::MATE);
	}

	const auto same_mate = assess(sample(2, kMove, GameValues::Mate_Threshold, 400, 2), first.next_state);
	CHECK(continue_iteration(same_mate, true).stop_reason == IterationStopReason::SOFT_LIMIT);
	const auto extended = continue_iteration(assess(sample(2, kOtherMove, 0, 400, 2), first.next_state), true);
	const auto later_mate = assess(sample(3, kOtherMove, GameValues::Mate_Threshold, 400, 3), extended.next_state);
	CHECK(continue_iteration(later_mate, true).stop_reason == IterationStopReason::SOFT_LIMIT);

	for (const int score :
	     {GameValues::Mate_Threshold - 1, -(GameValues::Mate_Threshold - 1), static_cast<int>(GameValues::Draw)}) {
		const auto near = assess(sample(2, kOtherMove, score, 400, 2), first.next_state);
		CHECK(continue_iteration(near, false).stop_reason == IterationStopReason::NONE);
	}
}
