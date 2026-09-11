// SearchFrontierFutilityTests.cpp — frontier futility pruning (#504).
//
// The eligibility tests move one node-level guard at a time off a passing baseline. The node tests
// hold the engine's skip count against an independent tally, so removing a move-level guard shows
// up as extra skips. The hash-move term is the exception: it is redundant behind the
// first-legal-move term while the hash move sorts first, so no case claims it.
//
// The feature ships enabled. Cases still set the flag explicitly rather than leaning on the
// default, so each one names the configuration it is asserting about.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// Quiet middlegame position, for the eligibility guards that never look at the board.
	constexpr const char* kBaselineFen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	// Two captures, four promotions (two of them checking), two quiet checks and 21 plain quiet
	// moves, so every move-level guard has a move of its own to protect. Move list verified
	// against python-chess.
	constexpr const char* kMixedFen = "7k/1P6/8/3p4/8/2N5/8/3QK3 w - - 0 1";

	// A rook up with nothing to capture or promote, and one quiet check (Ra8+). No move wins
	// material, so a window a little above the static evaluation fails low.
	constexpr const char* kRookUpFen = "7k/8/8/8/8/8/8/R3K3 w - - 0 1";

	// A rook and a pawn up, one half-move from the fifty-move limit. Every king and rook move
	// draws on the spot, so it is searched and returns a draw, below alpha. The two pawn pushes
	// reset the clock, so they are the only quiet moves the guard may skip.
	constexpr const char* kFiftyMoveEdgeFen = "7k/8/8/8/8/8/P7/R3K3 w - - 99 1";

	// White a queen and a pawn down, one half-move from the fifty-move limit, with no pawn and no
	// capture: every legal move draws on the spot. Skipping them would turn a draw into a loss.
	constexpr const char* kDrawingMovesFen = "7k/p6q/8/8/8/8/8/R3K3 w - - 99 1";

	// White's only legal move is Kb1, quiet and not a check, and White is a queen down.
	constexpr const char* kOneMoveFen = "q6k/8/8/8/8/p7/P7/K7 w - - 0 1";

	// White in check from an unprotected rook: evasions Kxe2 plus two quiet king moves, Kd1 and
	// Kf1, neither giving check. Verified against python-chess.
	constexpr const char* kInCheckFen = "4k3/8/8/8/8/8/4r3/4K3 w - - 0 1";

	// Far above anything these positions reach and outside the mate range: no searched move
	// fails high, and every candidate clears the margin.
	constexpr int kHighAlpha = 5000;

	// The baseline call: depth 1, non-PV, not in check, no exclusion, alpha nowhere near a mate
	// score. Every eligibility case changes exactly one of these.
	bool eligible(const AIPerlexTestFixture& fix, int depth = 1, int alpha = 0, bool is_pv_node = false,
	              bool in_check = false, bool is_exclusion_frame = false)
	{
		return fix.frontier_futility_eligible(depth, alpha, is_pv_node, in_check, is_exclusion_frame);
	}

	// One depth-1 null-window node at ply 1, the only shape the guard applies to.
	int search_depth1(const AIPerlexTestFixture& fix, int alpha)
	{
		return fix.search_node(/*depth=*/1, /*ply=*/1, alpha, alpha + 1, /*is_pv_node=*/false);
	}
} // namespace

// ============================================================================
// Eligibility
// ============================================================================

TEST_CASE("Frontier futility: shipped configuration prunes", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);

	// No set_frontier_futility call: this is what the shipping engine's tuning looks like.
	CHECK(eligible(fix));
}

TEST_CASE("Frontier futility: the runtime flag off makes a node ineligible", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_frontier_futility(false);

	CHECK_FALSE(eligible(fix));
}

TEST_CASE("Frontier futility: baseline node is eligible", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_frontier_futility(true);

	CHECK(eligible(fix));
}

TEST_CASE("Frontier futility: PV nodes are never pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_frontier_futility(true);

	CHECK_FALSE(eligible(fix, 1, 0, /*is_pv_node=*/true));
}

TEST_CASE("Frontier futility: a node in check is never pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_frontier_futility(true);

	CHECK_FALSE(eligible(fix, 1, 0, /*is_pv_node=*/false, /*in_check=*/true));
}

TEST_CASE("Frontier futility: an exclusion frame is never pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_frontier_futility(true);

	CHECK_FALSE(eligible(fix, 1, 0, /*is_pv_node=*/false, /*in_check=*/false, /*is_exclusion_frame=*/true));
}

TEST_CASE("Frontier futility: only depth 1 is pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_frontier_futility(true);

	CHECK(eligible(fix, /*depth=*/1));
	CHECK_FALSE(eligible(fix, /*depth=*/2));
}

TEST_CASE("Frontier futility: a mate-range alpha is never pruned", "[search][futility]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_frontier_futility(true);

	CHECK_FALSE(eligible(fix, 1, /*alpha=*/GameValues::Mate_Threshold));
	CHECK_FALSE(eligible(fix, 1, /*alpha=*/-GameValues::Mate_Threshold));
	CHECK(eligible(fix, 1, /*alpha=*/GameValues::Mate_Threshold - 1));
}

// ============================================================================
// The node-level guards as pvs() wires them
// ============================================================================
// The cases above call frontier_futility_eligible() directly, so they cannot see pvs() passing
// it the wrong argument. Each case below drives a whole node that would skip moves if its one
// excluding property were lost on the way in.

TEST_CASE("Frontier futility: a node in check skips nothing", "[search][futility]")
{
	AIPerlexTestFixture fix(kInCheckFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();

	search_depth1(fix, kHighAlpha);

	CHECK(fix.frontier_skips() == 0);
}

TEST_CASE("Frontier futility: a PV node skips nothing", "[search][futility]")
{
	AIPerlexTestFixture fix(kMixedFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();

	fix.search_node(/*depth=*/1, /*ply=*/1, kHighAlpha, kHighAlpha + 100, /*is_pv_node=*/true);

	CHECK(fix.frontier_skips() == 0);
}

TEST_CASE("Frontier futility: a depth-2 node skips nothing", "[search][futility]")
{
	AIPerlexTestFixture fix(kMixedFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();

	// The children are depth-1 nodes too, but their alpha is -kHighAlpha - 1, far below any
	// evaluation plus the margin, so none of them skips either.
	fix.search_node(/*depth=*/2, /*ply=*/1, kHighAlpha, kHighAlpha + 1, /*is_pv_node=*/false);

	CHECK(fix.frontier_skips() == 0);
}

TEST_CASE("Frontier futility: an exclusion frame skips nothing", "[search][futility]")
{
	AIPerlexTestFixture fix(kMixedFen);
	fix.set_frontier_futility(true);

	fix.search_node_excluding(/*depth=*/1, /*ply=*/1, "c3d5", kHighAlpha, kHighAlpha + 1);

	CHECK(fix.frontier_skips() == 0);
}

TEST_CASE("Frontier futility: a mate-range alpha skips nothing", "[search][futility]")
{
	AIPerlexTestFixture fix(kMixedFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();

	search_depth1(fix, GameValues::Mate_Threshold);

	CHECK(fix.frontier_skips() == 0);
}

// ============================================================================
// Which moves are skipped
// ============================================================================

TEST_CASE("Frontier futility: skips exactly the quiet, non-checking later moves", "[search][futility]")
{
	AIPerlexTestFixture fix(kMixedFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();

	// A capture or promotion sorts first, so the tally is the 21 plain quiet moves. Skipping a
	// capture, a promotion or a quiet check would push the count above it.
	const int expected = fix.count_frontier_candidates(/*ply=*/1);
	REQUIRE(expected == 21);

	search_depth1(fix, kHighAlpha);

	CHECK(fix.frontier_skips() == expected);
}

TEST_CASE("Frontier futility: neither live killer is skipped", "[search][futility]")
{
	AIPerlexTestFixture fix(kMixedFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();

	// Two quiet, non-checking moves, one in each killer slot.
	fix.store_killer_uci(/*ply=*/1, "e1e2");
	fix.store_killer_uci(/*ply=*/1, "c3b5");

	const int expected = fix.count_frontier_candidates(/*ply=*/1);
	REQUIRE(expected == 19);

	search_depth1(fix, kHighAlpha);

	CHECK(fix.frontier_skips() == expected);
}

TEST_CASE("Frontier futility: the flag off skips nothing", "[search][futility]")
{
	AIPerlexTestFixture fix(kMixedFen);
	fix.set_frontier_futility(false);
	fix.arm_clock();

	search_depth1(fix, kHighAlpha);

	CHECK(fix.frontier_skips() == 0);
}

TEST_CASE("Frontier futility: the margin is measured from the parent's evaluation", "[search][futility]")
{
	// Just inside the margin nothing is skipped; at it, every candidate is. The child's evaluation
	// is from the other side's point of view, a rook the wrong way, so a guard that read it
	// after DoMove() would skip at the first window too.
	AIPerlexTestFixture inside(kRookUpFen);
	inside.set_frontier_futility(true);
	inside.arm_clock();
	const int threshold = inside.static_eval() + inside.frontier_futility_margin();

	search_depth1(inside, threshold - 1);
	CHECK(inside.frontier_skips() == 0);

	AIPerlexTestFixture at(kRookUpFen);
	at.set_frontier_futility(true);
	at.arm_clock();
	const int expected = at.count_frontier_candidates(/*ply=*/1);
	REQUIRE(expected > 0);

	search_depth1(at, threshold);
	CHECK(at.frontier_skips() == expected);
}

TEST_CASE("Frontier futility: the first legal move is always searched", "[search][futility]")
{
	AIPerlexTestFixture fix(kOneMoveFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();
	REQUIRE(fix.count_legal_moves() == 1);

	// Skipping the only move would leave the node without one and report a stalemate draw.
	const int score = search_depth1(fix, kHighAlpha);

	CHECK(fix.frontier_skips() == 0);
	CHECK(score != GameValues::Draw);
}

TEST_CASE("Frontier futility: a move that draws on the spot is never skipped", "[search][futility]")
{
	// Down a queen, the margin calls every quiet move hopeless; but each one completes the
	// fifty-move rule, and a draw is far above this window. Skipping them all would fail low at
	// alpha where the node really holds a draw.
	AIPerlexTestFixture fix(kDrawingMovesFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();
	constexpr int kAlpha = -100;
	REQUIRE(fix.static_eval() + fix.frontier_futility_margin() <= kAlpha);

	const int score = search_depth1(fix, kAlpha);

	CHECK(fix.frontier_skips() == 0);
	CHECK(score == GameValues::Draw);
}

TEST_CASE("Frontier futility: the board is restored after every skip", "[search][futility]")
{
	AIPerlexTestFixture fix(kMixedFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();

	search_depth1(fix, kHighAlpha);

	REQUIRE(fix.frontier_skips() > 0);
	CHECK(fix.search_board_restored());
}

// ============================================================================
// What a node that skipped a move may claim
// ============================================================================

TEST_CASE("Frontier futility: a fail-low is floored at eval + margin and stored as UPPER", "[search][futility]")
{
	// Quiescence fails high at exactly its beta, so a fail-low child normally hands this node
	// exactly alpha and the floor, which is <= alpha, changes nothing. It binds when a searched
	// child returns below alpha, as a draw does. Here every searched move draws and the two pawn
	// pushes are skipped, so without the floor the node would report and store the draw score, a
	// claim the skipped pushes were never searched to support.
	AIPerlexTestFixture on(kFiftyMoveEdgeFen);
	on.set_frontier_futility(true);
	on.arm_clock();
	const int floor = on.static_eval() + on.frontier_futility_margin();
	const int alpha = floor + 100;
	REQUIRE(floor > GameValues::Draw);
	// A killer sorts ahead of both pushes, so the first legal move searched is a drawing one.
	on.store_killer_uci(/*ply=*/1, "e1e2");

	const int score = search_depth1(on, alpha);

	// Both pushes skipped means neither was the first legal move, so every searched child drew.
	REQUIRE(on.frontier_skips() == 2);
	CHECK(score == floor);
	const auto entry = on.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->bound == BoundType::UPPER);
	CHECK(entry->value == floor);

	// Nothing failed high, so no move-ordering write. This pins the property; the skip cannot
	// reach those writes in any window, because it continues before them.
	CHECK_FALSE(on.has_killer(2));
	CHECK(on.history_is_clear());
}

TEST_CASE("Frontier futility: the floor only ever raises the value", "[search][futility]")
{
	// The ordinary case: the searched children fail high in quiescence at exactly their beta, so
	// this node's value is already alpha, above the floor. The floor must leave it there.
	AIPerlexTestFixture fix(kRookUpFen);
	fix.set_frontier_futility(true);
	fix.arm_clock();
	const int floor = fix.static_eval() + fix.frontier_futility_margin();
	const int alpha = floor + 100;

	const int score = search_depth1(fix, alpha);

	REQUIRE(fix.frontier_skips() > 0);
	CHECK(score == alpha);
}
