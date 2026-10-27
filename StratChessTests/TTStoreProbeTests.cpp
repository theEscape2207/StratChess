// TTStoreProbeTests.cpp — Catch2 unit tests for TranspositionTable: store/probe round-trips, the static eval, key matching,
// mate-score ply adjustment and clear().
//
// See Docs/TestDesign.md for the rationale.

#include <catch2/catch_test_macros.hpp>
#include "TTTestFixture.h"
#include <limits>

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
	for (const int16_t eval : {int16_t{0}, int16_t{1}, int16_t{-1}, int16_t{-29743}, int16_t{29743}, kNoStaticEval}) {
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

// ── Mate band width ────────────────────────────────────────────────────
// A mate d plies from the root scores Mate - d, and the search addresses plies 0..MAX_PLY - 1, so the
// mate band must reach down to Mate - 255. Every value below is a literal worked out by hand, not
// derived from GameValues: a test that recomputed the formula would pass with the formula wrong.

TEST_CASE("TT - the mate band is the whole supported ply range", "[tt]")
{
	static_assert(GameValues::Mate == 30000 && MAX_PLY == 256);
	CHECK(static_cast<int>(GameValues::Mate_Threshold) == 29744);
	// The deepest terminal the search reaches is 254 plies out (29746), two plies inside the band.
	CHECK(29746 >= GameValues::Mate_Threshold);
	// The widest normalised score still fits the TT's int16_t field.
	CHECK(30000 + MAX_PLY < std::numeric_limits<int16_t>::max());
}

TEST_CASE("TT - a mate stored at any supported ply is rebased when probed at another", "[tt]")
{
	struct Case {
		int store_ply;
		int stored_value; // root-relative: 30000 - store_ply, the mate being found at the node itself
		int probe_ply;
		int expected; // 30000 - probe_ply: the same mate, seen from the probing node
	};
	// Distances 99/100/101 straddle the 100-ply mark; 254 and 255 are the last plies.
	// clang-format off
	const Case cases[] = {
	    {99, 29901, 7, 29993},    {100, 29900, 7, 29993},   {101, 29899, 7, 29993},
	    {200, 29800, 7, 29993},   {254, 29746, 7, 29993},   {255, 29745, 7, 29993},
	    {99, 29901, 0, 30000},    {101, 29899, 0, 30000},   {255, 29745, 0, 30000},
	    {101, 29899, 101, 29899}, {255, 29745, 255, 29745}, // same ply
	    {101, 29899, 254, 29746}, {7, 29993, 254, 29746},   // probed deeper than stored
	};
	// clang-format on
	for (const int sign : {1, -1}) {
		for (const auto& c : cases) {
			TranspositionTable tt(1);
			INFO("sign " << sign << " store_ply " << c.store_ply << " probe_ply " << c.probe_ply);
			do_store(tt, KEY_A, static_cast<int16_t>(sign * c.stored_value), 5, static_cast<int16_t>(c.store_ply));
			const auto result = tt.probe(KEY_A, c.probe_ply);
			REQUIRE(result.has_value());
			CHECK(result->value == sign * c.expected);
		}
	}
}

TEST_CASE("TT - a mate found deep from the root by a shallow node is rebased", "[tt]")
{
	// A node at ply 7 holding a mate 143 plies below it: root distance 150, score 29850. That is
	// beyond 100 plies yet a mate all the same, and must be stored as 30000 - 143 = 29857.
	for (const int sign : {1, -1}) {
		TranspositionTable tt(1);
		INFO("sign " << sign);
		do_store(tt, KEY_A, static_cast<int16_t>(sign * 29850), 5, /*ply=*/7);
		CHECK(tt.probe(KEY_A, 7)->value == sign * 29850);
		CHECK(tt.probe(KEY_A, 100)->value == sign * 29757);
		CHECK(tt.probe(KEY_A, 0)->value == sign * 29857);
	}
}

TEST_CASE("TT - a centipawn score just outside the mate band keeps its value", "[tt]")
{
	// 29743 is the largest finite score; 29744 is the first mate (distance 256 from the root).
	for (const int sign : {1, -1}) {
		TranspositionTable tt(1);
		INFO("sign " << sign);
		do_store(tt, KEY_A, static_cast<int16_t>(sign * 29743), 5, /*ply=*/150);
		do_store(tt, KEY_B, static_cast<int16_t>(sign * 29744), 5, /*ply=*/150);
		CHECK(tt.probe(KEY_A, 3)->value == sign * 29743);
		CHECK(tt.probe(KEY_A, 254)->value == sign * 29743);
		CHECK(tt.probe(KEY_B, 3)->value == sign * 29891); // 29744 + 150 - 3
	}
}

TEST_CASE("TT - short mates keep their distance across plies", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 29997, 5, /*ply=*/3); // mate 3 plies from the root, found at ply 3
	do_store(tt, KEY_B, -29990, 5, /*ply=*/10);
	CHECK(tt.probe(KEY_A, 1)->value == 29999);
	CHECK(tt.probe(KEY_A, 9)->value == 29991);
	CHECK(tt.probe(KEY_B, 4)->value == -29996);
}
