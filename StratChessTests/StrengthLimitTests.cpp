// StrengthLimitTests.cpp — Catch2 [strength] tests for the pure UCI_Elo policy: the Elo -> level table, the
// pick depth and the weakened pick. What reaches the wire is UCIStrengthLimitTests.cpp's.

#include <catch2/catch_test_macros.hpp>
#include "StrengthLimit.h"
#include "defines.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using StrengthLimit::kMaxElo;
using StrengthLimit::kMaxLevel;
using StrengthLimit::kMinElo;
using StrengthLimit::level_for_elo;
using StrengthLimit::pick;
using StrengthLimit::pick_depth;

namespace {
	constexpr int kPawn = 100;
	constexpr int kSeeds = 2000;

	// The largest gap to rank 1 the pick can ever choose: an exact bound of the integer formula.
	double gap_bound(int delta, double level)
	{
		const double weakness = 120.0 - 2.0 * level;
		return delta * (static_cast<int>(weakness) - 1) / (128.0 - weakness);
	}

	size_t pick_of(const std::vector<int>& scores, double level, uint64_t seed) { return pick(scores, level, seed); }
} // namespace

// ---------------------------------------------------------------------------
// The table and the pick depth
// ---------------------------------------------------------------------------

TEST_CASE("StrengthLimit table: endpoints, clamping and monotonicity", "[strength]")
{
	CHECK(level_for_elo(kMinElo) == 0.0);
	CHECK(level_for_elo(kMaxElo) == kMaxLevel);
	CHECK(level_for_elo(kMinElo - 500) == 0.0);
	CHECK(level_for_elo(0) == 0.0);
	CHECK(level_for_elo(kMaxElo + 500) == kMaxLevel);

	double previous = level_for_elo(kMinElo - 1);
	for (int elo = kMinElo; elo <= kMaxElo + 1; ++elo) {
		const double level = level_for_elo(elo);
		INFO("elo " << elo);
		REQUIRE(level >= previous);
		REQUIRE(level >= 0.0);
		REQUIRE(level <= kMaxLevel);
		previous = level;
	}
}

TEST_CASE("StrengthLimit table: the pick depth either side of a depth boundary", "[strength]")
{
	// Each depth's first Elo is ceil(inverse(level)): the Elo below it plays the previous depth, so a
	// gauntlet setting ceil(inverse(L)) lands on depth 1 + L. Found by scanning, so the table may change.
	int depth = pick_depth(level_for_elo(kMinElo));
	CHECK(depth == 1);
	for (int elo = kMinElo + 1; elo <= kMaxElo; ++elo) {
		const int next = pick_depth(level_for_elo(elo));
		if (next == depth)
			continue;
		INFO("elo " << elo);
		REQUIRE(next == depth + 1);
		CHECK(level_for_elo(elo - 1) < depth);
		CHECK(level_for_elo(elo) >= depth);
		depth = next;
	}
	CHECK(depth == 20);
	CHECK(pick_depth(level_for_elo(kMaxElo)) == 20);
}

// ---------------------------------------------------------------------------
// The pick
// ---------------------------------------------------------------------------

TEST_CASE("StrengthLimit pick: never picks past the gap bound", "[strength]")
{
	REQUIRE(g_iPieceValues[PAWN >> 1] == kPawn);
	// Spreads above one pawn, so delta sits at its cap and the bound is the one that can be violated;
	// with every gap below delta the bound could not fail.
	const std::vector<std::vector<int>> sets = {
	    {40, 30, 0, -150, -400, -1500, -2000},
	    {0, -100, -177, -400},
	    {300, 299, 120, 50, -1200},
	};
	for (const double level : {0.0, 0.5, 4.0094, 10.3, 18.99, 19.0}) {
		for (const auto& scores : sets) {
			const int delta = std::min(scores.front() - scores.back(), kPawn);
			REQUIRE(delta == kPawn);
			const double bound = gap_bound(delta, level);
			bool picked_below_rank1 = false;
			for (uint64_t seed = 0; seed < kSeeds; ++seed) {
				const size_t i = pick_of(scores, level, seed);
				REQUIRE(i < scores.size());
				INFO("level " << level << " seed " << seed << " index " << i);
				REQUIRE(scores.front() - scores[i] <= bound);
				picked_below_rank1 |= i != 0;
			}
			// The bound is checked against real picks, not rank 1 alone, wherever rank 2 is near.
			if (scores.front() - scores[1] <= 10)
				CHECK(picked_below_rank1);
		}
	}
}

TEST_CASE("StrengthLimit pick: the level-0 bound of 1487 cp is reachable, 1488 is not", "[strength]")
{
	// At level 0 a 1487 cp gap ties rank 1 when rank 2 draws the top r and rank 1 draws zero; the
	// later rank wins ties.
	const std::vector<int> reachable = {0, -1487, -1600};
	bool reached = false;
	for (uint64_t seed = 0; seed < 400'000 && !reached; ++seed)
		reached = pick_of(reachable, 0.0, seed) == 1;
	CHECK(reached);

	const std::vector<int> unreachable = {0, -1488, -1600};
	for (uint64_t seed = 0; seed < 100'000; ++seed)
		REQUIRE(pick_of(unreachable, 0.0, seed) == 0);
}

TEST_CASE("StrengthLimit pick: mates", "[strength]")
{
	const int mate_in_1 = GameValues::Mate - 1;
	for (uint64_t seed = 0; seed < kSeeds; ++seed) {
		INFO("seed " << seed);
		// Two winning mates: the slower one would adjust above the faster at level 0.
		REQUIRE(pick_of({mate_in_1, GameValues::Mate - 3, 119, 0}, 0.0, seed) == 0);
		// Two losing mates: the slowest proven loss, rank 1, is played.
		REQUIRE(pick_of({-(GameValues::Mate - 5), -(GameValues::Mate - 1)}, 0.0, seed) == 0);
		// A finite rank 1 never loses to a losing mate.
		REQUIRE(pick_of({0, -(GameValues::Mate - 2)}, 0.0, seed) == 0);
		// The mated rank is dropped before delta: delta is 50, as for the set without it.
		REQUIRE(pick_of({50, 0, -(GameValues::Mate - 10)}, 0.0, seed) == pick_of({50, 0}, 0.0, seed));
	}
}

TEST_CASE("StrengthLimit pick: ties go to the later rank", "[strength]")
{
	// Equal scores make delta 0, so every adjusted score is equal and the last rank wins.
	for (uint64_t seed = 0; seed < 100; ++seed) {
		CHECK(pick_of({25, 25}, 5.0, seed) == 1);
		CHECK(pick_of({0, 0, 0, 0}, 19.0, seed) == 3);
	}
}

TEST_CASE("StrengthLimit pick: a lone rank and a fixed seed", "[strength]")
{
	CHECK(pick_of({17}, 0.0, 1) == 0);
	CHECK(pick_of({GameValues::Mate - 1}, 0.0, 1) == 0);

	const std::vector<int> scores = {30, 20, 5, -60};
	for (uint64_t seed = 0; seed < 200; ++seed)
		REQUIRE(pick_of(scores, 3.5, seed) == pick_of(scores, 3.5, seed));
}
