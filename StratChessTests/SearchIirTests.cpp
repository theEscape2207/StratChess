// SearchIirTests.cpp — internal iterative reduction: a non-PV node with no hash move searches one ply
// shallower.
//
// The eligibility cases move one guard at a time off a passing baseline. The node cases read the depth
// the node stored, which is the depth it actually searched.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// Quiet middlegame position with no tactics a depth-5 null-move cut would short-circuit.
	constexpr const char* kFen = "r1bq1rk1/pp2ppbp/2np1np1/8/2PNP3/2N1B3/PP2BPPP/R2QK2R w KQ - 0 9";
	constexpr int kDepth = 5;
	constexpr int kPly = 1;

	// The baseline call: depth 5, non-PV, not in check, no exclusion, no hash move.
	bool eligible(const AIPerlexTestFixture& fix, int depth = kDepth, bool is_pv_node = false, bool in_check = false,
	              bool is_exclusion_frame = false, Move hash_move = Move::EmptyMove())
	{
		return fix.iir_eligible(depth, is_pv_node, in_check, is_exclusion_frame, hash_move);
	}

	int stored_depth(const AIPerlexTestFixture& fix)
	{
		fix.arm_clock();
		fix.search_node(kDepth, kPly, 0, 1, /*is_pv_node=*/false);
		const auto entry = fix.probe_tt(kPly);
		REQUIRE(entry.has_value());
		REQUIRE(entry->phase == SearchPhase::MAIN);
		return entry->depth;
	}
} // namespace

TEST_CASE("IIR eligibility", "[search][iir]")
{
	AIPerlexTestFixture fix(kFen);
	fix.set_iir(true);
	REQUIRE(eligible(fix));

	CHECK_FALSE(eligible(fix, kDepth - 1));
	CHECK(eligible(fix, MAX_PLY));
	CHECK_FALSE(eligible(fix, kDepth, /*is_pv_node=*/true));
	CHECK_FALSE(eligible(fix, kDepth, false, /*in_check=*/true));
	CHECK_FALSE(eligible(fix, kDepth, false, false, /*is_exclusion_frame=*/true));
	CHECK_FALSE(eligible(fix, kDepth, false, false, false, AnyLegalMove()));

	SECTION("the depth floor is the knob")
	{
		fix.set_iir(true, 4);
		CHECK(eligible(fix, 4));
		CHECK_FALSE(eligible(fix, 3));
	}

	SECTION("the flag")
	{
		fix.set_iir(false);
		CHECK_FALSE(eligible(fix));
	}
}

TEST_CASE("IIR: a node without a hash move stores the reduced depth", "[search][iir]")
{
	SECTION("on")
	{
		AIPerlexTestFixture fix(kFen);
		fix.set_iir(true);
		CHECK(stored_depth(fix) == kDepth - 1);
	}

	SECTION("off")
	{
		AIPerlexTestFixture fix(kFen);
		fix.set_iir(false);
		CHECK(stored_depth(fix) == kDepth);
	}

	SECTION("on, with a hash move")
	{
		AIPerlexTestFixture fix(kFen);
		fix.set_iir(true);
		fix.store_hash_move("a2a3", kPly);
		CHECK(stored_depth(fix) == kDepth);
	}
}
