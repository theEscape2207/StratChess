// TTStoreProbeTests.cpp — Catch2 unit tests for TranspositionTable: store/probe round-trips, the static eval, key matching,
// mate-score ply adjustment and clear().
//
// See Docs/TestDesign.md for the rationale.

#include <catch2/catch_test_macros.hpp>
#include "TTTestFixture.h"

TEST_CASE("TT - store and probe preserve all Move encoding bits", "[tt]")
{
	TranspositionTable tt(0);
	const Move move(h7, g8, MoveFlags::PROMOTION_QUEEN_CAPTURE);
	tt.store(KEY_A, 321, 7, 0, move, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->best_move == move);
}

// ── Static evaluation ─────────────────────────────────────────────────────────

TEST_CASE("TT - the static eval round-trips through store and probe", "[tt]")
{
	for (const int16_t eval : {int16_t{0}, int16_t{1}, int16_t{-1}, int16_t{-29899}, int16_t{29899}, kNoStaticEval}) {
		TranspositionTable tt(0);
		tt.store(KEY_A, -250, 6, 0, no_move(), BoundType::UPPER, NodeType::ALL_NODE, SearchPhase::MAIN, eval);
		const auto result = tt.probe(KEY_A, 0);
		INFO("eval " << eval);
		REQUIRE(result.has_value());
		CHECK(result->static_eval == eval);
		CHECK(result->value == -250); // the eval's bits do not leak into the payload
		CHECK(result->depth == 6);
	}
}

TEST_CASE("TT - an accepted same-key store without an eval keeps the stored one", "[tt]")
{
	TranspositionTable tt(0);
	tt.newSearch();
	tt.store(KEY_A, 10, 4, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE, -321);
	tt.store(KEY_A, 60, 8, 0, no_move(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);

	auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->depth == 8); // the store was accepted
	CHECK(result->static_eval == -321);

	// One that carries an eval writes it.
	tt.store(KEY_A, 60, 9, 0, no_move(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, 55);
	result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->static_eval == 55);
}

TEST_CASE("TT - a declined same-key store keeps the stored eval", "[tt]")
{
	TranspositionTable tt(0);
	tt.newSearch();
	tt.store(KEY_A, 500, 12, 0, Move(e2, e4, MoveFlags::QUIET), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN,
	         kNoStaticEval);
	REQUIRE(tt.store(KEY_A, 60, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE, 77) ==
	        TTStoreOutcome::Declined);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->static_eval == kNoStaticEval);
	CHECK(result->value == 500);
}

TEST_CASE("TT - an evicting store without an eval does not inherit the evicted one", "[tt]")
{
	TranspositionTable tt(0); // one bucket, so every key collides
	tt.newSearch();
	tt.store(KEY_A, 10, 1, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, 99);
	tt.store(KEY_B, 20, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, 1);
	tt.store(KEY_C, 30, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, 2);
	tt.store(KEY_D, 40, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, 3);
	tt.store(KEY_MISS, 50, 12, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);

	CHECK_FALSE(tt.probe(KEY_A, 0).has_value());
	const auto result = tt.probe(KEY_MISS, 0);
	REQUIRE(result.has_value());
	CHECK(result->static_eval == kNoStaticEval);
}

TEST_CASE("TT - a probe matches key bits 16-63 and ignores the rest", "[tt]")
{
	// In a one-bucket table the index implies no bits, so the low 16 are the ones the table gave up.
	TranspositionTable tt(0);
	tt.store(KEY_A, 42, 3, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, 7);

	CHECK(tt.probe(KEY_A ^ 0xFFFF, 0).has_value());
	for (unsigned bit = 16; bit < 64; ++bit) {
		INFO("bit " << bit);
		CHECK_FALSE(tt.probe(KEY_A ^ (std::uint64_t{1} << bit), 0).has_value());
	}
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("TT - store then probe returns stored entry", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 500, /*depth=*/5, /*ply=*/0);

	auto result = tt.probe(KEY_A, 0);

	REQUIRE(result.has_value());
	REQUIRE(result->value == 500);
	REQUIRE(result->depth == 5);
	REQUIRE(result->bound == BoundType::EXACT);
	REQUIRE(result->node_type == NodeType::PV_NODE);
}

TEST_CASE("TT - probe with unknown key returns nullopt", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 500);

	REQUIRE_FALSE(tt.probe(KEY_MISS, 0).has_value());
}

TEST_CASE("TT - same-key overwrite: second value is returned", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 100, /*depth=*/3);
	do_store(tt, KEY_A, 999, /*depth=*/6);

	auto result = tt.probe(KEY_A, 0);

	REQUIRE(result.has_value());
	REQUIRE(result->value == 999);
}

TEST_CASE("TT - non-mate score is unchanged regardless of probe ply", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 750, /*depth=*/5, /*ply=*/3);

	auto result = tt.probe(KEY_A, /*current_ply=*/7);

	REQUIRE(result.has_value());
	REQUIRE(result->value == 750); // well below Mate_Threshold — no adjustment
}

TEST_CASE("TT - winning mate score round-trips at same ply", "[tt]")
{
	TranspositionTable tt(1);
	const int16_t mate = static_cast<int16_t>(GameValues::Mate); // 30000

	do_store(tt, KEY_A, mate, /*depth=*/5, /*ply=*/3);
	auto result = tt.probe(KEY_A, /*current_ply=*/3);

	REQUIRE(result.has_value());
	REQUIRE(result->value == mate);
}

TEST_CASE("TT - losing mate score round-trips at same ply", "[tt]")
{
	TranspositionTable tt(1);
	const int16_t losing = -static_cast<int16_t>(GameValues::Mate); // -30000

	do_store(tt, KEY_A, losing, /*depth=*/5, /*ply=*/3);
	auto result = tt.probe(KEY_A, /*current_ply=*/3);

	REQUIRE(result.has_value());
	REQUIRE(result->value == losing);
}

TEST_CASE("TT - winning mate score adjusted when probe ply differs from store ply", "[tt]")
{
	// Store at ply 3: normalized = Mate + 3 = 30003.
	// Probe at ply 5: denormalized = 30003 - 5 = 29998 = Mate - 2.
	TranspositionTable tt(1);
	const int16_t mate = static_cast<int16_t>(GameValues::Mate);

	do_store(tt, KEY_A, mate, /*depth=*/5, /*ply=*/3);
	auto result = tt.probe(KEY_A, /*current_ply=*/5);

	REQUIRE(result.has_value());
	REQUIRE(result->value == mate - 2);
}

TEST_CASE("TT - clear removes all entries; subsequent probes return nullopt", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 100);
	do_store(tt, KEY_B, 200);

	REQUIRE(tt.clear());

	REQUIRE_FALSE(tt.probe(KEY_A, 0).has_value());
	REQUIRE_FALSE(tt.probe(KEY_B, 0).has_value());
}

TEST_CASE("TT - clear reports no work for a freshly constructed table", "[tt]")
{
	TranspositionTable tt(1);

	REQUIRE_FALSE(tt.clear());
}

TEST_CASE("TT - repeated clear reports no work after the table is empty", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 100);

	REQUIRE(tt.clear());
	REQUIRE_FALSE(tt.clear());
}
