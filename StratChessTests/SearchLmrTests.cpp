// SearchLmrTests.cpp — the late move reduction, its sqrt and log bases and its history adjustment.
//
// lmr_reduction() is pure, so the bounds and the placement of the adjustment are asserted on it
// directly; two search cases prove pvs() actually feeds it a nonzero history or log divisor.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// The base formula alone, restated independently of lmr_reduction().
	int base_reduction(int depth, int move_number)
	{
		const int raw = static_cast<int>(std::sqrt(static_cast<double>(depth - 1)) *
		                                 std::sqrt(static_cast<double>(move_number - 1)));
		return std::min(std::max(1, raw), std::max(1, depth - 2));
	}

	// The widest ordinary quiet score: butterfly history plus two continuation rows.
	constexpr int kOrdinaryMax = 3 * ThreadData::HISTORY_MAX;
	constexpr int kKillerScore = 900'000;
	// Divisors small against the ordinary score range, the default among them.
	constexpr int kSmallDivisors[] = {64, 256, 512};
} // namespace

TEST_CASE("LMR: divisor 0 is the base formula, whatever the score", "[search][lmr]")
{
	for (int depth = 1; depth <= MAX_PLY; ++depth)
		for (int move = 1; move <= 64; ++move)
			for (const int score : {-kOrdinaryMax, 0, kOrdinaryMax, kKillerScore})
				REQUIRE(lmr_reduction(depth, move, score, 0, 0, 0) == base_reduction(depth, move));
}

TEST_CASE("LMR: the adjustment never leaves [1, depth - 2]", "[search][lmr]")
{
	for (int depth = 3; depth <= 40; ++depth)
		for (int move = 1; move <= 64; ++move)
			for (const int divisor : {1, 64, 4096, 8192, 16384, 1'000'000})
				for (const int score : {-kOrdinaryMax, -1, 0, 1, kOrdinaryMax, kKillerScore}) {
					const int r = lmr_reduction(depth, move, score, divisor, 0, 0);
					REQUIRE(r >= 1);
					REQUIRE(depth - 1 - r >= 1);
				}
}

TEST_CASE("LMR: below depth 3 R is always 1", "[search][lmr]")
{
	for (const int depth : {1, 2})
		for (const int score : {-kOrdinaryMax, 0, kOrdinaryMax})
			CHECK(lmr_reduction(depth, 10, score, 1, 0, 0) == 1);
}

TEST_CASE("LMR: a positive score reduces less even where the base sits on the cap", "[search][lmr]")
{
	// d = 6, m = 20: the raw product is 9.7 against a cap of 4. An adjustment applied before the
	// cap would leave R at 4.
	REQUIRE(base_reduction(6, 20) == 4);
	CHECK(lmr_reduction(6, 20, 8192, 8192, 0, 0) == 3);
	CHECK(lmr_reduction(6, 20, 2 * 8192, 8192, 0, 0) == 2);
}

TEST_CASE("LMR: a negative score reduces more only up to the cap", "[search][lmr]")
{
	// d = 10, m = 3: base 4, cap 8.
	REQUIRE(base_reduction(10, 3) == 4);
	CHECK(lmr_reduction(10, 3, -8192, 8192, 0, 0) == 5);
	CHECK(lmr_reduction(10, 3, -kOrdinaryMax, 4096, 0, 0) == 8);
	// Already at the cap: nothing further.
	CHECK(lmr_reduction(6, 20, -kOrdinaryMax, 4096, 0, 0) == 4);
}

TEST_CASE("LMR: the quotient truncates toward zero in both directions", "[search][lmr]")
{
	// d = 10, m = 3: base 4.
	constexpr int d = 8192;
	CHECK(lmr_reduction(10, 3, d - 1, d, 0, 0) == 4);
	CHECK(lmr_reduction(10, 3, d, d, 0, 0) == 3);
	CHECK(lmr_reduction(10, 3, -(d - 1), d, 0, 0) == 4);
	CHECK(lmr_reduction(10, 3, -d, d, 0, 0) == 5);
	// Divisor 1 shifts by the whole score, within the bounds.
	CHECK(lmr_reduction(10, 3, 2, 1, 0, 0) == 2);
	CHECK(lmr_reduction(10, 3, -3, 1, 0, 0) == 7);
}

TEST_CASE("LMR: a small divisor acts almost as a sign switch", "[search][lmr]")
{
	// d = 10, m = 20: base 8 at the cap. At divisor 64 a score of 64 * (cap - 1) is already R = 1.
	REQUIRE(base_reduction(10, 20) == 8);
	CHECK(lmr_reduction(10, 20, 63, 64, 0, 0) == 8);
	CHECK(lmr_reduction(10, 20, 64, 64, 0, 0) == 7);
	CHECK(lmr_reduction(10, 20, 7 * 64, 64, 0, 0) == 1);
	CHECK(lmr_reduction(10, 20, kOrdinaryMax, 64, 0, 0) == 1);
}

TEST_CASE("LMR: a displaced killer's score reduces it least at small divisors", "[search][lmr]")
{
	for (const int divisor : kSmallDivisors)
		for (int depth = 3; depth <= 40; ++depth)
			REQUIRE(lmr_reduction(depth, 20, kKillerScore, divisor, 0, 0) == 1);
	// At the largest divisor the quotient is 0 and the base stands.
	CHECK(lmr_reduction(6, 3, kKillerScore, 1'000'000, 0, 0) == base_reduction(6, 3));
}

TEST_CASE("LMR: log divisor 0 is the sqrt reduction, whatever the log base", "[search][lmr]")
{
	for (int depth = 1; depth <= MAX_PLY; ++depth)
		for (int move = 1; move <= 64; ++move)
			for (const int log_base : {75, 1000})
				for (const int divisor : {0, 64, 8192})
					for (const int score : {-kOrdinaryMax, 0, kOrdinaryMax, kKillerScore})
						REQUIRE(lmr_reduction(depth, move, score, divisor, log_base, 0) ==
						        lmr_reduction(depth, move, score, divisor, 0, 0));
}

TEST_CASE("LMR: the screened log bases stay inside [1, depth - 2]", "[search][lmr]")
{
	// Base / divisor pairs in hundredths: 0.75 / 2.25, 1.00 / 1.75 and 1.00 / 1.50.
	for (const auto& [log_base, log_divisor] : {std::pair{75, 225}, std::pair{100, 175}, std::pair{100, 150}})
		for (int depth = 3; depth < MAX_PLY; ++depth)
			for (int move = 1; move <= static_cast<int>(MoveList::MAX_MOVES); ++move)
				for (const int divisor : {0, 64})
					for (const int score : {-kOrdinaryMax, 0, kOrdinaryMax, kKillerScore}) {
						const int r = lmr_reduction(depth, move, score, divisor, log_base, log_divisor);
						REQUIRE(r >= 1);
						REQUIRE(depth - 1 - r >= 1);
					}
}

TEST_CASE("LMR: log base 0.75 / 2.25 reduces less than the cap at depth 10 and 4", "[search][lmr]")
{
	// d = 10, m = 5/10/20/40: R = 2/3/3/4, so the reduced child searches at 7/6/6/5.
	const auto child_depth = [](int move) { return 10 - 1 - lmr_reduction(10, move, 0, 0, 75, 225); };
	CHECK(child_depth(5) == 7);
	CHECK(child_depth(10) == 6);
	CHECK(child_depth(20) == 6);
	CHECK(child_depth(40) == 5);
	// Depth 4 is not always capped: m = 3 gives 0.75 + 1.52 / 2.25 = 1.43, so R = 1 against the
	// sqrt base's capped 2.
	CHECK(lmr_reduction(4, 3, 0, 0, 75, 225) == 1);
	CHECK(lmr_reduction(4, 3, 0, 0, 0, 0) == 2);
}

TEST_CASE("LMR: a nonzero log divisor reaches the search", "[search][lmr][nodes]")
{
	constexpr const char* fen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
	AIPerlexTestFixture sqrt_base(fen);
	sqrt_base.set_lmr_log(0, 0);
	sqrt_base.get_move_at_threads(1, 8);

	AIPerlexTestFixture log_base(fen);
	log_base.set_lmr_log(75, 225);
	log_base.get_move_at_threads(1, 8);

	CHECK(log_base.mainnodes() != sqrt_base.mainnodes());
}

TEST_CASE("LMR: a nonzero divisor reaches the search", "[search][lmr][nodes]")
{
	constexpr const char* fen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4";
	AIPerlexTestFixture off(fen);
	off.set_lmr_history_divisor(0);
	off.get_move_at_threads(1, 8);

	AIPerlexTestFixture on(fen);
	on.set_lmr_history_divisor(256);
	on.get_move_at_threads(1, 8);

	CHECK(on.mainnodes() != off.mainnodes());
}
