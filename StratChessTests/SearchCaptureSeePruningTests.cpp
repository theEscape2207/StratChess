// SearchCaptureSeePruningTests.cpp — capture SEE pruning in pvs() (#944).
//
// As for quiet SEE pruning: the eligibility tests move one node-level guard at a time off a passing
// baseline, and the node tests hold the engine's skip count against the fixture's independent tally,
// after requiring that the position puts a SEE-losing capture behind the exemption under test.
//
// The PV and in-check guards: zugzwang_safe also excludes both nodes, so only the direct eligibility
// test falsifies them.
//
// Node tests search at depth 1 or 2: the band starts at depth 1, so a deeper node would add its nested
// White frames' skips, and Black, with no piece in these positions, never prunes. The first-legal-move, hash-move and capture-promotion exemptions are defence in
// depth behind the mated-score term, hash ordering and see_ge(0), and have no isolating case.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "SearchTestFixture.h"

namespace {
	// Qxe6 loses the queen for a pawn (f7 recaptures); Nxb5 and Bxb5 win a pawn and sort first.
	constexpr const char* kLosingCaptureFen = "6k1/5p2/4p3/1p6/8/2N5/8/4QBK1 w - - 0 1";

	// As above with a rook: Rxe6 fxe6 is SEE -400, the only losing capture.
	constexpr const char* kRookLosesFen = "6k1/5p2/4p3/1p6/8/2N5/8/4RBK1 w - - 0 1";

	// Qxf7+ loses the queen to the king, with check; Qxb5 and Nxb5 win a pawn and sort first.
	constexpr const char* kLosingCheckFen = "6k1/5pp1/8/1p6/2Q5/2N5/8/5BK1 w - - 0 1";

	// Black has three men, below MATERIAL_PRUNING_MIN_PIECES; White still has two non-pawn pieces.
	// Rxe6 is the only capture, so a killer must sort ahead of it.
	constexpr const char* kSparseFen = "6k1/5p2/4p3/8/8/2N5/8/4RBK1 w - - 0 1";

	// One half-move from the fifty-move limit: the knight's quiet moves draw, Rxe6 (SEE -400) and the pawn
	// push reset the clock. No capture wins material, so the killers sort first.
	constexpr const char* kDrawsThenCaptureFen = "6k1/p4p2/4p3/8/8/2N4P/8/4RBK1 w - - 99 1";
	// As above without the h-pawn, so every move but Rxe6 draws.
	constexpr const char* kDrawsOnlyFen = "6k1/p4p2/4p3/8/8/2N5/8/4RBK1 w - - 99 1";

	constexpr int kHighAlpha = 5000;
	constexpr int kMargin = 100;

	using Tally = AIPerlexTestFixture::CaptureSeeTally;

	// Capture SEE pruning alone, at depths up to `max_depth`.
	AIPerlexTestFixture enabled_fixture(const char* fen, int max_depth = 4, int margin = kMargin)
	{
		AIPerlexTestFixture fix(fen);
		fix.set_capture_see_pruning(true, max_depth, margin);
		fix.set_quiet_see_pruning(false);
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
		return fix.tally_capture_see(/*ply=*/1, -margin * depth);
	}

	// The baseline call: depth 2, non-PV, not in check, no exclusion, far from mate, enough pieces.
	bool eligible(const AIPerlexTestFixture& fix, int depth = 2, int alpha = 0, bool is_pv_node = false,
	              bool in_check = false, bool is_exclusion_frame = false, bool zugzwang_safe = true)
	{
		return fix.capture_see_pruning_eligible(depth, alpha, is_pv_node, in_check, is_exclusion_frame, zugzwang_safe);
	}

	Move uci_move(const char* fen, const char* uci)
	{
		const Board board(fen);
		const Move move = MoveFormatter::FromUCI(uci, board);
		REQUIRE_FALSE(move.is_null());
		return move;
	}
} // namespace

// ============================================================================
// Eligibility
// ============================================================================

TEST_CASE("Capture SEE pruning: the runtime flag starts off, and on makes a node eligible", "[search][capturesee]")
{
	AIPerlexTestFixture fix(kLosingCaptureFen);
	CHECK_FALSE(eligible(fix));
	fix.set_capture_see_pruning(true);
	CHECK(eligible(fix));
}

TEST_CASE("Capture SEE pruning: the depth band is 1 to the inclusive maximum", "[search][capturesee]")
{
	AIPerlexTestFixture fix(kLosingCaptureFen);
	fix.set_capture_see_pruning(true, /*max_depth=*/3);
	CHECK_FALSE(eligible(fix, /*depth=*/0));
	CHECK(eligible(fix, /*depth=*/1));
	CHECK(eligible(fix, /*depth=*/3));
	CHECK_FALSE(eligible(fix, /*depth=*/4));
}

TEST_CASE("Capture SEE pruning: each node-level guard makes a node ineligible", "[search][capturesee]")
{
	AIPerlexTestFixture fix(kLosingCaptureFen);
	fix.set_capture_see_pruning(true);
	CHECK_FALSE(eligible(fix, 2, 0, /*is_pv_node=*/true));
	CHECK_FALSE(eligible(fix, 2, 0, false, /*in_check=*/true));
	CHECK_FALSE(eligible(fix, 2, 0, false, false, /*is_exclusion_frame=*/true));
	CHECK_FALSE(eligible(fix, 2, 0, false, false, false, /*zugzwang_safe=*/false));
	CHECK_FALSE(eligible(fix, 2, GameValues::Mate_Threshold));
	CHECK_FALSE(eligible(fix, 2, -GameValues::Mate_Threshold));
	CHECK(eligible(fix, 2, GameValues::Mate_Threshold - 1));
	CHECK(eligible(fix, 2, -GameValues::Mate_Threshold + 1));
}

TEST_CASE("Capture SEE pruning: a side below the material floor makes a node ineligible", "[search][capturesee]")
{
	AIPerlexTestFixture sparse(kSparseFen);
	sparse.set_capture_see_pruning(true);
	REQUIRE(sparse.zugzwang_safe());
	CHECK_FALSE(eligible(sparse));

	AIPerlexTestFixture full(kRookLosesFen);
	full.set_capture_see_pruning(true);
	CHECK(eligible(full));
}

// ============================================================================
// The losing-tier read
// ============================================================================

TEST_CASE("Capture SEE pruning: IsLosingCaptureScore holds for the losing tier alone", "[search][capturesee][sort]")
{
	const Board board(kLosingCaptureFen);
	MoveList list;
	MoveGenerator::ComputeLegalMoves(board, list);
	const Move losing = uci_move(kLosingCaptureFen, "e1e6");
	const Move good = uci_move(kLosingCaptureFen, "c3b5");
	const Move hash = uci_move(kLosingCaptureFen, "f1b5");
	const Move killer0 = uci_move(kLosingCaptureFen, "c3e4");
	const Move killer1 = uci_move(kLosingCaptureFen, "c3d5");
	const Move quiet = uci_move(kLosingCaptureFen, "g1h2");

	static const int32_t history[2][64][64]{};
	ScoredMoves scored{};
	const int n = static_cast<int>(list.size());
	MoveSorter::ScoreMoves(list, n, {board, killer0, killer1, history, {}}, hash, scored);

	const auto score_of = [&](const Move& move) {
		for (int i = 0; i < n; ++i)
			if (list[scored[i].second] == move)
				return scored[i].first;
		FAIL("move not generated");
		return 0;
	};

	CHECK(MoveSorter::IsLosingCaptureScore(score_of(losing)));
	CHECK_FALSE(MoveSorter::IsLosingCaptureScore(score_of(good)));
	CHECK_FALSE(MoveSorter::IsLosingCaptureScore(score_of(hash)));
	CHECK_FALSE(MoveSorter::IsLosingCaptureScore(score_of(killer0)));
	CHECK_FALSE(MoveSorter::IsLosingCaptureScore(score_of(killer1)));
	CHECK_FALSE(MoveSorter::IsLosingCaptureScore(score_of(quiet)));
}

// ============================================================================
// Which captures are skipped
// ============================================================================

TEST_CASE("Capture SEE pruning: skips exactly the SEE-losing captures", "[search][capturesee]")
{
	const int depth = GENERATE(1, 2);
	auto fix = enabled_fixture(kLosingCaptureFen);

	const Tally tally = tally_at(fix, depth);
	REQUIRE(tally.candidates == 1);

	search_at(fix, depth);

	CAPTURE(depth);
	CHECK(fix.capture_see_skips() == tally.candidates);
	CHECK(fix.search_board_restored());
}

TEST_CASE("Capture SEE pruning: the flag off skips nothing", "[search][capturesee]")
{
	auto fix = enabled_fixture(kLosingCaptureFen);
	REQUIRE(tally_at(fix, 1).candidates > 0);
	fix.set_capture_see_pruning(false);
	search_at(fix, 1);
	CHECK(fix.capture_see_skips() == 0);
}

TEST_CASE("Capture SEE pruning: the threshold is minus margin times depth", "[search][capturesee]")
{
	// Rxe6 is exactly -400. see_ge is inclusive, so a threshold of -400 keeps it. At depth 2 and margin
	// 100 the linear threshold (-200) skips it, where quiet SEE's depth squared (-400) would keep it.
	const Board board(kRookLosesFen);
	const Move rxe6 = uci_move(kRookLosesFen, "e1e6");
	REQUIRE(See::see_ge(board, rxe6, -400));
	REQUIRE_FALSE(See::see_ge(board, rxe6, -399));

	struct Case {
		int depth;
		int margin;
		bool skips;
	};
	const auto [depth, margin, skips] =
	    GENERATE(Case{1, 399, true}, Case{1, 400, false}, Case{2, 100, true}, Case{2, 199, true}, Case{2, 200, false});

	auto fix = enabled_fixture(kRookLosesFen, 4, margin);
	search_at(fix, depth);

	CAPTURE(depth, margin);
	CHECK((fix.capture_see_skips() > 0) == skips);
}

TEST_CASE("Capture SEE pruning: a node above the maximum depth skips nothing", "[search][capturesee]")
{
	auto fix = enabled_fixture(kLosingCaptureFen, /*max_depth=*/1);
	REQUIRE(tally_at(fix, 2).candidates > 0);
	search_at(fix, 2);
	CHECK(fix.capture_see_skips() == 0);
}

TEST_CASE("Capture SEE pruning: a PV node skips nothing", "[search][capturesee]")
{
	auto fix = enabled_fixture(kLosingCaptureFen);
	REQUIRE(tally_at(fix, 1).candidates > 0);
	fix.search_node(/*depth=*/1, /*ply=*/1, kHighAlpha, kHighAlpha + 1, /*is_pv_node=*/true);
	CHECK(fix.capture_see_skips() == 0);
}

TEST_CASE("Capture SEE pruning: a recapture is searched", "[search][capturesee]")
{
	SECTION("the previous move landed on the capture square")
	{
		auto fix = enabled_fixture(kLosingCaptureFen);
		// Black's last move put the pawn on e6; Qxe6 takes it back.
		fix.set_previous_move_to(/*ply=*/1, "e1e6");
		const Tally tally = tally_at(fix, 1);
		REQUIRE(tally.losing_recaptures == 1);
		REQUIRE(tally.candidates == 0);
		search_at(fix, 1);
		CHECK(fix.capture_see_skips() == 0);
	}
	SECTION("the previous move landed elsewhere")
	{
		auto fix = enabled_fixture(kLosingCaptureFen);
		fix.set_previous_move_to(/*ply=*/1, "c3b5");
		const Tally tally = tally_at(fix, 1);
		REQUIRE(tally.candidates == 1);
		search_at(fix, 1);
		CHECK(fix.capture_see_skips() == tally.candidates);
	}
	SECTION("below a null move there is no previous move")
	{
		auto fix = enabled_fixture(kLosingCaptureFen);
		fix.cont_key(1) = ThreadData::kNoContinuation;
		const Tally tally = tally_at(fix, 1);
		REQUIRE(tally.candidates == 1);
		search_at(fix, 1);
		CHECK(fix.capture_see_skips() == tally.candidates);
	}
}

TEST_CASE("Capture SEE pruning: a checking capture is searched", "[search][capturesee]")
{
	auto fix = enabled_fixture(kLosingCheckFen);
	const Tally tally = tally_at(fix, 1);
	REQUIRE(tally.losing_checks > 0);

	search_at(fix, 1);

	CHECK(fix.capture_see_skips() == tally.candidates);
}

TEST_CASE("Capture SEE pruning: a side below the material floor skips nothing", "[search][capturesee]")
{
	auto fix = enabled_fixture(kSparseFen);
	fix.store_killer_uci(/*ply=*/1, "c3e4");
	REQUIRE(tally_at(fix, 1).candidates == 1);

	search_at(fix, 1);

	CHECK(fix.capture_see_skips() == 0);
}

// ============================================================================
// What the node returns and stores
// ============================================================================

TEST_CASE("Capture SEE pruning: a skipped fail-low returns alpha and stores UPPER at alpha", "[search][capturesee][tt]")
{
	// Every searched move draws, strictly below alpha, so only the floor lifts the node to it. A move
	// answered by quiescence would not do: its fail-hard stand-pat returns alpha itself. The killer puts a
	// drawing move ahead of Rxe6.
	auto fix = enabled_fixture(kDrawsOnlyFen);
	fix.store_killer_uci(/*ply=*/1, "c3b5");
	REQUIRE(tally_at(fix, 1).candidates == 1);

	const int score = search_at(fix, 1);

	REQUIRE(fix.capture_see_skips() == 1);
	CHECK(score == kHighAlpha);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->value == kHighAlpha);
	CHECK(entry->depth == 1);
	CHECK(entry->bound == BoundType::UPPER);
	CHECK_FALSE(entry->best_move.is_null());
}

TEST_CASE("Capture SEE pruning: a searched cutoff after a skip stores LOWER", "[search][capturesee][tt]")
{
	// Both killers draw below alpha, Rxe6 is skipped, and a later quiet fails high: White is a rook,
	// a bishop and a knight up.
	constexpr int kAlpha = 100;
	auto fix = enabled_fixture(kDrawsThenCaptureFen);
	fix.set_reverse_futility(false);
	fix.set_null_move_enabled(false);
	fix.store_killer_uci(/*ply=*/1, "c3b5");
	fix.store_killer_uci(/*ply=*/1, "c3d5");
	REQUIRE(tally_at(fix, 1).candidates == 1);

	const int score = search_at(fix, 1, kAlpha);

	REQUIRE(fix.capture_see_skips() == 1);
	CHECK(score > kAlpha);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->bound == BoundType::LOWER);
}

TEST_CASE("Capture SEE pruning: disabling the depth-2 LMP store still preserves the parent entry",
          "[search][capturesee][tt]")
{
	constexpr int16_t kPlantedValue = 1234;

	auto fix = enabled_fixture(kLosingCaptureFen);
	fix.set_late_move_pruning(true);
	fix.set_late_move_pruning_depth2_store_upper(false);
	fix.set_late_move_pruning_min_index(4);
	// Shallower than the node, so it supplies no cutoff.
	fix.store_main_entry(kPlantedValue, /*depth=*/1, /*ply=*/1, BoundType::EXACT);

	const int score = search_at(fix, 2);

	REQUIRE(fix.capture_see_skips() > 0);
	REQUIRE(fix.lmp_skips() > 0);
	CHECK(score == kHighAlpha);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->value == kPlantedValue);
	CHECK(entry->depth == 1);
	CHECK(entry->bound == BoundType::EXACT);
}

// ============================================================================
// Whole searches and the counter
// ============================================================================

TEST_CASE("Capture SEE pruning: an aborted search unwinds to the root board, deterministically",
          "[search][capturesee][nodes]")
{
	const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
	constexpr int64_t node_budget = 20000;

	AIPerlexTestFixture first(fen, /*max_depth=*/8);
	first.set_capture_see_pruning(true);
	const Move first_move = first.search_with_nodes(node_budget);

	REQUIRE_FALSE(first_move.is_null());
	REQUIRE(first.search_is_aborted());
	REQUIRE(first.capture_see_skips() > 0);
	CHECK(first.search_board_restored());

	AIPerlexTestFixture second(fen, /*max_depth=*/8);
	second.set_capture_see_pruning(true);
	const Move second_move = second.search_with_nodes(node_budget);

	CHECK(MoveFormatter::ToUCI(first_move) == MoveFormatter::ToUCI(second_move));
	CHECK(first.capture_see_skips() == second.capture_see_skips());
	CHECK(first.mainnodes() + first.qnodes() == second.mainnodes() + second.qnodes());
}

TEST_CASE("Capture SEE pruning: the search result reports the skip count, and off reports none", "[search][capturesee]")
{
	const char* kiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	AIPerlexTestFixture on(kiwipete);
	on.set_capture_see_pruning(true);
	const SearchResult on_result = on.result_to_depth(5);
	REQUIRE(on.capture_see_skips() > 0);
	CHECK(on_result.telemetry.capture_see.skips == on.capture_see_skips());

	AIPerlexTestFixture off(kiwipete);
	const SearchResult off_result = off.result_to_depth(5);
	CHECK(off_result.telemetry.capture_see.skips == 0);
}

TEST_CASE("Capture SEE pruning: the counter sums across threads and resets", "[search][capturesee][telemetry]")
{
	SearchTelemetry total;
	total.capture_see.skips = 3;
	SearchTelemetry helper;
	helper.capture_see.skips = 4;
	total.add(helper);
	CHECK(total.capture_see.skips == 7);
	total.reset();
	CHECK(total.capture_see.skips == 0);
}
