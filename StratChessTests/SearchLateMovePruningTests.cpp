// SearchLateMovePruningTests.cpp — depth-two late move pruning (#547).
//
// The eligibility tests move one node-level guard at a time off a passing baseline. The node tests
// hold the engine's skip count against the fixture's independent tally, after requiring that the
// position puts a late move behind the exemption under test, so removing that guard shows up as
// extra skips. The hash-move term is the exception: the hash move sorts first, so it can never be
// late, and no case claims it.
//
// The feature ships on. Every case still sets the flag, so each one names the configuration it
// asserts about.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// Quiet middlegame position, for the eligibility guards that never look at the board.
	constexpr const char* kBaselineFen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	// 29 legal moves: two captures and four promotions sort first, and two quiet checks land late.
	// Verified against python-chess.
	constexpr const char* kMixedFen = "7k/1P6/8/3p4/8/2N5/8/3QK3 w - - 0 1";

	// 31 legal moves, 19 captures (an en passant among them) and 24 promotions, so captures, the en
	// passant, promotions and both killer slots all reach the pruning index. Verified against
	// python-chess.
	constexpr const char* kCrowdedFen = "r1r2r1r/1P4P1/8/3pP2k/1p6/2p5/N7/4K3 w - d6 0 1";

	// The e7 rook pins the e2 knight: its six pseudo-legal moves sort among the quiets and DoMove
	// rejects them, so indexing the pseudo-legal list would skip more. Verified against python-chess.
	constexpr const char* kPinnedKnightFen = "k7/4r3/8/8/8/8/4N3/1R2K1R1 w - - 0 1";

	// White in check from the e8 rook with 15 legal evasions, most of them quiet interpositions.
	// Verified against python-chess.
	constexpr const char* kInCheckFen = "1k2r3/8/7R/2NN2B1/Q7/8/1B5R/4K3 w - - 0 1";

	// White a knight, a bishop and a pawn up, one half-move from the fifty-move limit: every piece
	// move draws on the spot, and only the two pawn pushes do not. Verified against python-chess.
	constexpr const char* kFiftyMoveEdgeFen = "k7/8/8/8/3N4/8/P6B/4K3 w - - 99 1";
	// Piece moves given history so they sort ahead of the pawn pushes, which then land late.
	constexpr std::array<const char*, 12> kFiftyMoveEdgeEarlyMoves = {"d4b3", "d4b5", "d4c2", "d4c6", "d4e2", "d4e6",
	                                                                  "d4f3", "d4f5", "h2g1", "h2g3", "h2f4", "h2e5"};

	// White a queen and a rook down, one half-move from the fifty-move limit. The 16 pawn moves
	// sort first and fail low; the two king moves sort last and draw, well above that window.
	constexpr const char* kPawnsThenDrawFen = "qr5k/8/8/8/8/8/PPPPPPPP/4K3 w - - 99 1";

	// White's only legal move is Kb1.
	constexpr const char* kOneMoveFen = "q6k/8/8/8/8/p7/P7/K7 w - - 0 1";

	// Black to move and stalemated.
	constexpr const char* kStalemateFen = "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1";

	// Far above anything these positions reach and outside the mate range: no searched move fails
	// high, so every legal move is visited.
	constexpr int kHighAlpha = 5000;

	using Tally = AIPerlexTestFixture::LateMoveTally;

	// The baseline call: depth 2, non-PV null window, not in check, no exclusion, far from mate.
	// Every eligibility case changes exactly one of these.
	bool eligible(const AIPerlexTestFixture& fix, int depth = 2, int alpha = 0, int beta = 1, bool is_pv_node = false,
	              bool in_check = false, bool is_exclusion_frame = false)
	{
		return fix.late_move_pruning_eligible(depth, alpha, beta, is_pv_node, in_check, is_exclusion_frame);
	}

	// One depth-2 null-window node at ply 1, the only shape the guard applies to.
	int search_depth2(const AIPerlexTestFixture& fix, int alpha)
	{
		return fix.search_node(/*depth=*/2, /*ply=*/1, alpha, alpha + 1, /*is_pv_node=*/false);
	}

	Tally late_moves(const AIPerlexTestFixture& fix) { return fix.tally_late_moves(/*ply=*/1, 12); }

	AIPerlexTestFixture enabled_fixture(const char* fen)
	{
		AIPerlexTestFixture fix(fen);
		fix.set_late_move_pruning(true);
		fix.arm_clock();
		return fix;
	}
} // namespace

// ============================================================================
// Eligibility
// ============================================================================

TEST_CASE("Late move pruning: the runtime flag starts on, and off makes a node ineligible", "[search][lmp]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	CHECK(fix.late_move_pruning_enabled());

	fix.set_late_move_pruning(false);
	CHECK_FALSE(eligible(fix));
}

TEST_CASE("Late move pruning: baseline node is eligible", "[search][lmp]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_late_move_pruning(true);

	CHECK(eligible(fix));
}

TEST_CASE("Late move pruning: only depth 2 is pruned", "[search][lmp]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_late_move_pruning(true);

	CHECK_FALSE(eligible(fix, /*depth=*/1));
	CHECK(eligible(fix, /*depth=*/2));
	CHECK_FALSE(eligible(fix, /*depth=*/3));
}

TEST_CASE("Late move pruning: PV nodes and wider windows are never pruned", "[search][lmp]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_late_move_pruning(true);

	CHECK_FALSE(eligible(fix, 2, 0, 1, /*is_pv_node=*/true));
	CHECK_FALSE(eligible(fix, 2, 0, /*beta=*/2));
}

TEST_CASE("Late move pruning: in-check and exclusion frames are never pruned", "[search][lmp]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_late_move_pruning(true);

	CHECK_FALSE(eligible(fix, 2, 0, 1, false, /*in_check=*/true));
	CHECK_FALSE(eligible(fix, 2, 0, 1, false, false, /*is_exclusion_frame=*/true));
}

TEST_CASE("Late move pruning: both window endpoints must be outside the mate range", "[search][lmp]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_late_move_pruning(true);
	const int mate = GameValues::Mate_Threshold;

	CHECK(eligible(fix, 2, mate - 2, mate - 1));
	CHECK_FALSE(eligible(fix, 2, mate - 1, mate)); // beta reaches the threshold
	CHECK(eligible(fix, 2, -mate + 1, -mate + 2));
	CHECK_FALSE(eligible(fix, 2, -mate, -mate + 1)); // alpha reaches the threshold
}

// ============================================================================
// The node-level guards as pvs() wires them
// ============================================================================
// Each position below has late candidates, so a node that lost its one excluding property on the
// way into the predicate would skip them.

TEST_CASE("Late move pruning: an ineligible node skips nothing", "[search][lmp]")
{
	SECTION("flag off")
	{
		AIPerlexTestFixture fix(kMixedFen);
		fix.set_late_move_pruning(false);
		fix.arm_clock();
		search_depth2(fix, kHighAlpha);
		CHECK(fix.lmp_skips() == 0);
	}
	SECTION("depth 1")
	{
		auto fix = enabled_fixture(kMixedFen);
		fix.search_node(/*depth=*/1, /*ply=*/1, kHighAlpha, kHighAlpha + 1, /*is_pv_node=*/false);
		CHECK(fix.lmp_skips() == 0);
	}
	SECTION("PV node")
	{
		auto fix = enabled_fixture(kMixedFen);
		// A null window, so only the PV flag makes the node ineligible.
		fix.search_node(/*depth=*/2, /*ply=*/1, kHighAlpha, kHighAlpha + 1, /*is_pv_node=*/true);
		CHECK(fix.lmp_skips() == 0);
	}
	SECTION("non-PV node with a wider window")
	{
		auto fix = enabled_fixture(kMixedFen);
		fix.search_node(/*depth=*/2, /*ply=*/1, kHighAlpha, kHighAlpha + 2, /*is_pv_node=*/false);
		CHECK(fix.lmp_skips() == 0);
	}
	SECTION("mate-range window")
	{
		auto fix = enabled_fixture(kMixedFen);
		search_depth2(fix, GameValues::Mate_Threshold - 1);
		CHECK(fix.lmp_skips() == 0);
	}
	SECTION("exclusion frame")
	{
		AIPerlexTestFixture fix(kMixedFen);
		fix.set_late_move_pruning(true);
		fix.search_node_excluding(/*depth=*/2, /*ply=*/1, "c3d5", kHighAlpha, kHighAlpha + 1);
		CHECK(fix.lmp_skips() == 0);
	}
	SECTION("in check")
	{
		auto fix = enabled_fixture(kInCheckFen);
		REQUIRE(late_moves(fix).candidates > 0);
		search_depth2(fix, kHighAlpha);
		CHECK(fix.lmp_skips() == 0);
	}
}

// ============================================================================
// Which moves are skipped
// ============================================================================

TEST_CASE("Late move pruning: skips late quiet moves from the thirteenth legal move", "[search][lmp]")
{
	auto fix = enabled_fixture(kMixedFen);

	// Index 11 is a candidate too, so an off-by-one threshold skips one more; the quiet checks
	// are late, so skipping a checking move would too.
	const Tally tally = late_moves(fix);
	REQUIRE(tally.candidates == 15);
	REQUIRE(fix.tally_late_moves(/*ply=*/1, 11).candidates == tally.candidates + 1);
	REQUIRE(tally.late_checks > 0);

	search_depth2(fix, kHighAlpha);

	CHECK(fix.lmp_skips() == tally.candidates);
	CHECK(fix.search_board_restored());
}

TEST_CASE("Late move pruning: late captures, promotions and killers are searched", "[search][lmp]")
{
	auto fix = enabled_fixture(kCrowdedFen);
	fix.store_killer_uci(/*ply=*/1, "e1d1");
	fix.store_killer_uci(/*ply=*/1, "e1e2");

	const Tally tally = late_moves(fix);
	REQUIRE(tally.candidates > 0);
	REQUIRE(tally.late_captures > 0);
	REQUIRE(tally.late_en_passant > 0);
	REQUIRE(tally.late_promotions > 0);
	REQUIRE(tally.late_killers == 2);

	search_depth2(fix, kHighAlpha);

	CHECK(fix.lmp_skips() == tally.candidates);
}

TEST_CASE("Late move pruning: moves DoMove rejects do not advance the index", "[search][lmp]")
{
	auto fix = enabled_fixture(kPinnedKnightFen);

	const Tally tally = late_moves(fix);
	REQUIRE(tally.candidates > 0);
	REQUIRE(tally.candidates_by_pseudo_index != tally.candidates);

	search_depth2(fix, kHighAlpha);

	CHECK(fix.lmp_skips() == tally.candidates);
}

TEST_CASE("Late move pruning: a move that repeats the position is never skipped", "[search][lmp]")
{
	AIPerlexTestFixture fix("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	fix.set_late_move_pruning(true);
	fix.arm_clock();
	const std::initializer_list<const char*> line = {"g1f3", "g8f6", "f3g1"};

	// Black's Ng8 returns to the root position, a repetition inside the search.
	const Tally tally = fix.tally_late_moves(fix.board_after(line), /*ply=*/3, 12);
	REQUIRE(tally.candidates > 0);
	REQUIRE(tally.late_draws == 1);

	fix.search_node_after(line, /*depth=*/2, kHighAlpha, kHighAlpha + 1, /*is_pv_node=*/false);

	CHECK(fix.lmp_skips() == tally.candidates);
}

TEST_CASE("Late move pruning: single-move and terminal nodes skip nothing", "[search][lmp]")
{
	SECTION("only one legal move")
	{
		auto fix = enabled_fixture(kOneMoveFen);
		REQUIRE(fix.count_legal_moves() == 1);
		const int score = search_depth2(fix, kHighAlpha);
		CHECK(fix.lmp_skips() == 0);
		CHECK(score != GameValues::Draw);
	}
	SECTION("stalemate")
	{
		auto fix = enabled_fixture(kStalemateFen);
		REQUIRE(fix.count_legal_moves() == 0);
		const int score = search_depth2(fix, kHighAlpha);
		CHECK(fix.lmp_skips() == 0);
		CHECK(score == GameValues::Draw);
	}
}

// ============================================================================
// What a node that skipped a move may return and store
// ============================================================================

TEST_CASE("Late move pruning: a skipped fail-low returns entry alpha and stores nothing", "[search][lmp][tt]")
{
	// Every searched move draws, below alpha; the two late pawn pushes would have failed high. The
	// node must neither report the draw nor overwrite the planted entry with a bound on moves it
	// never searched.
	constexpr int kAlpha = 100;
	constexpr int16_t kPlantedValue = 1234;

	auto fix = enabled_fixture(kFiftyMoveEdgeFen);
	// White stands far above beta, so reverse futility would return before the move loop.
	fix.set_reverse_futility(false);
	for (const char* uci : kFiftyMoveEdgeEarlyMoves)
		fix.seed_history(uci, 1);
	// Shallower than the node, so it supplies no cutoff.
	fix.store_main_entry(kPlantedValue, /*depth=*/1, /*ply=*/1, BoundType::EXACT);

	const Tally tally = late_moves(fix);
	REQUIRE(tally.candidates == 2);
	REQUIRE(tally.late_draws > 0);

	const int score = search_depth2(fix, kAlpha);

	REQUIRE(fix.lmp_skips() == tally.candidates);
	CHECK(score == kAlpha);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->value == kPlantedValue);
	CHECK(entry->depth == 1);
	CHECK(entry->bound == BoundType::EXACT);
}

TEST_CASE("Late move pruning: the same node without a skip fails high and stores", "[search][lmp][tt]")
{
	// The control for the case above: with the flag off, a pawn push is searched, fails high and
	// replaces the planted entry, so the case above is not passing on a store that never happens.
	constexpr int kAlpha = 100;

	AIPerlexTestFixture fix(kFiftyMoveEdgeFen);
	fix.set_late_move_pruning(false);
	fix.set_reverse_futility(false);
	fix.arm_clock();
	for (const char* uci : kFiftyMoveEdgeEarlyMoves)
		fix.seed_history(uci, 1);
	fix.store_main_entry(1234, /*depth=*/1, /*ply=*/1, BoundType::EXACT);

	const int score = search_depth2(fix, kAlpha);

	CHECK(fix.lmp_skips() == 0);
	CHECK(score > kAlpha);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->depth == 2);
	CHECK(entry->bound == BoundType::LOWER);
}

TEST_CASE("Late move pruning: a searched cutoff after a skip stores LOWER", "[search][lmp][tt]")
{
	constexpr int kAlpha = -300;

	auto fix = enabled_fixture(kPawnsThenDrawFen);
	const Tally tally = late_moves(fix);
	REQUIRE(tally.candidates == 4);
	REQUIRE(tally.late_draws == 2);

	const int score = search_depth2(fix, kAlpha);

	REQUIRE(fix.lmp_skips() == tally.candidates);
	CHECK(score == GameValues::Draw);
	const auto entry = fix.probe_tt(/*ply=*/1);
	REQUIRE(entry.has_value());
	CHECK(entry->depth == 2);
	CHECK(entry->bound == BoundType::LOWER);
	CHECK(entry->value == GameValues::Draw);
	CHECK(fix.has_killer(1));
}

// ============================================================================
// Whole searches
// ============================================================================

TEST_CASE("Late move pruning: an aborted search unwinds to the root board, deterministically", "[search][lmp][nodes]")
{
	const std::string fen = "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2";
	constexpr int64_t node_budget = 20000;

	AIPerlexTestFixture first(fen, /*max_depth=*/8);
	first.set_late_move_pruning(true);
	const Move first_move = first.search_with_nodes(node_budget);

	REQUIRE_FALSE(first_move.is_null());
	REQUIRE(first.search_is_aborted());
	REQUIRE(first.lmp_skips() > 0);
	CHECK(first.search_board_restored());

	AIPerlexTestFixture second(fen, /*max_depth=*/8);
	second.set_late_move_pruning(true);
	const Move second_move = second.search_with_nodes(node_budget);

	CHECK(MoveFormatter::ToUCI(first_move) == MoveFormatter::ToUCI(second_move));
	CHECK(first.lmp_skips() == second.lmp_skips());
	CHECK(first.mainnodes() + first.qnodes() == second.mainnodes() + second.qnodes());
}

TEST_CASE("Late move pruning: the search result reports the skip count", "[search][lmp]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_late_move_pruning(true);

	const SearchResult result = fix.result_to_depth(5);

	REQUIRE(fix.lmp_skips() > 0);
	CHECK(result.late_move_pruning_skips == fix.lmp_skips());
}
