// SuitePolicyTests.cpp — Catch2 tests for TacticalTestRunner::evaluate_results()
//
// Roadmap acceptance criterion for the tactical suite: pass 90%+ tactical
// tests, 100% mate tests. evaluate_results enforces: overall pass rate >=
// required threshold AND zero failures in any category starting with "mate".

#include <catch_amalgamated.hpp>
#include "Tests/TacticalTestRunner.h"

using Testing::TacticalResult;
using Testing::TacticalTestRunner;

static TacticalResult make_result(const char* id, const char* category, bool passed)
{
	TacticalResult r;
	r.id = id;
	r.category = category;
	r.passed = passed;
	return r;
}

TEST_CASE("all positions passing yields ok", "[suite_policy]")
{
	std::vector<TacticalResult> results = {
	    make_result("M1-001", "mate_in_1", true),
	    make_result("WAC-001", "mate_in_2", true),
	    make_result("HANG-001", "hanging_piece", true),
	};
	const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
	CHECK(v.ok);
	CHECK(v.passed == 3);
	CHECK(v.total == 3);
	CHECK(v.failed_mate_ids.empty());
}

TEST_CASE("one mate failure fails the suite even above the overall threshold", "[suite_policy]")
{
	// 19/20 = 95% overall — above 90% — but the failed position is a mate.
	std::vector<TacticalResult> results;
	for (int i = 0; i < 19; ++i)
		results.push_back(make_result("T", "tactical_win", true));
	results.push_back(make_result("WAC-004", "mate_in_2", false));

	const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
	CHECK_FALSE(v.ok);
	REQUIRE(v.failed_mate_ids.size() == 1);
	CHECK(v.failed_mate_ids[0] == "WAC-004");
}

TEST_CASE("non-mate failures within the threshold still pass", "[suite_policy]")
{
	// 9/10 = 90%, failure is non-mate -> ok at 0.90 threshold.
	std::vector<TacticalResult> results;
	for (int i = 0; i < 9; ++i)
		results.push_back(make_result("T", "tactical_win", true));
	results.push_back(make_result("T-FAIL", "tactical_win", false));

	const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
	CHECK(v.ok);
	CHECK(v.failed_mate_ids.empty());
}

TEST_CASE("non-mate failures below the threshold fail", "[suite_policy]")
{
	// 8/10 = 80% < 90%.
	std::vector<TacticalResult> results;
	for (int i = 0; i < 8; ++i)
		results.push_back(make_result("T", "tactical_win", true));
	results.push_back(make_result("T-FAIL1", "tactical_win", false));
	results.push_back(make_result("T-FAIL2", "tactical_win", false));

	const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
	CHECK_FALSE(v.ok);
}

TEST_CASE("empty result set fails safe", "[suite_policy]")
{
	const auto v = TacticalTestRunner::evaluate_results({}, 0.90);
	CHECK_FALSE(v.ok);
	CHECK(v.total == 0);
}

// --- evaluate_stability() -------------------------------------------------
// Stability policy: ok iff at least one run, all runs the same size, every
// run individually satisfies evaluate_results(), and no position's pass flag
// differs between runs (a "flip" = nondeterminism).

static std::vector<TacticalResult>
make_run(std::initializer_list<std::pair<const char*, bool>> entries)
{
	std::vector<TacticalResult> run;
	for (const auto& [id, passed] : entries)
		run.push_back(make_result(id, "tactical_win", passed));
	return run;
}

TEST_CASE("identical passing runs are stable", "[suite_policy]")
{
	const auto run = make_run({{"A", true}, {"B", true}, {"C", true}});
	const auto v = TacticalTestRunner::evaluate_stability({run, run, run}, 0.90);
	CHECK(v.ok);
	CHECK(v.runs == 3);
	CHECK(v.flipped_ids.empty());
	CHECK(v.failed_run_indices.empty());
}

TEST_CASE("a single run degenerates to the per-run gate verdict", "[suite_policy]")
{
	const auto run = make_run({{"A", true}, {"B", true}});
	const auto v = TacticalTestRunner::evaluate_stability({run}, 0.90);
	CHECK(v.ok);
	CHECK(v.runs == 1);
}

TEST_CASE("a position flipping between runs fails stability", "[suite_policy]")
{
	// 10 positions per run so one failure (9/10 = 90%) still passes the
	// per-run gate — the flip alone must fail stability.
	std::vector<TacticalResult> run_pass, run_flip;
	for (int i = 0; i < 9; ++i) {
		run_pass.push_back(make_result("T", "tactical_win", true));
		run_flip.push_back(make_result("T", "tactical_win", true));
	}
	run_pass.push_back(make_result("FLIP", "tactical_win", true));
	run_flip.push_back(make_result("FLIP", "tactical_win", false));

	const auto v = TacticalTestRunner::evaluate_stability({run_pass, run_flip}, 0.90);
	CHECK_FALSE(v.ok);
	CHECK(v.failed_run_indices.empty()); // both runs pass the per-run gate
	REQUIRE(v.flipped_ids.size() == 1);
	CHECK(v.flipped_ids[0] == "FLIP");
}

TEST_CASE("a consistent mate failure fails every run but is not a flip", "[suite_policy]")
{
	std::vector<TacticalResult> run;
	for (int i = 0; i < 19; ++i)
		run.push_back(make_result("T", "tactical_win", true));
	run.push_back(make_result("WAC-004", "mate_in_2", false)); // fails per-run gate

	const auto v = TacticalTestRunner::evaluate_stability({run, run}, 0.90);
	CHECK_FALSE(v.ok);
	CHECK(v.flipped_ids.empty()); // deterministic — no flip
	REQUIRE(v.failed_run_indices.size() == 2);
	CHECK(v.failed_run_indices[0] == 1);
	CHECK(v.failed_run_indices[1] == 2);
}

TEST_CASE("a consistent tolerated non-mate failure stays stable", "[suite_policy]")
{
	// 9/10 = 90% with a non-mate failure passes the gate; failing identically
	// in every run is deterministic, so stability passes too.
	std::vector<TacticalResult> run;
	for (int i = 0; i < 9; ++i)
		run.push_back(make_result("T", "tactical_win", true));
	run.push_back(make_result("T-FAIL", "tactical_win", false));

	const auto v = TacticalTestRunner::evaluate_stability({run, run, run}, 0.90);
	CHECK(v.ok);
}

TEST_CASE("empty run set fails stability safe", "[suite_policy]")
{
	const auto v = TacticalTestRunner::evaluate_stability({}, 0.90);
	CHECK_FALSE(v.ok);
	CHECK(v.runs == 0);
}

TEST_CASE("mismatched run sizes fail stability safe", "[suite_policy]")
{
	const auto a = make_run({{"A", true}, {"B", true}});
	const auto b = make_run({{"A", true}});
	const auto v = TacticalTestRunner::evaluate_stability({a, b}, 0.90);
	CHECK_FALSE(v.ok);
	CHECK_FALSE(v.comparable);
}
