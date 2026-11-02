// SearchQuietSeePruningTests.cpp — quiet SEE pruning in pvs().
//
// The eligibility tests move one node-level guard at a time off a passing baseline. The node tests
// hold the engine's skip count against the fixture's independent tally, after requiring that the
// position puts a SEE-losing move behind the exemption under test, so removing that guard shows up
// as extra skips. Each fixture narrows the band to one depth, so nested frames add no skips.
//
// The PV and in-check guards are the exception: zugzwang_safe also excludes both nodes, so their node
// cases hold with the guard removed, and only the direct eligibility test falsifies it.
//
// The legal-count term backs up the mated-score term, which also holds before any child has returned:
// the first-move case fails only with both removed.
//
// Three terms have no case here. The hash move sorts first, so the legal-count term already covers it.
// The frontier and late-move exclusions only save a SEE call: those guards skip the same move under
// the same post-move gates. Castling cannot change the count, since a legal castle's destination is
// unattacked; its exclusion is enforced by see_ge's Debug assert, which the castling case trips.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "SearchTestFixture.h"

namespace {
	// No captures. Nb5, Nd5 and Bb5 each lose a minor to a pawn; the other moves are safe.
	constexpr const char* kQuietFen = "6k1/8/2p1p3/8/8/2N5/8/5BK1 w - - 0 1";

	// Nh6+ and Nf6+ each lose the knight to the g-pawn.
	constexpr const char* kLosingChecksFen = "6k1/6p1/2p5/8/6N1/8/8/5BK1 w - - 0 1";

	// One half-move from the fifty-move limit, so every piece move draws on the spot.
	constexpr const char* kFiftyMoveFen = "6k1/8/2p1p3/8/8/2N5/8/5BK1 w - - 99 1";

	// Qxe6 loses the queen for a pawn. Nxb5 wins a pawn and sorts first, which leaves Qxe6 late.
	constexpr const char* kLosingCaptureFen = "6k1/5p2/4p3/1p6/8/2N5/8/4QBK1 w - - 0 1";

	// kQuietFen plus a pawn to win: Nxa4 sorts first, ahead of both killers.
	constexpr const char* kCaptureThenQuietsFen = "6k1/8/2p1p3/8/p7/2N5/8/5BK1 w - - 0 1";

	// b8=Q and the other pushes lose to Rxb8 and give no check; bxa8 sorts first.
	constexpr const char* kLosingPromotionFen = "r7/1P5k/8/8/8/2N5/8/5BK1 w - - 0 1";

	// The side to move has one non-pawn piece, and Nb5 and Nd5 lose it.
	constexpr const char* kOnePieceFen = "6k1/8/2p5/8/8/2N5/8/6K1 w - - 0 1";

	// Both castlings are legal, with Nb5 and Nd5 still losing.
	constexpr const char* kCastlingFen = "6k1/8/2p5/8/8/2N5/8/R3K2R w KQ - 0 1";

	// In check from the e8 rook; Ne4 interposes and loses the knight.
	constexpr const char* kInCheckFen = "4r1k1/8/8/8/8/2N5/8/4KB2 w - - 0 1";

	// Re1 mates unless White makes luft. The knight moves leave that threat standing: Nd7 sorts first
	// and comes back mated, and Nb6, Na6 and Nc6 lose a knight to a pawn before any pawn move runs.
	constexpr const char* kMateThreatFen = "NN2r1k1/pp6/8/8/8/8/5PPP/6K1 w - - 0 1";

	// Every piece move draws, a3 loses a pawn and h4 is safe. White is a knight and a bishop up.
	constexpr const char* kDrawsThenPawnsFen = "k7/8/8/8/pp1N4/7P/P6B/4K3 w - - 99 1";
	// As above without the h-pawn, so a3 is the only move that does not draw.
	constexpr const char* kDrawsThenLoserFen = "k7/8/8/8/pp1N4/8/P6B/4K3 w - - 99 1";
	constexpr std::array<const char*, 8> kDrawingPieceMoves = {"d4b3", "d4b5", "d4c2", "d4c6",
	                                                           "d4e2", "d4e6", "d4f3", "d4f5"};

	// Far above anything these positions reach and outside the mate range: no searched move fails
	// high, so every legal move is visited.
	constexpr int kHighAlpha = 5000;
	constexpr int kMargin = 20;

	using Tally = AIPerlexTestFixture::QuietSeeTally;

	// Quiet SEE pruning alone, in a band of exactly `depth`.
	AIPerlexTestFixture enabled_fixture(const char* fen, int depth, int margin = kMargin)
	{
		AIPerlexTestFixture fix(fen);
		fix.set_quiet_see_pruning(true, depth, depth, margin);
		fix.set_frontier_futility(false);
		fix.set_late_move_pruning(false);
		fix.arm_clock();
		return fix;
	}

	int search_at(const AIPerlexTestFixture& fix, int depth, int alpha = kHighAlpha)
	{
		return fix.search_node(depth, /*ply=*/1, alpha, alpha + 1, /*is_pv_node=*/false);
	}

	Tally tally_at(const AIPerlexTestFixture& fix, int depth, int margin = kMargin)
	{
		return fix.tally_quiet_see(/*ply=*/1, -margin * depth * depth);
	}

	// The baseline call: depth 2, non-PV, not in check, no exclusion, far from mate, enough pieces.
	bool eligible(const AIPerlexTestFixture& fix, int depth = 2, int alpha = 0, bool is_pv_node = false,
	              bool in_check = false, bool is_exclusion_frame = false, bool zugzwang_safe = true)
	{
		return fix.quiet_see_pruning_eligible(depth, alpha, is_pv_node, in_check, is_exclusion_frame, zugzwang_safe);
	}
} // namespace

// ============================================================================
// Eligibility
// ============================================================================

TEST_CASE("Quiet SEE pruning: the runtime flag starts off, and on makes a node eligible", "[search][quietsee]")
{
	AIPerlexTestFixture fix(kQuietFen);
	CHECK_FALSE(eligible(fix));
	fix.set_quiet_see_pruning(true);
	CHECK(eligible(fix));
}

TEST_CASE("Quiet SEE pruning: the depth band is inclusive at both ends", "[search][quietsee]")
{
	AIPerlexTestFixture fix(kQuietFen);
	fix.set_quiet_see_pruning(true, /*min_depth=*/2, /*max_depth=*/3);
	CHECK_FALSE(eligible(fix, /*depth=*/1));
	CHECK(eligible(fix, /*depth=*/2));
	CHECK(eligible(fix, /*depth=*/3));
	CHECK_FALSE(eligible(fix, /*depth=*/4));
}

TEST_CASE("Quiet SEE pruning: each node-level guard makes a node ineligible", "[search][quietsee]")
{
	AIPerlexTestFixture fix(kQuietFen);
	fix.set_quiet_see_pruning(true);
	CHECK_FALSE(eligible(fix, 2, 0, /*is_pv_node=*/true));
	CHECK_FALSE(eligible(fix, 2, 0, false, /*in_check=*/true));
	CHECK_FALSE(eligible(fix, 2, 0, false, false, /*is_exclusion_frame=*/true));
	CHECK_FALSE(eligible(fix, 2, 0, false, false, false, /*zugzwang_safe=*/false));
	CHECK_FALSE(eligible(fix, 2, GameValues::Mate_Threshold));
	CHECK_FALSE(eligible(fix, 2, -GameValues::Mate_Threshold));
	CHECK(eligible(fix, 2, GameValues::Mate_Threshold - 1));
	CHECK(eligible(fix, 2, -GameValues::Mate_Threshold + 1));
}

TEST_CASE("Quiet SEE pruning: an ineligible node skips nothing", "[search][quietsee]")
{
	SECTION("flag off")
	{
		auto fix = enabled_fixture(kQuietFen, 1);
		REQUIRE(tally_at(fix, 1).candidates > 0);
		fix.set_quiet_see_pruning(false);
		search_at(fix, 1);
		CHECK(fix.quiet_see_skips() == 0);
	}
	SECTION("outside the band")
	{
		auto fix = enabled_fixture(kQuietFen, 2);
		REQUIRE(tally_at(fix, 1).candidates > 0);
		search_at(fix, 1);
		CHECK(fix.quiet_see_skips() == 0);
	}
	SECTION("PV node")
	{
		auto fix = enabled_fixture(kQuietFen, 1);
		REQUIRE(tally_at(fix, 1).candidates > 0);
		fix.search_node(/*depth=*/1, /*ply=*/1, kHighAlpha, kHighAlpha + 1, /*is_pv_node=*/true);
		CHECK(fix.quiet_see_skips() == 0);
	}
	SECTION("mate-range alpha")
	{
		auto fix = enabled_fixture(kQuietFen, 1);
		REQUIRE(tally_at(fix, 1).candidates > 0);
		search_at(fix, 1, GameValues::Mate_Threshold);
		CHECK(fix.quiet_see_skips() == 0);
	}
	SECTION("exclusion frame")
	{
		AIPerlexTestFixture fix(kQuietFen);
		fix.set_quiet_see_pruning(true, 2, 2);
		fix.set_late_move_pruning(false);
		fix.set_frontier_futility(false);
		REQUIRE(tally_at(fix, 2).candidates > 0);
		fix.search_node_excluding(/*depth=*/2, /*ply=*/1, "c3a4", kHighAlpha, kHighAlpha + 1);
		CHECK(fix.quiet_see_skips() == 0);
	}
	SECTION("in check")
	{
		auto fix = enabled_fixture(kInCheckFen, 1);
		// Sorts the king move first, which leaves the interposition behind the first-move exemption.
		fix.seed_history("e1d1", 5);
		REQUIRE(tally_at(fix, 1).candidates > 0);
		search_at(fix, 1);
		CHECK(fix.quiet_see_skips() == 0);
	}
	SECTION("too few pieces")
	{
		auto fix = enabled_fixture(kOnePieceFen, 1);
		REQUIRE(tally_at(fix, 1).candidates > 0);
		search_at(fix, 1);
		CHECK(fix.quiet_see_skips() == 0);
	}
}

// ============================================================================
// Which moves are skipped
// ============================================================================

TEST_CASE("Quiet SEE pruning: skips exactly the SEE-losing quiets", "[search][quietsee]")
{
	const int depth = GENERATE(1, 2, 3);
	auto fix = enabled_fixture(kQuietFen, depth);

	const Tally tally = tally_at(fix, depth);
	REQUIRE(tally.candidates >= 2);

	search_at(fix, depth);

	CAPTURE(depth);
	CHECK(fix.quiet_see_skips() == tally.candidates);
	CHECK(fix.search_board_restored());
}

TEST_CASE("Quiet SEE pruning: the threshold is minus margin times depth squared", "[search][quietsee]")
{
	// The losing moves in kQuietFen are all exactly -300.
	struct Case {
		int depth;
		int margin;
		bool skips;
	};
	const auto [depth, margin, skips] = GENERATE(Case{1, 299, true}, Case{1, 300, false}, Case{2, 74, true},
	                                             Case{2, 75, false}, Case{3, 33, true}, Case{3, 34, false});

	auto fix = enabled_fixture(kQuietFen, depth, margin);
	search_at(fix, depth);

	CAPTURE(depth, margin);
	CHECK((fix.quiet_see_skips() > 0) == skips);
}

TEST_CASE("Quiet SEE pruning: the first legal move is always searched", "[search][quietsee]")
{
	auto fix = enabled_fixture(kQuietFen, 1);
	fix.seed_history("c3d5", 5);

	const Tally tally = tally_at(fix, 1);
	REQUIRE(tally.losing_first == 1);

	search_at(fix, 1);

	CHECK(fix.quiet_see_skips() == tally.candidates);
}

TEST_CASE("Quiet SEE pruning: losing captures and promotions are searched", "[search][quietsee]")
{
	SECTION("capture")
	{
		auto fix = enabled_fixture(kLosingCaptureFen, 1);
		const Tally tally = tally_at(fix, 1);
		REQUIRE(tally.losing_captures > 0);
		search_at(fix, 1);
		CHECK(fix.quiet_see_skips() == tally.candidates);
	}
	SECTION("promotion")
	{
		auto fix = enabled_fixture(kLosingPromotionFen, 1);
		const Tally tally = tally_at(fix, 1);
		REQUIRE(tally.losing_promotions > 0);
		search_at(fix, 1);
		CHECK(fix.quiet_see_skips() == tally.candidates);
	}
}

TEST_CASE("Quiet SEE pruning: both killer slots are searched", "[search][quietsee]")
{
	auto fix = enabled_fixture(kCaptureThenQuietsFen, 1);
	fix.store_killer_uci(/*ply=*/1, "c3b5");
	fix.store_killer_uci(/*ply=*/1, "c3d5");

	const Tally tally = tally_at(fix, 1);
	REQUIRE(tally.losing_killers == 2);

	search_at(fix, 1);

	CHECK(fix.quiet_see_skips() == tally.candidates);
}

TEST_CASE("Quiet SEE pruning: a checking move is searched", "[search][quietsee]")
{
	auto fix = enabled_fixture(kLosingChecksFen, 1);
	const Tally tally = tally_at(fix, 1);
	REQUIRE(tally.losing_checks > 0);

	search_at(fix, 1);

	CHECK(fix.quiet_see_skips() == tally.candidates);
}

TEST_CASE("Quiet SEE pruning: a move that draws on the spot is searched", "[search][quietsee]")
{
	auto fix = enabled_fixture(kFiftyMoveFen, 1);
	const Tally tally = tally_at(fix, 1);
	REQUIRE(tally.losing_draws > 0);
	REQUIRE(tally.candidates == 0);

	search_at(fix, 1);

	CHECK(fix.quiet_see_skips() == 0);
}

TEST_CASE("Quiet SEE pruning: nothing is skipped once a searched move came back mated", "[search][quietsee]")
{
	// A low alpha, so the child must find the mate to fail high. Reverse futility and null move would
	// otherwise return from a node standing this far above beta.
	AIPerlexTestFixture fix(kMateThreatFen);
	fix.set_quiet_see_pruning(true, 2, 2);
	fix.set_frontier_futility(false);
	fix.set_late_move_pruning(false);
	fix.set_reverse_futility(false);
	fix.set_null_move_enabled(false);
	fix.arm_clock();
	fix.seed_history("b8d7", 6);
	fix.seed_history("a8b6", 4);
	fix.seed_history("b8a6", 4);
	fix.seed_history("b8c6", 4);

	REQUIRE(tally_at(fix, 2).candidates >= 3);

	search_at(fix, 2, /*alpha=*/-kHighAlpha);

	CHECK(fix.quiet_see_skips() == 0);
}

TEST_CASE("Quiet SEE pruning: a position with castling reaches no see_ge on the castle", "[search][quietsee]")
{
	auto fix = enabled_fixture(kCastlingFen, 1);
	const Tally tally = tally_at(fix, 1);
	REQUIRE(tally.candidates > 0);

	search_at(fix, 1);

	CHECK(fix.quiet_see_skips() == tally.candidates);
}

// ============================================================================
// What the node returns and stores
// ============================================================================

TEST_CASE("Quiet SEE pruning: a skipped fail-low returns alpha and stores UPPER at alpha", "[search][quietsee][tt]")
{
	// Every searched move draws, below alpha, so only the floor lifts the node to alpha.
	auto fix = enabled_fixture(kDrawsThenLoserFen, 1);
	for (const char* uci : kDrawingPieceMoves)
		fix.seed_history(uci, 1);

	const int score = search_at(fix, 1);

	REQUIRE(fix.quiet_see_skips() == 1);
	CHECK(score == kHighAlpha);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->value == kHighAlpha);
	CHECK(entry->depth == 1);
	CHECK(entry->bound == BoundType::UPPER);
	CHECK_FALSE(entry->best_move.is_null());
}

TEST_CASE("Quiet SEE pruning: a searched cutoff after a skip stores LOWER", "[search][quietsee][tt]")
{
	// The piece moves draw below alpha, a3 is skipped, and h4 fails high: White is two pieces up.
	constexpr int kAlpha = 100;
	auto fix = enabled_fixture(kDrawsThenPawnsFen, 1);
	fix.set_reverse_futility(false);
	fix.set_null_move_enabled(false);
	for (const char* uci : kDrawingPieceMoves)
		fix.seed_history(uci, 3);
	fix.seed_history("a2a3", 1);

	const int score = search_at(fix, 1, kAlpha);

	REQUIRE(fix.quiet_see_skips() == 1);
	CHECK(score > kAlpha);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->bound == BoundType::LOWER);
}

TEST_CASE("Quiet SEE pruning: a node late move pruning also skipped stores nothing", "[search][quietsee][tt]")
{
	constexpr int16_t kPlantedValue = 1234;

	auto fix = enabled_fixture(kQuietFen, 2);
	fix.set_late_move_pruning(true);
	fix.set_late_move_pruning_min_index(4);
	fix.seed_history("c3e4", 6);
	fix.seed_history("c3d5", 4);
	// Shallower than the node, so it supplies no cutoff.
	fix.store_main_entry(kPlantedValue, /*depth=*/1, /*ply=*/1, BoundType::EXACT);

	const int score = search_at(fix, 2);

	REQUIRE(fix.quiet_see_skips() > 0);
	REQUIRE(fix.lmp_skips() > 0);
	CHECK(score == kHighAlpha);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->value == kPlantedValue);
	CHECK(entry->depth == 1);
	CHECK(entry->bound == BoundType::EXACT);
}

// ============================================================================
// Whole searches
// ============================================================================

TEST_CASE("Quiet SEE pruning: an aborted search unwinds to the root board, deterministically",
          "[search][quietsee][nodes]")
{
	const std::string fen = "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2";
	constexpr int64_t node_budget = 20000;

	AIPerlexTestFixture first(fen, /*max_depth=*/8);
	first.set_quiet_see_pruning(true);
	const Move first_move = first.search_with_nodes(node_budget);

	REQUIRE_FALSE(first_move.is_null());
	REQUIRE(first.search_is_aborted());
	REQUIRE(first.quiet_see_skips() > 0);
	CHECK(first.search_board_restored());

	AIPerlexTestFixture second(fen, /*max_depth=*/8);
	second.set_quiet_see_pruning(true);
	const Move second_move = second.search_with_nodes(node_budget);

	CHECK(MoveFormatter::ToUCI(first_move) == MoveFormatter::ToUCI(second_move));
	CHECK(first.quiet_see_skips() == second.quiet_see_skips());
	CHECK(first.mainnodes() + first.qnodes() == second.mainnodes() + second.qnodes());
}

TEST_CASE("Quiet SEE pruning: the search result reports the skip count", "[search][quietsee]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
	fix.set_quiet_see_pruning(true);

	const SearchResult result = fix.result_to_depth(5);

	REQUIRE(fix.quiet_see_skips() > 0);
	CHECK(result.telemetry.quiet_see.skips == fix.quiet_see_skips());
}
