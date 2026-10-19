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

namespace {
	// Turns tracking on through the one option that reads the trend.
	void track_eval_trend(const AIPerlexTestFixture& fix) { fix.set_reverse_futility_improving_relief(50); }

	constexpr const char* kQuietFen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
} // namespace

TEST_CASE("Eval stack: a depth-1 node writes its eval", "[search][improving]")
{
	// The depth every reader can sit at.
	AIPerlexTestFixture fix(kQuietFen);
	track_eval_trend(fix);
	fix.arm_clock();

	fix.search_node(1, /*ply=*/2);

	CHECK(fix.ply_eval(2) == fix.evaluate());
}

TEST_CASE("Eval stack: a depth-5 node writes its eval", "[search][improving]")
{
	// Above the readers' depths: a depth-3 reader's unreduced grandparent sits here.
	AIPerlexTestFixture fix(kQuietFen);
	track_eval_trend(fix);
	fix.arm_clock();

	fix.search_node(5, /*ply=*/2);

	CHECK(fix.ply_eval(2) == fix.evaluate());
}

TEST_CASE("Eval stack: the sentinel is written while no reader is on", "[search][improving]")
{
	AIPerlexTestFixture fix(kQuietFen);
	fix.set_ply_eval(2, 123);
	fix.arm_clock();

	fix.search_node(3, /*ply=*/2);

	CHECK(fix.ply_eval(2) == kNoStaticEval);
}

TEST_CASE("Eval stack: a singular verification keeps the node's eval in its slot", "[search][improving][singular]")
{
	// SearchSingularTests' baseline: a quiet position whose planted hash move is verified. The
	// verification frame shares the node's ply and position, so the node's eval must survive it.
	AIPerlexTestFixture fix(kQuietFen);
	track_eval_trend(fix);
	fix.set_singular_enabled(true);
	fix.set_singular_min_depth(4);
	fix.arm_clock();
	fix.store_main_entry_with_move(900, 4, /*ply=*/1, BoundType::LOWER, fix.first_sorted_move_uci());
	fix.clear_singular_telemetry();

	fix.search_node(4, /*ply=*/1);

	REQUIRE(fix.singular_verifications() == 1);
	CHECK(fix.ply_eval(1) == fix.evaluate());
}
