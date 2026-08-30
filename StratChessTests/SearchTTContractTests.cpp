// SearchTTContractTests.cpp — Catch2 tests for the contract between the search and the
// transposition table: how a terminal node is stored, and what a probed bound may and may not
// do to the caller's window. The table itself is covered by TTTests.cpp.

#include "SearchTestFixture.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "MoveFormatter.h"
#include "TranspositionTable.h"
#include "defines.h"
#include <cstdint>
#include <string>

// ============================================================================
// Terminal-node TT storage
// ============================================================================
// A node with no legal move leaves best_value at the -Search_Init sentinel.
// Narrowing that to the TT's int16_t value field wraps it to +15536, so pvs()
// must resolve the checkmate/stalemate score before it classifies and stores
// the entry. These tests drive a single pvs() node so the terminal position
// sits at a chosen ply, then read back what reached the table.

TEST_CASE("Search - checkmate node is stored as an exact ply-adjusted mate score", "[search][tt]")
{
	// Black to move and mated: Ra8 covers the back rank, f7/g7/h7 block every escape.
	AIPerlexTestFixture fix("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 0);

	const int ply = GENERATE(0, 1, 5);
	INFO("ply = " << ply);

	REQUIRE(fix.search_node(/*depth=*/3, ply) == -GameValues::Mate + ply);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	CHECK(entry->value == -GameValues::Mate + ply);
	// The wrapped sentinel this storage path used to write instead.
	CHECK(entry->value != static_cast<int16_t>(-GameValues::Search_Init));
	CHECK(entry->bound == BoundType::EXACT);
	CHECK(entry->best_move.is_null());
	CHECK(entry->phase == SearchPhase::MAIN);
}

TEST_CASE("Search - stalemate node is stored as an exact draw score", "[search][tt]")
{
	// White to move and stalemated: Qf2 covers g1/g2/h2 without giving check.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/5q2/7K w - - 0 1");
	REQUIRE_FALSE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 0);

	const int ply = GENERATE(0, 2, 7);
	INFO("ply = " << ply);

	REQUIRE(fix.search_node(/*depth=*/3, ply) == GameValues::Draw);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	CHECK(entry->value == GameValues::Draw);
	CHECK(entry->value != static_cast<int16_t>(-GameValues::Search_Init));
	CHECK(entry->bound == BoundType::EXACT);
	CHECK(entry->best_move.is_null());
	CHECK(entry->phase == SearchPhase::MAIN);
}

TEST_CASE("Search - terminal score is stored exact even when it falls outside the window", "[search][tt]")
{
	// The terminal value is the position's true minimax value, not a window-relative
	// bound, so it is stored EXACT regardless of the window it was searched under.
	AIPerlexTestFixture fix("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
	REQUIRE(fix.count_legal_moves() == 0);

	REQUIRE(fix.search_node(/*depth=*/3, /*ply=*/0, /*alpha=*/100, /*beta=*/200) == -GameValues::Mate);

	const auto entry = fix.probe_tt(0);
	REQUIRE(entry.has_value());
	CHECK(entry->value == -GameValues::Mate);
	CHECK(entry->bound == BoundType::EXACT);
}

TEST_CASE("Search - a probe of a terminal entry cannot return the wrapped sentinel", "[search][tt]")
{
	// The stored value only misleads a prober whose alpha already exceeds +15536,
	// which is the regime an aspiration window enters after a mate score is seeded.
	// Stored as an UPPER bound of +15536, the entry collapsed such a window and
	// handed the caller +155 pawns for a drawn position; stored EXACT it cannot.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/5q2/7K w - - 0 1");
	REQUIRE(fix.count_legal_moves() == 0);

	// Seed the entry at a depth deeper than the probing call will ask for.
	REQUIRE(fix.search_node(/*depth=*/5, /*ply=*/0) == GameValues::Draw);

	const int score = fix.search_node(/*depth=*/1, /*ply=*/0, /*alpha=*/GameValues::Mate - 60,
	                                  /*beta=*/GameValues::Mate, /*is_pv_node=*/false);
	CHECK(score == GameValues::Draw);
	CHECK(score != static_cast<int16_t>(-GameValues::Search_Init));
}

TEST_CASE("Search - a full search stores its mated child node correctly", "[search][tt]")
{
	// Ra8 is mate in one. Depth 2 is the shallowest search that reaches the mated
	// position through pvs(): at depth 1 the child node is entered with depth 0 and
	// handed to quiescence, which never generates moves or detects mate.
	const std::string fen = "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1";
	AIPerlexTestFixture fix(fen);

	Board mated(fen);
	const Move mating_move = MoveFormatter::FromUCI("a1a8", mated);
	REQUIRE(mated.DoMove(mating_move));
	const uint64_t mated_key = mated.get_zobrist_hash();

	REQUIRE(MoveFormatter::ToUCI(fix.search_to_depth(2)) == "a1a8");

	// The mated position was searched at ply 1, so probing at ply 1 must give back
	// the same mate-in-one the root saw.
	const auto entry = fix.probe_tt(mated_key, 1);
	REQUIRE(entry.has_value());
	CHECK(entry->value == -GameValues::Mate + 1);
	CHECK(entry->bound == BoundType::EXACT);
	CHECK(entry->best_move.is_null());
}

// A MAIN entry is strictly more search than a quiescence node at the same position, so
// quiescence() may cut off on one. What it must not do is use one to narrow its window:
// the classification at the bottom of quiescence() measures against the alpha captured
// before the probe, so a narrowing that is invisible there lets the node return, and store
// as EXACT, a value produced under a window the caller never asked for.
TEST_CASE("Qsearch - a MAIN bound inside the window does not change the value", "[search][tt][qsearch]")
{
	// Enough hanging material that quiescence has real captures to resolve, so a changed
	// window can actually change the outcome.
	const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	constexpr int alpha = -50;
	constexpr int beta = 5000;

	AIPerlexTestFixture clean(fen);
	const int without_entry = clean.quiesce_node(alpha, beta, AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0);

	// Strictly inside (alpha, beta), so it can never license a cutoff — only a narrowing.
	REQUIRE(without_entry > alpha);
	REQUIRE(without_entry < beta);

	AIPerlexTestFixture seeded(fen);
	seeded.store_main_entry(static_cast<int16_t>(without_entry + 200), /*depth=*/1, /*ply=*/0, BoundType::LOWER);
	const int with_entry = seeded.quiesce_node(alpha, beta, AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0);

	CHECK(with_entry == without_entry);
}

// The other half of the same contract: an entry that DOES resolve the node against the
// caller's window is still used, so the fix above did not simply disable the cache.
TEST_CASE("Qsearch - a MAIN bound at or beyond beta cuts off", "[search][tt][qsearch]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	constexpr int16_t stored = 4000;
	fix.store_main_entry(stored, /*depth=*/1, /*ply=*/0, BoundType::LOWER);

	CHECK(fix.quiesce_node(/*alpha=*/-50, /*beta=*/stored - 100, AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0) ==
	      stored);
}

// pvs() carries the same contract quiescence() does, and had the same defect: original_alpha
// is captured before the probe, so a window narrowed from a TT bound is invisible to the
// classification at the bottom of the function. The node then searches under one window and
// reports under another, and can store the result as EXACT.
//
// Driven through a non-PV node with a wide window, which is where the narrowing was enabled.
// The search itself never produces that combination -- every !is_pv_node call site passes a
// null window, which is why the fix costs no nodes -- but pvs() must not depend on the caller
// for the property.
TEST_CASE("Search - a TT bound inside the window does not change the value", "[search][tt]")
{
	// Enough hanging material for the window to actually change which children survive.
	const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	constexpr int depth = 3;
	constexpr int alpha = -50;
	constexpr int beta = 5000;

	AIPerlexTestFixture clean(fen);
	clean.arm_clock();
	const int without_entry = clean.search_node(depth, /*ply=*/0, alpha, beta, /*is_pv_node=*/false);

	// Strictly inside (alpha, beta), so the planted entry can never license a cutoff --
	// asserted for the seeded value itself, not just for the node's own score, or an eval
	// change that lifts without_entry could turn this into a cutoff test by accident.
	REQUIRE(without_entry > alpha);
	REQUIRE(without_entry + 200 < beta);

	AIPerlexTestFixture seeded(fen);
	seeded.arm_clock();
	seeded.store_main_entry(static_cast<int16_t>(without_entry + 200), depth, /*ply=*/0, BoundType::LOWER);
	const int with_entry = seeded.search_node(depth, /*ply=*/0, alpha, beta, /*is_pv_node=*/false);

	CHECK(with_entry == without_entry);

	// The stored bound is the other half of the damage: a value produced under a narrowed
	// window classified EXACT is what a later probe would serve as the node's true score.
	const auto entry = seeded.probe_tt(/*ply=*/0);
	REQUIRE(entry.has_value());
	CHECK(entry->value == static_cast<int16_t>(without_entry));
}

// The other half of the contract: an entry that DOES resolve the node against the caller's
// window is still used, so the fix did not simply disable the cache.
TEST_CASE("Search - a TT bound at or beyond beta cuts off", "[search][tt]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	constexpr int16_t stored = 4000;
	fix.arm_clock();
	fix.store_main_entry(stored, /*depth=*/3, /*ply=*/0, BoundType::LOWER);

	CHECK(fix.search_node(/*depth=*/3, /*ply=*/0, /*alpha=*/-50, /*beta=*/stored - 100, /*is_pv_node=*/false) ==
	      stored);
}

// The UPPER half of the same cutoff contract. Neither the pair above nor #392's qsearch pair
// reaches it, so without this the branch is untested.
TEST_CASE("Search - a TT bound at or below alpha cuts off", "[search][tt]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	constexpr int16_t stored = -4000;
	fix.arm_clock();
	fix.store_main_entry(stored, /*depth=*/3, /*ply=*/0, BoundType::UPPER);

	CHECK(fix.search_node(/*depth=*/3, /*ply=*/0, /*alpha=*/stored + 100, /*beta=*/5000, /*is_pv_node=*/false) ==
	      stored);
}
