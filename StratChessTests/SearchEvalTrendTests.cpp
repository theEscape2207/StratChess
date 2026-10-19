// SearchEvalTrendTests.cpp — the eval stack pvs() keeps in ThreadData::ply_eval and the trend
// eval_trend() reads from it.
//
// eval_trend() is pure, so the improving rule is asserted on it directly.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// An eval stack with the ancestors named and every other slot unset.
	std::array<int16_t, MAX_PLY> evals(std::initializer_list<std::pair<int, int16_t>> slots)
	{
		std::array<int16_t, MAX_PLY> stack{};
		stack.fill(kNoStaticEval);
		for (const auto& [ply, eval] : slots)
			stack[ply] = eval;
		return stack;
	}
} // namespace

TEST_CASE("Improving: the eval two plies back decides", "[search][improving]")
{
	CHECK(eval_trend(evals({{4, 10}, {6, 20}}), 6) == EvalTrend::Improving);
	CHECK(eval_trend(evals({{4, 20}, {6, 20}}), 6) == EvalTrend::NotImproving);
	CHECK(eval_trend(evals({{4, 30}, {6, 20}}), 6) == EvalTrend::NotImproving);
	// ply - 1 is the opponent's view and plays no part.
	CHECK(eval_trend(evals({{4, 30}, {5, -100}, {6, 20}}), 6) == EvalTrend::NotImproving);
}

TEST_CASE("Improving: an in-check ancestor falls back to ply - 4", "[search][improving]")
{
	CHECK(eval_trend(evals({{2, 30}, {6, 20}}), 6) == EvalTrend::NotImproving);
	CHECK(eval_trend(evals({{2, 10}, {6, 20}}), 6) == EvalTrend::Improving);
	CHECK(eval_trend(evals({{0, 20}, {4, 20}}), 4) == EvalTrend::NotImproving);
}

TEST_CASE("Improving: no evidence is Unknown", "[search][improving]")
{
	// Both ancestors missing.
	CHECK(eval_trend(evals({{6, 20}}), 6) == EvalTrend::Unknown);
	// ply < 2.
	CHECK(eval_trend(evals({{0, 20}, {1, 20}}), 0) == EvalTrend::Unknown);
	CHECK(eval_trend(evals({{0, 20}, {1, 20}}), 1) == EvalTrend::Unknown);
	// ply 2 and 3 with no eval at ply - 2, and no ply - 4 to fall back to.
	CHECK(eval_trend(evals({{2, -50}}), 2) == EvalTrend::Unknown);
	CHECK(eval_trend(evals({{3, -50}}), 3) == EvalTrend::Unknown);
	// The node itself has none.
	CHECK(eval_trend(evals({{4, 30}}), 6) == EvalTrend::Unknown);
}
