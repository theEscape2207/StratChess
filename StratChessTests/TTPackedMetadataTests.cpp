// TTPackedMetadataTests.cpp — Catch2 unit tests for TranspositionTable: the packed entry metadata, age wrap, hashfull and the store outcome.
//
// See Docs/TestDesign.md for the rationale.

#include <catch2/catch_test_macros.hpp>
#include "TTTestFixture.h"

// ── Packed storage ────────────────────────────────────────────────────────────

TEST_CASE("TT - packed metadata represents every valid enum combination", "[tt]")
{
	using Entry = TranspositionTableTestFixture::Entry;

	for (const SearchPhase phase : {SearchPhase::MAIN, SearchPhase::QUIESCENCE}) {
		for (const BoundType bound : {BoundType::EXACT, BoundType::LOWER, BoundType::UPPER}) {
			for (const NodeType node_type : {NodeType::PV_NODE, NodeType::CUT_NODE, NodeType::ALL_NODE}) {
				Entry entry;
				entry.set_phase(phase);
				entry.set_bound(bound);
				entry.set_node_type(node_type);

				INFO("phase " << static_cast<int>(phase) << " bound " << static_cast<int>(bound) << " node "
				              << static_cast<int>(node_type));
				CHECK(entry.phase() == phase);
				CHECK(entry.bound() == bound);
				CHECK(entry.node_type() == node_type);
				const TTEntry unpacked = entry.unpack();
				CHECK(unpacked.phase == phase);
				CHECK(unpacked.bound == bound);
				CHECK(unpacked.node_type == node_type);
			}
		}
	}
}

TEST_CASE("TT - packed metadata setters preserve unrelated and reserved bits", "[tt]")
{
	using Entry = TranspositionTableTestFixture::Entry;
	Entry entry;
	entry.metadata = 0xF8;

	const uint8_t before_phase = entry.metadata;
	entry.set_phase(SearchPhase::QUIESCENCE);
	CHECK((entry.metadata & ~Entry::PHASE_MASK) == (before_phase & ~Entry::PHASE_MASK));

	const uint8_t before_bound = entry.metadata;
	entry.set_bound(BoundType::LOWER);
	CHECK((entry.metadata & ~Entry::BOUND_MASK) == (before_bound & ~Entry::BOUND_MASK));

	const uint8_t before_node = entry.metadata;
	entry.set_node_type(NodeType::PV_NODE);
	CHECK((entry.metadata & ~Entry::NODE_MASK) == (before_node & ~Entry::NODE_MASK));
}

TEST_CASE("TT - packed entry defaults and table clear reset reserved metadata", "[tt]")
{
	using Entry = TranspositionTableTestFixture::Entry;
	const Entry initial;
	CHECK(initial.empty());
	CHECK(initial.static_eval() == kNoStaticEval);
	CHECK(initial.value == 0);
	CHECK(initial.depth == 0);
	CHECK(initial.best_move.is_null());
	CHECK(initial.metadata == 0x10);
	CHECK(initial.age == 0);

	TranspositionTable tt(0);
	const Entry fresh = TranspositionTableTestFixture::first_entry(tt);
	CHECK(fresh.empty());
	CHECK(fresh.static_eval() == kNoStaticEval);
	CHECK(fresh.value == 0);
	CHECK(fresh.depth == 0);
	CHECK(fresh.best_move.is_null());
	CHECK(fresh.metadata == 0x10);
	CHECK(fresh.age == 0);

	tt.newSearch();
	tt.store(KEY_A, 123, 4, 0, Move(e2, e4, MoveFlags::QUIET), BoundType::UPPER, NodeType::PV_NODE,
	         SearchPhase::QUIESCENCE, kNoStaticEval);
	REQUIRE(tt.clear());

	const Entry cleared = TranspositionTableTestFixture::first_entry(tt);
	CHECK(cleared.empty());
	CHECK(cleared.static_eval() == kNoStaticEval);
	CHECK(cleared.value == 0);
	CHECK(cleared.depth == 0);
	CHECK(cleared.best_move.is_null());
	CHECK(cleared.metadata == 0x10);
	CHECK(cleared.age == 0);
}

TEST_CASE("TT - packed entry preserves every age and wraps from 255 to 0", "[tt]")
{
	using Entry = TranspositionTableTestFixture::Entry;
	Entry entry;
	for (unsigned age = 0; age < 256; ++age) {
		entry.age = static_cast<uint8_t>(age);
		INFO("age " << age);
		CHECK(entry.unpack().age == age);
	}

	TranspositionTable tt(0);
	for (int search = 0; search < 255; ++search)
		tt.newSearch();
	tt.store(KEY_A, 1, 1, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	REQUIRE(tt.probe(KEY_A, 0).has_value());
	CHECK(tt.probe(KEY_A, 0)->age == 255);

	tt.newSearch();
	tt.store(KEY_B, 2, 1, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	REQUIRE(tt.probe(KEY_B, 0).has_value());
	CHECK(tt.probe(KEY_B, 0)->age == 0);
}

TEST_CASE("TT - hashfull counts the current search's generation and not the previous one", "[tt]")
{
	// Fills one quarter of the fixed sample the way Search() brackets a search: snapshot the age,
	// then advance it once. Each key maps to its bucket and carries a nonzero key48.
	const auto hashfull_after_search = [](int searches_before) {
		TranspositionTable tt(1);
		for (int i = 0; i < searches_before; ++i)
			tt.newSearch();
		const uint8_t search_start_age = tt.currentAge();
		CHECK(tt.hashfull(search_start_age) == 0);
		tt.newSearch();
		for (uint64_t bucket = 0; bucket < 250; ++bucket)
			do_store(tt, bucket | (uint64_t{1} << 32), 1);
		CHECK(tt.hashfull(search_start_age) == 250);

		// The same entries are stale from the next search's starting age.
		const uint8_t next_search_start_age = tt.currentAge();
		tt.newSearch();
		CHECK(tt.hashfull(next_search_start_age) == 0);
	};

	SECTION("plain boundary") { hashfull_after_search(5); }
	SECTION("search that wraps 255 -> 0") { hashfull_after_search(255); }
}

TEST_CASE("TT - store reports its outcome, splitting evictions at the search start", "[tt]")
{
	// The test binary compiles the stats in; without them EvictedCurrentSearch is never reported.
	REQUIRE(kTTStatsCompiled);

	TranspositionTable tt(0); // one bucket: every key competes for the same four slots
	const auto store_at_depth = [&tt](uint64_t key, int16_t depth) {
		return tt.store(key << 16, 1, depth, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN,
		                kNoStaticEval);
	};

	tt.setStatsSearchStartAge(tt.currentAge());
	tt.newSearch();
	CHECK(store_at_depth(1, 4) == TTStoreOutcome::Filled);
	CHECK(store_at_depth(1, 6) == TTStoreOutcome::Refreshed);
	CHECK(store_at_depth(1, 2) == TTStoreOutcome::Declined);
	for (uint64_t key = 2; key <= 4; ++key)
		CHECK(store_at_depth(key, 4) == TTStoreOutcome::Filled);
	CHECK(store_at_depth(5, 8) == TTStoreOutcome::EvictedCurrentSearch);

	// Everything in the bucket now predates the next search.
	tt.setStatsSearchStartAge(tt.currentAge());
	tt.newSearch();
	CHECK(store_at_depth(6, 8) == TTStoreOutcome::Evicted);
}

TEST_CASE("TT - an age wrap does not change same-key replacement", "[tt]")
{
	// The ranking compares ages modulo 256. If that arithmetic were not modular, the search that
	// wraps 255 -> 0 would make the incumbent look fresher than the store landing on it. Run the same
	// scenario at a plain boundary and at the wrap, and require the two to agree.
	auto same_key_restore_wins = [](int searches_before) {
		TranspositionTable tt(0);
		for (int i = 0; i < searches_before; ++i)
			tt.newSearch();
		tt.store(KEY_A, 1, 4, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
		tt.newSearch();
		tt.store(KEY_A, 42, 4, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
		const auto result = tt.probe(KEY_A, 0);
		REQUIRE(result.has_value());
		return result->value == 42;
	};

	CHECK(same_key_restore_wins(5));
	CHECK(same_key_restore_wins(255) == same_key_restore_wins(5));
}

TEST_CASE("TT - a store carries all three packed metadata fields through the table", "[tt]")
{
	// The setters are exercised directly elsewhere; this pins that store() writes and probe() reads
	// all three fields at once, so a mask overlapping a neighbour cannot pass unnoticed.
	TranspositionTable tt(0);
	tt.store(KEY_A, 17, 3, 0, no_move(), BoundType::UPPER, NodeType::CUT_NODE, SearchPhase::QUIESCENCE, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->bound == BoundType::UPPER);
	CHECK(result->node_type == NodeType::CUT_NODE);
	CHECK(result->phase == SearchPhase::QUIESCENCE);
}
