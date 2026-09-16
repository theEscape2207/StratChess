// SearchFutilityTests.cpp — reverse futility pruning (#87).
//
// Two groups. The eligibility tests move one guard at a time off a baseline that is known to
// pass, so a failure names the guard that broke; removing any single guard from
// reverse_futility_eligible() makes exactly one of them fail. The cutoff tests then drive a
// whole pvs() node and check what the guard is allowed to do once it fires: return beta, search
// no children, and store nothing. The refinement tests plant a TT entry the node's probe finds
// but cannot cut on, and check which entries may raise the value the guard compares.
//
// The feature ships enabled. Cases still set the flag explicitly rather than leaning on the
// default, so each one names the configuration it is asserting about.

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

TEST_CASE("Reverse futility: shipped configuration prunes", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);

	// No set_reverse_futility call: this is what the shipping engine's tuning looks like.
	CHECK(eligible(fix));
}

TEST_CASE("Reverse futility: the runtime flag off makes a node ineligible", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(false);

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

TEST_CASE("Reverse futility: the band abuts null move's floor", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(true);
	fix.set_reverse_futility_max_depth(3);
	fix.set_null_move_min_depth(3);

	// The two guards share a zugzwang floor that pvs() establishes once, ahead of both. That
	// costs nothing extra only while the bands abut: the first depth above the futility band is
	// already inside null move's, so no depth is left where the floor is computed for a node
	// neither guard can use. Moving either knob opens such a gap.
	CHECK(eligible(fix, /*depth=*/3));
	CHECK_FALSE(eligible(fix, /*depth=*/4));
	CHECK(fix.try_null_move(/*depth=*/4, /*beta=*/1, /*ply=*/1, /*is_pv_node=*/false, /*in_check=*/false));
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
	// Checkmate is unreachable (the in-check guard), and the node is searched properly once
	// iterative deepening reaches it above the depth band. The error is not confined to this node:
	// beta reaches the parent as exactly its alpha, so a parent no sibling improves can store an
	// UPPER bound that is false when the truth is a draw. That is what a wrong fail-high from any
	// pruning heuristic does here; closing it would mean generating moves before the cutoff, which
	// is the entire cost the cutoff exists to avoid.
	//
	// Widening reverse_futility_max_depth or lowering the material floor enlarges this hole. This
	// test is here so whoever does that has to read about it first.
	REQUIRE(fix.count_legal_moves() == 0);

	// A window far below the evaluation, so the cutoff fires whatever this construction evaluates to.
	constexpr int kBeta = -999;
	REQUIRE(fix.static_eval() - 100 >= kBeta);

	CHECK(fix.search_node(/*depth=*/1, /*ply=*/1, /*alpha=*/-1000, kBeta, /*is_pv_node=*/false) == kBeta);
}

// ============================================================================
// What the fail-hard return buys the rest of the search
// ============================================================================
// Both cases below pin consequences of returning beta rather than the static evaluation. They
// are here so a later fail-soft conversion -- returning eval - margin * depth, which is what
// several engines do -- fails loudly instead of quietly changing what the surrounding search
// may conclude.

TEST_CASE("Reverse futility: a cut null-move child cannot fail high", "[search][futility]")
{
	AIPerlexTestFixture fix(kWinningFen);
	fix.set_reverse_futility(true);
	fix.arm_clock();

	// The exact window null-move pruning opens: -pvs(td, ..., -beta, -beta + 1) for a parent at
	// alpha 0, beta 1. The child's own beta is therefore 0. The position is this fixture's rather
	// than a real null-move child's, which changes nothing the guard reads.
	constexpr int kParentBeta = 1;
	constexpr int kChildAlpha = -kParentBeta;
	constexpr int kChildBeta = -kParentBeta + 1;
	REQUIRE(fix.static_eval() - 100 >= kChildBeta); // the cutoff does fire here

	const int child = fix.search_node(/*depth=*/1, /*ply=*/2, kChildAlpha, kChildBeta, /*is_pv_node=*/false);
	const int null_score = -child;

	// Fail-hard makes this exactly beta - 1, one below the cutoff that would otherwise reach
	// tt.store(... LOWER, CUT_NODE). A fail-soft return hands back the evaluation instead, and
	// this position's is a queen and a bishop clear of the window.
	CHECK(child == kChildBeta);
	CHECK(null_score == kParentBeta - 1);
	CHECK(null_score < kParentBeta);
}

TEST_CASE("Reverse futility: cut children leave no killer or history behind", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_reverse_futility(true);
	// Late move pruning would skip late moves at this depth-2 node and suppress the store asserted below.
	fix.set_late_move_pruning(false);
	fix.arm_clock();

	REQUIRE_FALSE(fix.has_killer(1));
	REQUIRE(fix.history_is_clear());

	// A window far above anything this quiet position can reach, so nothing the move loop
	// searches can fail high either. Every child is a depth-1 node whose evaluation clears
	// -kAlpha by more than the margin, so every child is cut -- and no move here gives check,
	// which is the one thing that would make a child search normally instead.
	constexpr int kAlpha = 5000;
	const int score = fix.search_node(/*depth=*/2, /*ply=*/1, kAlpha, kAlpha + 1, /*is_pv_node=*/false);

	// beta arrives from each cut child as exactly this node's alpha. This is the assertion that
	// discriminates: a fail-soft child would hand back its own evaluation, which negates to a few
	// hundred centipawns at this node rather than kAlpha.
	CHECK(score == kAlpha);

	// No improvement, so no cutoff, and the two move-ordering tables a cutoff would have written
	// stay untouched. Both hold under either return convention at this window -- they pin the
	// property rather than falsify a change to it.
	CHECK_FALSE(fix.has_killer(1));
	CHECK_FALSE(fix.has_killer(2));
	CHECK(fix.history_is_clear());

	// And the node stores what it actually established: every move failed low, so an upper bound,
	// never the lower bound a cutoff would have written.
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->bound == BoundType::UPPER);
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

// ============================================================================
// Refinement by a transposition-table value
// ============================================================================
// Every refining entry is shallower than the node. An entry at least as deep with a value that
// clears beta is taken by the TT cutoff before reverse futility runs, so it cannot test this.

namespace {
	struct RefineOutcome {
		int beta;
		int score;
		bool searched;
		std::optional<TTEntry> entry;
	};

	struct PlantedEntry {
		int depth;
		BoundType bound;
		int value_over_beta;
		SearchPhase phase = SearchPhase::MAIN;
	};

	// One non-PV node of kBaselineFen with a planted entry, at a beta just above its static
	// evaluation so the unrefined guard never cuts.
	RefineOutcome search_with_planted_entry(bool refine, int node_depth, const PlantedEntry& planted)
	{
		AIPerlexTestFixture fix(kBaselineFen);
		fix.set_reverse_futility(true);
		fix.set_reverse_futility_tt_refine(refine);
		fix.arm_clock();

		const int beta = fix.static_eval() + 1;
		const auto value = static_cast<int16_t>(beta + planted.value_over_beta);
		if (planted.phase == SearchPhase::MAIN)
			fix.store_main_entry(value, static_cast<int16_t>(planted.depth), /*ply=*/1, planted.bound);
		else
			fix.store_qsearch_entry(value, static_cast<int16_t>(planted.depth), /*ply=*/1);

		const int64_t before = fix.mainnodes();
		const int score = fix.search_node(node_depth, /*ply=*/1, beta - 1, beta, /*is_pv_node=*/false);
		return {beta, score, fix.mainnodes() > before, fix.probe_tt(/*ply=*/1)};
	}
} // namespace

TEST_CASE("Reverse futility: a shallower LOWER or EXACT value clearing the margin cuts", "[search][futility]")
{
	for (const BoundType bound : {BoundType::LOWER, BoundType::EXACT}) {
		for (const auto& [node_depth, entry_depth] : {std::pair{2, 1}, std::pair{3, 1}, std::pair{3, 2}}) {
			CAPTURE(static_cast<int>(bound), node_depth, entry_depth);
			const int margin = 100 * node_depth;

			const auto on = search_with_planted_entry(true, node_depth, {entry_depth, bound, margin});
			CHECK_FALSE(on.searched);
			// Fail-hard, and the planted entry is untouched: the cut stores nothing.
			CHECK(on.score == on.beta);
			REQUIRE(on.entry.has_value());
			CHECK(on.entry->value == on.beta + margin);
			CHECK(on.entry->depth == entry_depth);
			CHECK(on.entry->bound == bound);

			CHECK(search_with_planted_entry(false, node_depth, {entry_depth, bound, margin}).searched);
			CHECK(search_with_planted_entry(true, node_depth, {entry_depth, bound, margin - 1}).searched);
		}
	}
}

TEST_CASE("Reverse futility: UPPER, mate-range and quiescence entries never refine", "[search][futility]")
{
	CHECK(search_with_planted_entry(true, 2, {1, BoundType::UPPER, 500}).searched);
	CHECK(search_with_planted_entry(true, 2, {2, BoundType::EXACT, 500, SearchPhase::QUIESCENCE}).searched);

	// A mate score clears any margin, so only the mate-range guard keeps it out.
	const int mate_over_beta = GameValues::Mate_Threshold - AIPerlexTestFixture(kBaselineFen).static_eval();
	CHECK(search_with_planted_entry(true, 2, {1, BoundType::LOWER, mate_over_beta}).searched);
}

TEST_CASE("Reverse futility: a TT value at or below static evaluation does not stop a cut", "[search][futility]")
{
	// At equality, and low enough that comparing the TT value alone would not cut.
	for (const bool equal : {true, false}) {
		CAPTURE(equal);
		AIPerlexTestFixture fix(kWinningFen);
		fix.set_reverse_futility(true);
		fix.set_reverse_futility_tt_refine(true);
		fix.arm_clock();

		constexpr int kBeta = 1;
		const int eval = fix.static_eval();
		REQUIRE(eval - 200 >= kBeta);
		fix.store_main_entry(static_cast<int16_t>(equal ? eval : 100), /*depth=*/1, /*ply=*/1, BoundType::LOWER);

		const int64_t before = fix.mainnodes();
		CHECK(fix.search_node(/*depth=*/2, /*ply=*/1, kBeta - 1, kBeta, /*is_pv_node=*/false) == kBeta);
		CHECK(fix.mainnodes() == before);
	}
}

TEST_CASE("Reverse futility: a TT value never reaches frontier futility's fail-low floor", "[search][futility]")
{
	// A rook and a bishop up with nothing to capture, so a window above the static evaluation fails
	// low and frontier futility skips quiet moves. Two non-pawn pieces, so reverse futility is eligible. The planted value lies between static evaluation and
	// beta: it cannot cut, and if it leaked into static_eval the floor would clear alpha.
	constexpr const char* kRookBishopUpFen = "7k/8/8/8/8/8/8/R3KB2 w - - 0 1";

	const auto run = [](bool refine) {
		AIPerlexTestFixture fix(kRookBishopUpFen);
		fix.set_reverse_futility(true);
		fix.set_reverse_futility_tt_refine(refine);
		fix.set_frontier_futility(true);
		fix.arm_clock();

		const int alpha = fix.static_eval() + 250;
		fix.store_main_entry(static_cast<int16_t>(alpha), /*depth=*/1, /*ply=*/1, BoundType::LOWER);
		const int score = fix.search_node(/*depth=*/1, /*ply=*/1, alpha, alpha + 1, /*is_pv_node=*/false);
		REQUIRE(fix.frontier_skips() > 0);
		const auto entry = fix.probe_tt(/*ply=*/1);
		REQUIRE(entry.has_value());
		return std::tuple{score - alpha, entry->value - alpha, entry->bound};
	};

	const auto on = run(true);
	CHECK(on == run(false));
	CHECK(std::get<0>(on) <= 0);
	CHECK(std::get<2>(on) == BoundType::UPPER);
}
