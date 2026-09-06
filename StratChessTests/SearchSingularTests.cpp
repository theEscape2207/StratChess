// SearchSingularTests.cpp — singular extensions (#95).
//
// Two groups. The eligibility tests move one knob at a time off a baseline that is known to
// trigger, so a failure names the gate that broke. The exclusion-semantics tests drive a
// verification frame directly, because those guards are what make an exclusion search safe
// to run at all and none of them is observable from a normal search.
//
// The feature ships disabled, so every case here turns it on first. That is deliberate: it
// means this file is also the proof that the default-off build is the untested path only in
// the sense that it does nothing.

#include <catch2/catch_test_macros.hpp>
#include "SearchTestFixture.h"

namespace {
	// Quiet middlegame position with plenty of legal moves, used as the eligibility baseline.
	constexpr const char* kBaselineFen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	// Depth and TT depth chosen to clear the default gate (min_depth 8, tt_depth_margin 3).
	constexpr int kDepth = 8;
	constexpr int16_t kTtDepth = 8;

	// Comfortably above anything the alternatives will score, so the verification fails low
	// and the extension is granted. Well below Mate_Threshold, so the mate-score gate passes.
	constexpr int16_t kTtValue = 900;

	// Arms the fixture at the baseline: feature on, a LOWER entry naming the first sorted
	// move, and a clock so the per-node poll does not latch an abort.
	void arm_baseline(const AIPerlexTestFixture& fix)
	{
		fix.set_singular_enabled(true);
		fix.arm_clock();
		fix.store_main_entry_with_move(kTtValue, kTtDepth, /*ply=*/1, BoundType::LOWER, fix.first_sorted_move_uci());
		fix.clear_singular_telemetry();
	}
} // namespace

// ============================================================================
// Eligibility
// ============================================================================

TEST_CASE("Singular: baseline position triggers a verification and extends", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	arm_baseline(fix);

	fix.search_node(kDepth, /*ply=*/1);

	CHECK(fix.singular_eligible() == 1);
	CHECK(fix.singular_verifications() == 1);
	// The TT claims 900 for this move; nothing else in a quiet position comes close, so the
	// verification fails below the margin and the move is singular.
	CHECK(fix.singular_extensions() == 1);
}

TEST_CASE("Singular: disabled by default", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	arm_baseline(fix);
	fix.set_singular_enabled(false); // the shipped configuration

	fix.search_node(kDepth, /*ply=*/1);

	CHECK(fix.singular_eligible() == 0);
	CHECK(fix.singular_verifications() == 0);
	CHECK(fix.singular_extensions() == 0);
}

TEST_CASE("Singular: not eligible below the depth threshold", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	arm_baseline(fix);
	fix.set_singular_min_depth(kDepth + 1); // one more than this node has

	fix.search_node(kDepth, /*ply=*/1);

	CHECK(fix.singular_eligible() == 0);
}

TEST_CASE("Singular: not eligible at the root", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	arm_baseline(fix);

	fix.search_node(kDepth, /*ply=*/0);

	CHECK(fix.singular_eligible() == 0);
}

TEST_CASE("Singular: an UPPER-bound entry is not a candidate", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_singular_enabled(true);
	fix.arm_clock();
	// An UPPER bound says the value is at most this — it never claims the move is good.
	fix.store_main_entry_with_move(kTtValue, kTtDepth, /*ply=*/1, BoundType::UPPER, fix.first_sorted_move_uci());
	fix.clear_singular_telemetry();

	fix.search_node(kDepth, /*ply=*/1);

	CHECK(fix.singular_eligible() == 0);
}

TEST_CASE("Singular: an entry with no best move is not a candidate", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_singular_enabled(true);
	fix.arm_clock();
	// store_main_entry() plants an empty move — there is nothing to extend.
	fix.store_main_entry(kTtValue, kTtDepth, /*ply=*/1, BoundType::LOWER);
	fix.clear_singular_telemetry();

	fix.search_node(kDepth, /*ply=*/1);

	CHECK(fix.singular_eligible() == 0);
}

TEST_CASE("Singular: a mate-score entry is not a candidate", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_singular_enabled(true);
	fix.arm_clock();
	// The margin arithmetic is meaningless against a mate score, and the TT normalises those
	// by ply — subtracting a depth-scaled margin from one produces a bound with no meaning.
	fix.store_main_entry_with_move(static_cast<int16_t>(GameValues::Mate_Threshold + 10), kTtDepth, /*ply=*/1,
	                               BoundType::LOWER, fix.first_sorted_move_uci());
	fix.clear_singular_telemetry();

	fix.search_node(kDepth, /*ply=*/1);

	CHECK(fix.singular_eligible() == 0);
}

TEST_CASE("Singular: a too-shallow entry is not a candidate", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.set_singular_enabled(true);
	fix.arm_clock();
	// One ply shallower than the margin allows: depth - tt_depth_margin - 1.
	fix.store_main_entry_with_move(kTtValue, static_cast<int16_t>(kDepth - 4), /*ply=*/1, BoundType::LOWER,
	                               fix.first_sorted_move_uci());
	fix.clear_singular_telemetry();

	fix.search_node(kDepth, /*ply=*/1);

	CHECK(fix.singular_eligible() == 0);
}

TEST_CASE("Singular: no verification nested inside a verification", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	arm_baseline(fix);

	// Driven as a verification frame: the exclusion slot is set for this node, so it must not
	// launch a verification of its own even though every other gate would pass.
	fix.search_node_excluding(kDepth, /*ply=*/1, fix.first_sorted_move_uci(), -GameValues::Search_Init,
	                          GameValues::Search_Init);

	CHECK(fix.singular_eligible() == 0);

	// What actually enforces this is the skipped TT probe below, not the !is_exclusion_frame
	// term in the eligibility conjunction: with no probe there is no hash move and no usable
	// entry, so the gate cannot pass however the rest of it is written. Removing that term
	// alone leaves this test green. The probe skip has its own test for exactly that reason.
}

TEST_CASE("Singular: a verification search takes no TT cutoff", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.arm_clock();

	// An entry that WOULD resolve this node instantly: EXACT, deeper than the search, and a
	// value no real evaluation of this position produces.
	constexpr int16_t kDistinctive = 1234;
	fix.store_main_entry_with_move(kDistinctive, /*depth=*/8, /*ply=*/1, BoundType::EXACT, fix.first_sorted_move_uci());

	// Null window at a non-PV node is exactly the shape that takes the cutoff.
	const int score = fix.search_node_excluding(/*depth=*/4, /*ply=*/1, fix.first_sorted_move_uci(), /*alpha=*/-1,
	                                            /*beta=*/0);

	// This key describes the position with every legal move available; the exclusion search is
	// asking about a strictly smaller set. Returning the entry's value would answer the wrong
	// question -- and would let the frame inherit a hash move, which is what would make a
	// nested verification possible.
	CHECK(score != kDistinctive);
}

// ============================================================================
// Exclusion-search semantics
// ============================================================================

TEST_CASE("Singular: excluding the only legal move fails low, not mate", "[search][singular]")
{
	// White is in check from the black queen on b2; Kxb2 is the only legal reply.
	AIPerlexTestFixture fix("k7/8/8/8/8/8/1q6/K7 w - - 0 1");
	REQUIRE(fix.count_legal_moves() == 1);

	const std::string only_move = MoveFormatter::ToUCI(fix.first_sorted_move());
	constexpr int alpha = -100;
	constexpr int beta = -99;

	const int score = fix.search_node_excluding(/*depth=*/4, /*ply=*/1, only_move, alpha, beta);

	// The position HAS a legal move; this search was merely forbidden to play it. Adjudicating
	// it as mate would be a lie about the real position, and would be a mate score rather than
	// the fail-low the caller asked for.
	CHECK(score == alpha);
	CHECK(std::abs(score) < GameValues::Mate_Threshold);
}

TEST_CASE("Singular: a verification search stores nothing under the position's key", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.arm_clock();

	// No entry for this position to begin with.
	REQUIRE_FALSE(fix.probe_tt(/*ply=*/1).has_value());

	fix.search_node_excluding(/*depth=*/4, /*ply=*/1, fix.first_sorted_move_uci(), -GameValues::Search_Init,
	                          GameValues::Search_Init);

	// An exclusion search saw a strictly smaller move set than this key describes. Caching its
	// result here would hand a later probe a partial search dressed as a complete one.
	CHECK_FALSE(fix.probe_tt(/*ply=*/1).has_value());
}

TEST_CASE("Singular: null-move pruning is off inside a verification search", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);

	// Same arguments both times; only the exclusion slot differs.
	CHECK(fix.try_null_move(/*depth=*/6, /*beta=*/100, /*ply=*/1, /*is_pv_node=*/false, /*in_check=*/false));

	const Move excluded = fix.first_sorted_move();
	{
		const ExcludedMoveGuard guard(fix.thread_data(), /*ply=*/1, excluded);
		// Passing is not one of the alternatives a verification is disproving, so a null-move
		// cutoff would let a move be called singular on the strength of a pass.
		CHECK_FALSE(fix.try_null_move(/*depth=*/6, /*beta=*/100, /*ply=*/1, /*is_pv_node=*/false, /*in_check=*/false));
	}

	CHECK(fix.try_null_move(/*depth=*/6, /*beta=*/100, /*ply=*/1, /*is_pv_node=*/false, /*in_check=*/false));
}

TEST_CASE("Singular: the exclusion slot is restored on every exit", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.arm_clock();
	constexpr int kPly = 1;

	REQUIRE(fix.excluded_move(kPly) == Move::EmptyMove());

	fix.search_node_excluding(/*depth=*/4, kPly, fix.first_sorted_move_uci(), -GameValues::Search_Init,
	                          GameValues::Search_Init);

	// A slot left populated would silently disable the transposition table and null-move
	// pruning for every later node at this ply, with no symptom but a slower search.
	CHECK(fix.excluded_move(kPly) == Move::EmptyMove());
}

TEST_CASE("Singular: the exclusion slot is restored when the search aborts", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	constexpr int kPly = 1;

	// Latched before the call, so the verification frame returns through pvs()'s early-exit
	// path rather than through the bottom of the function — the case a hand-written
	// set/call/clear sequence would leak, and the reason the guard is RAII.
	fix.request_stop();
	REQUIRE(fix.search_is_aborted());

	fix.search_node_excluding(/*depth=*/4, kPly, fix.first_sorted_move_uci(), -GameValues::Search_Init,
	                          GameValues::Search_Init);

	CHECK(fix.excluded_move(kPly) == Move::EmptyMove());
}

// ============================================================================
// Ply backstop
// ============================================================================

TEST_CASE("Singular: pvs() returns a static evaluation at the ply backstop", "[search][singular]")
{
	AIPerlexTestFixture fix(kBaselineFen);
	fix.arm_clock();

	// Until extensions existed, depth fell on every recursive call and bounded the recursion
	// long before ply could reach here. An extension searches a child at the parent's depth,
	// so pvs() has to bound itself — and it indexes killers, last_move_was_null and the PV
	// table by ply, writing last_move_was_null[ply + 1].
	const int score = fix.search_node(/*depth=*/4, /*ply=*/MAX_PLY - 1);

	CHECK(score == fix.evaluate());
}
