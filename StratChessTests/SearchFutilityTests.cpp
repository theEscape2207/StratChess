// SearchFutilityTests.cpp — reverse futility pruning (#87, Stage 1).
//
// Two groups. The eligibility tests move one guard at a time off a baseline that is known to
// pass, so a failure names the guard that broke; removing any single guard from
// reverse_futility_eligible() makes exactly one of them fail. The cutoff tests then drive a
// whole pvs() node and check what the guard is allowed to do once it fires: return beta, search
// no children, and store nothing.
//
// The feature ships disabled, so every case here turns it on first.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// Quiet middlegame position, both sides far above the zugzwang floor.
	constexpr const char* kBaselineFen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	// King and pawn against king: the side to move has no non-pawn piece, which is the endgame
	// class the material floor exists for (#66).
	constexpr const char* kZugzwangFen = "8/8/8/4k3/8/4K3/4P3/8 w - - 0 1";

	// White a queen and a bishop up against a bare king, so the static evaluation clears any
	// small beta by far more than the margin. Two non-pawn pieces, so the material floor passes.
	constexpr const char* kWinningFen = "4k3/8/8/8/8/8/8/3QKB2 w - - 0 1";

	// A composed stalemate that clears every guard: White to move, not in check, no legal move,
	// and two non-pawn pieces so the material floor does not save it. Verified stalemate against
	// an independent oracle (python-chess). This is the one position class where the cutoff can
	// return a score for a node that is really a draw -- see the test at the bottom of this file.
	constexpr const char* kStalemateFen = "k7/8/8/8/5p1p/1p2pPpP/1P2P1P1/B4nNK w - - 0 1";

	// The baseline call: shallow, non-PV, not in check, no exclusion, beta nowhere near a mate
	// score. Every case below changes exactly one of these.
	bool eligible(const AIPerlexTestFixture& fix, int depth = 1, int beta = 0, bool is_pv_node = false,
	              bool in_check = false, bool is_exclusion_frame = false)
	{
		return fix.reverse_futility_eligible(depth, beta, is_pv_node, in_check, is_exclusion_frame);
	}
} // namespace

// ============================================================================
// Eligibility
// ============================================================================

TEST_CASE("Reverse futility: shipped configuration never prunes", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);

	// No set_reverse_futility(true): this is what the shipping engine's tuning looks like, and a
	// build that compiled the feature in must still leave the search alone until it is asked.
	CHECK_FALSE(eligible(fix));
}

TEST_CASE("Reverse futility: baseline node is eligible", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(true);

	CHECK(eligible(fix));
}

TEST_CASE("Reverse futility: PV nodes are never pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(true);

	CHECK_FALSE(eligible(fix, 1, 0, /*is_pv_node=*/true));
}

TEST_CASE("Reverse futility: a node in check is never pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(true);

	CHECK_FALSE(eligible(fix, 1, 0, /*is_pv_node=*/false, /*in_check=*/true));
}

TEST_CASE("Reverse futility: an exclusion frame is never pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(true);

	// A verification search's fail-low is what grants a singular extension, so it must come from
	// a search rather than from this margin.
	CHECK_FALSE(eligible(fix, 1, 0, /*is_pv_node=*/false, /*in_check=*/false, /*is_exclusion_frame=*/true));
}

TEST_CASE("Reverse futility: the depth band is a closed interval", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(true);
	fix.set_reverse_futility_max_depth(3);

	CHECK(eligible(fix, /*depth=*/3));
	CHECK_FALSE(eligible(fix, /*depth=*/4));
}

TEST_CASE("Reverse futility: a mate-range beta is never pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(true);

	CHECK_FALSE(eligible(fix, 1, /*beta=*/GameValues::Mate_Threshold));
	CHECK_FALSE(eligible(fix, 1, /*beta=*/-GameValues::Mate_Threshold));
	CHECK(eligible(fix, 1, /*beta=*/GameValues::Mate_Threshold - 1));
}

TEST_CASE("Reverse futility: below the zugzwang floor nothing is pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kZugzwangFen);
	fix.set_reverse_futility(true);

	// The side to move has no non-pawn piece, so "already good enough" is exactly the claim a
	// zugzwang position falsifies.
	CHECK_FALSE(eligible(fix));
}

// ============================================================================
// The cutoff itself
// ============================================================================

TEST_CASE("Reverse futility: a firing cutoff returns beta and searches nothing", "[search][futility]")
{
	AIPerlexTestFixture fix(kWinningFen);
	fix.set_reverse_futility(true);
	fix.arm_clock();

	constexpr int kBeta = 1;
	REQUIRE(fix.static_eval() - 100 >= kBeta); // margin * depth at depth 1

	const int64_t before = fix.mainnodes();
	const int score = fix.search_node(/*depth=*/1, /*ply=*/1, /*alpha=*/0, kBeta, /*is_pv_node=*/false);

	CHECK(score == kBeta);
	// No child was searched: the cutoff is the whole node.
	CHECK(fix.mainnodes() == before);
	// And nothing was stored — the evidence is a static evaluation, not a search, so it must not
	// be able to answer a later, deeper probe.
	CHECK_FALSE(fix.probe_tt(/*ply=*/1).has_value());
}

TEST_CASE("Reverse futility: the same node is searched normally with the flag off", "[search][futility]")
{
	AIPerlexTestFixture fix(kWinningFen);
	fix.set_reverse_futility(false);
	fix.arm_clock();

	const int64_t before = fix.mainnodes();
	fix.search_node(/*depth=*/1, /*ply=*/1, /*alpha=*/0, /*beta=*/1, /*is_pv_node=*/false);

	CHECK(fix.mainnodes() > before);
}

TEST_CASE("Reverse futility: a stalemate node returns beta, not a draw score", "[search][futility]")
{
	AIPerlexTestFixture fix(kStalemateFen);
	fix.set_reverse_futility(true);
	fix.arm_clock();

	// PINS A KNOWN, ACCEPTED HOLE rather than a desired behaviour. The guard runs before move
	// generation, so a node with no legal move is cut before anything can discover it is terminal.
	// Checkmate is unreachable (the in-check guard), the value is never stored, and the node is
	// searched properly once iterative deepening reaches it above the depth band -- so the error is
	// local and short-lived. Closing it would mean generating moves before the cutoff, which is the
	// entire cost the cutoff exists to avoid.
	//
	// Widening reverse_futility_max_depth or lowering the material floor enlarges this hole. This
	// test is here so whoever does that has to read about it first.
	REQUIRE(fix.count_legal_moves() == 0);

	// A window far below the evaluation, so the cutoff fires whatever this construction evaluates to.
	constexpr int kBeta = -999;
	REQUIRE(fix.static_eval() - 100 >= kBeta);

	CHECK(fix.search_node(/*depth=*/1, /*ply=*/1, /*alpha=*/-1000, kBeta, /*is_pv_node=*/false) == kBeta);
}

TEST_CASE("Reverse futility: a static evaluation inside the margin does not cut", "[search][futility]")
{
	AIPerlexTestFixture fix(kWinningFen);
	fix.set_reverse_futility(true);
	fix.arm_clock();

	// Margin raised past the whole advantage, so the guard is eligible but the comparison fails.
	fix.set_reverse_futility_margin(fix.static_eval() + 100);

	const int64_t before = fix.mainnodes();
	fix.search_node(/*depth=*/1, /*ply=*/1, /*alpha=*/0, /*beta=*/1, /*is_pv_node=*/false);

	CHECK(fix.mainnodes() > before);
}
