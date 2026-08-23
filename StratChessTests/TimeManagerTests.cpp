#include <catch2/catch_test_macros.hpp>
#include "Utils/TimeUtils.h"
#include "Utils/TimeManager.h"
#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ============================================================
// Formula tests — deterministic, no sleeps, < 1 ms each
// ============================================================
TEST_CASE("compute_budget: blitz midgame (3+2, 150 s remaining)", "[time_mgr]")
{
	// Example A from design doc: base = 149950/30 + 1600 ≈ 6598 ms
	auto b = Engine::compute_budget(150'000ms, 2'000ms, 0);
	REQUIRE(b.soft.count() >= 6'000);
	REQUIRE(b.soft.count() <= 7'500);
	// hard = min(base*1.5, usable*0.5) ≈ 9900
	REQUIRE(b.hard.count() >= b.soft.count());
	REQUIRE(b.hard.count() <= 75'000);
}

TEST_CASE("compute_budget: classical with moves_to_go (60 min, 20 moves left)", "[time_mgr]")
{
	// Example B from design doc: base = 3599950/20 = 180000 ms = 3 min
	auto b = Engine::compute_budget(3'600'000ms, 0ms, 20);
	REQUIRE(b.soft.count() >= 179'000);
	REQUIRE(b.soft.count() <= 181'000);
	// hard = min(180000*1.5, 3599950*0.5) = 270000 ms
	REQUIRE(b.hard.count() >= 269'000);
	REQUIRE(b.hard.count() <= 271'000);
}

TEST_CASE("compute_budget: increment-heavy time trouble (200 ms remaining, 5 s increment)", "[time_mgr]")
{
	// A 5 s increment dwarfs the 200 ms clock, so the cap decides: usable = 150,
	// cap = 75. Spending the increment before it is credited would forfeit.
	auto b = Engine::compute_budget(200ms, 5'000ms, 0);
	REQUIRE(b.soft.count() == 75);
	REQUIRE(b.hard.count() == 75);
	REQUIRE(b.hard.count() <= 200);
}

TEST_CASE("compute_budget: clock below overhead yields a zero budget", "[time_mgr]")
{
	// 30 ms is less than the 50 ms overhead: nothing is safely spendable, so the
	// caller must move immediately rather than search on borrowed time.
	auto b = Engine::compute_budget(30ms, 0ms, 0);
	REQUIRE(b.soft.count() == 0);
	REQUIRE(b.hard.count() == 0);
}

TEST_CASE("compute_budget: zero increment, no moves_to_go", "[time_mgr]")
{
	auto b = Engine::compute_budget(60'000ms, 0ms, 0);
	// base = 59950/30 ≈ 1998 ms ≈ 2 s
	REQUIRE(b.soft.count() >= 1'800);
	REQUIRE(b.soft.count() <= 2'200);
	REQUIRE(b.hard.count() >= b.soft.count());
}

TEST_CASE("compute_budget: hard is always >= soft invariant", "[time_mgr]")
{
	struct Case {
		int remaining_ms, inc_ms, mtg;
	};
	for (auto [r, i, m] : std::initializer_list<Case>{{5000, 0, 0}, {500, 500, 5}, {100000, 1000, 15}, {50, 0, 0}}) {
		auto b = Engine::compute_budget(std::chrono::milliseconds(r), std::chrono::milliseconds(i), m);
		CAPTURE(r, i, m);
		REQUIRE(b.hard.count() >= b.soft.count());
		REQUIRE(b.soft.count() >= 0);
		REQUIRE(b.hard.count() <= r);
	}
}

TEST_CASE("compute_budget: hard never exceeds remaining", "[time_mgr]")
{
	// The assertion that would have caught issue #204 without playing a game:
	// a budget larger than the clock it was drawn from is a forfeit waiting to
	// happen, whatever the increment or horizon says.
	for (int r : {0, 1, 10, 30, 49, 50, 51, 100, 150, 200, 250, 500, 1'000, 10'000, 100'000}) {
		for (int i : {0, 20, 50, 100, 1'000, 5'000}) {
			for (int m : {0, 1, 5, 30}) {
				auto b = Engine::compute_budget(std::chrono::milliseconds(r), std::chrono::milliseconds(i), m);
				CAPTURE(r, i, m, b.soft.count(), b.hard.count());
				REQUIRE(b.soft.count() >= 0);
				REQUIRE(b.hard.count() >= b.soft.count());
				REQUIRE(b.hard.count() <= r);
				// Half of what is left, so the following move can still be paid for
				REQUIRE(b.hard.count() <= std::max(r - 50, 0) / 2);
			}
		}
	}
}

TEST_CASE("compute_budget: sub-100 ms increments do not drain the clock", "[time_mgr]")
{
	// Issue #204's failure mode: at an increment below the old 100 ms floor every
	// move cost more than it repaid, so the clock walked down to a forfeit.
	// Spending the *hard* limit every move is the worst case; the search normally
	// stops at soft. The clock must instead settle on a positive fixed point.
	// The clock settles where hard == increment, i.e. (r - 50) / 2 == inc. Integer
	// division makes both r = 50 + 2*inc and r + 1 satisfy that, so the resting
	// value is a two-wide band and which end it reaches depends on the approach.
	// Assert the band, not one of its two members.
	for (int inc : {20, 50}) { // 2+0.02 and 5+0.05
		const int64_t settled_min = 50 + 2 * inc;
		auto clock = std::chrono::milliseconds(120'000);
		std::vector<int64_t> tail;
		for (int move = 0; move < 400; ++move) {
			auto b = Engine::compute_budget(clock, std::chrono::milliseconds(inc), 0);
			clock -= b.hard;
			CAPTURE(inc, move, b.hard.count(), clock.count());
			REQUIRE(clock.count() >= 0); // never overspends what is there
			clock += std::chrono::milliseconds(inc);
			if (move >= 380)
				tail.push_back(clock.count());
		}
		CAPTURE(inc, clock.count(), settled_min);
		REQUIRE(clock.count() >= settled_min);
		REQUIRE(clock.count() <= settled_min + 1);
		// Converged, not merely still positive
		REQUIRE(std::equal(tail.begin() + 1, tail.end(), tail.begin()));
	}
}

// ============================================================
// TimeManager timing tests — short sleeps, total < 300 ms
// ============================================================
TEST_CASE("TimeManager: two-arg start — soft fires before hard", "[time_mgr]")
{
	chess::TimeManager tm;
	tm.start(20ms, 60ms);

	REQUIRE_FALSE(tm.should_stop_search());
	REQUIRE_FALSE(tm.should_stop_iteration());

	std::this_thread::sleep_for(25ms);
	// soft should have fired, hard should not yet
	REQUIRE(tm.should_stop_iteration());
	REQUIRE_FALSE(tm.should_stop_search());

	std::this_thread::sleep_for(40ms);
	// now hard fires too
	REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: one-arg start — soft == hard (backward compat)", "[time_mgr]")
{
	chess::TimeManager tm;
	tm.start(20ms);

	REQUIRE_FALSE(tm.should_stop_search());
	std::this_thread::sleep_for(25ms);
	REQUIRE(tm.should_stop_iteration());
	REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: stop() fires should_stop_search immediately", "[time_mgr]")
{
	chess::TimeManager tm;
	tm.start(10'000ms, 20'000ms);
	REQUIRE_FALSE(tm.should_stop_search());
	tm.stop();
	REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: elapsed() is monotone", "[time_mgr]")
{
	chess::TimeManager tm;
	tm.start(5'000ms);
	auto t1 = tm.elapsed();
	std::this_thread::sleep_for(10ms);
	auto t2 = tm.elapsed();
	REQUIRE(t2 > t1);
}
