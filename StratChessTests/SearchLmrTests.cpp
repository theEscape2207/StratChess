// SearchLmrTests.cpp — the late move reduction and its history adjustment.
//
// lmr_reduction() is pure, so the bounds and the placement of the adjustment are asserted on it
// directly; one search case proves pvs() actually feeds it a nonzero divisor's effect.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// The formula before the history adjustment existed, restated independently.
	int base_reduction(int depth, int move_number)
	{
		const int raw = static_cast<int>(std::sqrt(static_cast<double>(depth - 1)) *
		                                 std::sqrt(static_cast<double>(move_number - 1)));
		return std::min(std::max(1, raw), std::max(1, depth - 2));
	}

	// The widest ordinary quiet score: butterfly history plus two continuation rows.
	constexpr int kOrdinaryMax = 3 * ThreadData::HISTORY_MAX;
	constexpr int kKillerScore = 900'000;
	constexpr int kArmDivisors[] = {4096, 8192, 16384};
} // namespace

TEST_CASE("LMR: divisor 0 is the base formula, whatever the score", "[search][lmr]")
{
	for (int depth = 1; depth <= MAX_PLY; ++depth)
		for (int move = 1; move <= 64; ++move)
			for (const int score : {-kOrdinaryMax, 0, kOrdinaryMax, kKillerScore})
				REQUIRE(lmr_reduction(depth, move, score, 0) == base_reduction(depth, move));
}

TEST_CASE("LMR: the adjustment never leaves [1, depth - 2]", "[search][lmr]")
{
	for (int depth = 3; depth <= 40; ++depth)
		for (int move = 1; move <= 64; ++move)
			for (const int divisor : {1, 4096, 8192, 16384, 1'000'000})
				for (const int score : {-kOrdinaryMax, -1, 0, 1, kOrdinaryMax, kKillerScore}) {
					const int r = lmr_reduction(depth, move, score, divisor);
					REQUIRE(r >= 1);
					REQUIRE(depth - 1 - r >= 1);
				}
}

TEST_CASE("LMR: below depth 3 R stays 1, as before", "[search][lmr]")
{
	for (const int depth : {1, 2})
		for (const int score : {-kOrdinaryMax, 0, kOrdinaryMax})
			CHECK(lmr_reduction(depth, 10, score, 1) == 1);
}

TEST_CASE("LMR: a positive score reduces less even where the base sits on the cap", "[search][lmr]")
{
	// d = 6, m = 20: the raw product is 9.7 against a cap of 4. An adjustment applied before the
	// cap would leave R at 4.
	REQUIRE(base_reduction(6, 20) == 4);
	CHECK(lmr_reduction(6, 20, 8192, 8192) == 3);
	CHECK(lmr_reduction(6, 20, 2 * 8192, 8192) == 2);
}

TEST_CASE("LMR: a negative score reduces more only up to the cap", "[search][lmr]")
{
	// d = 10, m = 3: base 4, cap 8.
	REQUIRE(base_reduction(10, 3) == 4);
	CHECK(lmr_reduction(10, 3, -8192, 8192) == 5);
	CHECK(lmr_reduction(10, 3, -kOrdinaryMax, 4096) == 8);
	// Already at the cap: nothing further.
	CHECK(lmr_reduction(6, 20, -kOrdinaryMax, 4096) == 4);
}

TEST_CASE("LMR: the quotient truncates toward zero in both directions", "[search][lmr]")
{
	// d = 10, m = 3: base 4.
	constexpr int d = 8192;
	CHECK(lmr_reduction(10, 3, d - 1, d) == 4);
	CHECK(lmr_reduction(10, 3, d, d) == 3);
	CHECK(lmr_reduction(10, 3, -(d - 1), d) == 4);
	CHECK(lmr_reduction(10, 3, -d, d) == 5);
}

TEST_CASE("LMR: a displaced killer's score reduces it least at the arm divisors", "[search][lmr]")
{
	for (const int divisor : kArmDivisors)
		for (int depth = 3; depth <= 40; ++depth)
			REQUIRE(lmr_reduction(depth, 20, kKillerScore, divisor) == 1);
	// At the largest divisor the quotient is 0 and the base stands.
	CHECK(lmr_reduction(6, 3, kKillerScore, 1'000'000) == base_reduction(6, 3));
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
