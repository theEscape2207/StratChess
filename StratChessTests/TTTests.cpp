// TTTests.cpp — Catch2 unit tests for TranspositionTable
//
// Tests the TT in complete isolation — no Board, no engine startup required.
// Covers store/probe round-trips, same-key overwrite, mate-score normalization,
// clear(), and the atomic diagnostic counters (entry_count, pv_count).
//
// See Docs/TestDesign.md §Phase 0 for the rationale.

#include <catch_amalgamated.hpp>
#include "TranspositionTable.h"
#include "defines.h"

// ── Helpers ───────────────────────────────────────────────────────────────────

static Move no_move() { return Move::EmptyMove(); }

// Convenience wrapper: stores a value at the given key with sensible defaults.
static void do_store(TranspositionTable& tt, uint64_t key, int16_t value, int16_t depth = 5, int16_t ply = 0)
{
	tt.store(key, value, depth, ply, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
}

static constexpr uint64_t KEY_A = 1'000'001;
static constexpr uint64_t KEY_B = 2'000'002;
static constexpr uint64_t KEY_MISS = 9'876'543;

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

TEST_CASE("TT - entry_count increments when a new key is stored", "[tt]")
{
	TranspositionTable tt(1);
	const size_t before = tt.count_entries();

	do_store(tt, KEY_A, 100);

	REQUIRE(tt.count_entries() == before + 1);
}

TEST_CASE("TT - entry_count does not increment when overwriting the same key", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 100);
	const size_t after_first = tt.count_entries();

	do_store(tt, KEY_A, 200); // overwrite — same slot, not new entry

	REQUIRE(tt.count_entries() == after_first);
}

TEST_CASE("TT - pv_count tracks PV_NODE entries only", "[tt]")
{
	TranspositionTable tt(1);
	const size_t before = tt.count_pv_nodes();

	// PV node — should increment pv_count
	tt.store(KEY_A, 100, 5, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	REQUIRE(tt.count_pv_nodes() == before + 1);

	// CUT node — should NOT increment pv_count
	tt.store(KEY_B, 200, 5, 0, no_move(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);
	REQUIRE(tt.count_pv_nodes() == before + 1);
}

TEST_CASE("TT - clear resets entry_count and pv_count to zero", "[tt]")
{
	TranspositionTable tt(1);
	do_store(tt, KEY_A, 100);
	do_store(tt, KEY_B, 200);

	REQUIRE(tt.clear());

	REQUIRE(tt.count_entries() == 0);
	REQUIRE(tt.count_pv_nodes() == 0);
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

// ── Allocation reporting ──────────────────────────────────────────────────────
//
// The bucket count is rounded down to a power of two, so the table is generally
// smaller than the megabytes requested. These pin the two properties that make
// that acceptable: the diagnostic must describe the allocation rather than the
// request, and the allocation must never exceed the request.

TEST_CASE("TT - memory_mb reports what was allocated, not what was requested", "[tt]")
{
	// 256 MiB / 96-byte buckets = 2796202, floored to 2^21 = 2097152 buckets,
	// which is 192 MiB of entries. Reporting 256 here would hide a 25% shortfall.
	TranspositionTable tt(256);

	REQUIRE(tt.requested_memory_mb() == 256);
	REQUIRE(tt.bucket_count() == 2097152u);
	REQUIRE(tt.memory_mb() == 192);
}

TEST_CASE("TT - allocation never exceeds the requested budget", "[tt]")
{
	// The argument is a memory cap, so rounding must not overshoot it. Sizes
	// either side of a power-of-two boundary, where round-to-nearest would.
	for (const size_t requested :
	     {size_t{1}, size_t{7}, size_t{64}, size_t{100}, size_t{256}, size_t{300}, size_t{511}}) {
		TranspositionTable tt(requested);

		INFO("requested " << requested << " MiB");
		REQUIRE(tt.requested_memory_mb() == requested);
		REQUIRE(tt.memory_mb() <= requested);
	}
}

TEST_CASE("TT - byte accounting is self-consistent", "[tt]")
{
	TranspositionTable tt(16);

	REQUIRE(tt.bucket_count() > 0);
	REQUIRE(tt.entry_bytes() == tt.bucket_count() * 96); // 4 x 24-byte entries
	REQUIRE(tt.lock_bytes() == tt.bucket_count() * sizeof(std::shared_mutex));
	REQUIRE(tt.allocated_bytes() == tt.entry_bytes() + tt.lock_bytes());
	REQUIRE(tt.memory_mb() == tt.entry_bytes() / (size_t{1024} * 1024));
}

TEST_CASE("TT - bucket count is a power of two", "[tt]")
{
	// probe/store index with `key & index_mask`, which is only equivalent to a
	// modulo when the bucket count is a power of two.
	for (const size_t requested : {size_t{1}, size_t{5}, size_t{16}, size_t{100}, size_t{256}}) {
		TranspositionTable tt(requested);
		const size_t n = tt.bucket_count();

		INFO("requested " << requested << " MiB, got " << n << " buckets");
		REQUIRE(n > 0);
		REQUIRE((n & (n - 1)) == 0);
	}
}

// ── Replacement ranking across phases ─────────────────────────────────────────
//
// entry.depth is remaining search in both phases, so replacement can rank the two
// with one arithmetic — a quiescence budget discounted onto the main-search scale.
// These pin the discount, because it decides how much shallow main-search content
// quiescence entries are allowed to evict, and nothing else would notice it moving.

static TTEntry entry_with(int16_t depth, SearchPhase phase, NodeType node_type = NodeType::ALL_NODE)
{
	TTEntry e;
	e.key = KEY_A;
	e.depth = depth;
	e.phase = phase;
	e.node_type = node_type;
	return e;
}

TEST_CASE("TT - a quiescence entry that kept more budget outranks one that kept less", "[tt]")
{
	TranspositionTable tt(1);
	constexpr int age = 0;

	const int full = tt.replacementScore(entry_with(15, SearchPhase::QUIESCENCE), age);
	const int spent = tt.replacementScore(entry_with(0, SearchPhase::QUIESCENCE), age);
	const int exhausted = tt.replacementScore(entry_with(-5, SearchPhase::QUIESCENCE), age);

	CHECK(full > spent);
	CHECK(spent >= exhausted);
}

TEST_CASE("TT - even a full-budget quiescence entry ranks below every main entry", "[tt]")
{
	// The phase penalty exists to make this true at the same age. Without it the discount
	// alone puts a full-budget quiescence entry at main-search depth 7, where it evicts the
	// shallow main entries that supply pvs() with hash moves — worth ~35% more nodes on the
	// bench suite at a 16 MB table, and nothing at all at 192 MB.
	//
	// Every node type is checked because the PV bonus is added after the discount: a
	// PV_NODE quiescence entry is the one that comes closest to escaping the band.
	TranspositionTable tt(1);
	constexpr int age = 0;

	const int weakest_main = tt.replacementScore(entry_with(0, SearchPhase::MAIN, NodeType::ALL_NODE), age);

	for (const NodeType type : {NodeType::ALL_NODE, NodeType::CUT_NODE, NodeType::PV_NODE}) {
		const int fresh_qsearch = tt.replacementScore(entry_with(15, SearchPhase::QUIESCENCE, type), age);
		INFO("node type " << static_cast<int>(type));
		CHECK(fresh_qsearch < weakest_main);
	}
}

TEST_CASE("TT - an empty slot is filled before any occupied entry is evicted", "[tt]")
{
	// store() picks the lowest-scoring slot, and the quiescence phase penalty puts an
	// exhausted quiescence entry below what an empty slot scores. Without an explicit
	// preference for empty slots, that entry is evicted while three slots stay empty, and the
	// bucket's effective associativity silently drops to one.
	TranspositionTable tt(0); // one bucket, so every key collides
	REQUIRE(tt.bucket_count() == 1);

	tt.newSearchIteration(); // matches the search: age is never 0 at store time

	// An exhausted quiescence entry: the most evictable thing the engine can produce.
	tt.store(KEY_A, 10, /*depth=*/-5, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE);

	// Three more distinct keys. A four-entry bucket holds all four without evicting anything.
	tt.store(KEY_B, 20, 4, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	tt.store(KEY_MISS, 30, 6, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	tt.store(KEY_A + 1, 40, 8, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);

	CHECK(tt.probe(KEY_A, 0).has_value());
	CHECK(tt.probe(KEY_B, 0).has_value());
	CHECK(tt.probe(KEY_MISS, 0).has_value());
	CHECK(tt.probe(KEY_A + 1, 0).has_value());
}

TEST_CASE("TT - a quiescence store evicts the weakest main entry, not an arbitrary one", "[tt]")
{
	// The phase ranking pinned above, driven through store() rather than read off
	// replacementScore(): the scoring function is only worth anything if the storage path
	// actually consults it, and only a full bucket forces it to choose.
	TranspositionTable tt(0);
	REQUIRE(tt.bucket_count() == 1);

	tt.newSearchIteration();

	const uint64_t deep = KEY_A;
	const uint64_t middling = KEY_B;
	const uint64_t shallow = KEY_MISS;
	const uint64_t shallowest = KEY_A + 1;

	tt.store(deep, 10, 12, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);
	tt.store(middling, 20, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);
	tt.store(shallow, 30, 4, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);
	tt.store(shallowest, 40, 1, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);

	// Bucket is full; this one has to displace something.
	tt.store(KEY_A + 2, 50, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE);

	CHECK_FALSE(tt.probe(shallowest, 0).has_value());
	CHECK(tt.probe(deep, 0).has_value());
	CHECK(tt.probe(middling, 0).has_value());
	CHECK(tt.probe(shallow, 0).has_value());
}

TEST_CASE("TT - a same-key store overwrites regardless of phase", "[tt]")
{
	// Pre-existing behaviour, pinned because it is the one route by which a quiescence
	// result can still destroy a main-search entry and its hash move: the key match short
	// circuits before any scoring. Whether that is right is not this change's question.
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 12, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	tt.store(KEY_A, 60, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->phase == SearchPhase::QUIESCENCE);
	CHECK(result->value == 60);
}

TEST_CASE("TT - a newer quiescence entry still displaces an aged main entry", "[tt]")
{
	// The penalty must not make quiescence entries immortal in the other direction: a main
	// entry several generations old is stale, and age is the axis that is allowed to override
	// the phase ranking.
	TranspositionTable tt(1);

	const int fresh_qsearch = tt.replacementScore(entry_with(15, SearchPhase::QUIESCENCE), /*age=*/0);
	const int aged_main = tt.replacementScore(entry_with(4, SearchPhase::MAIN), /*age=*/8);

	CHECK(fresh_qsearch > aged_main);
}

TEST_CASE("TT - the quiescence depth discount is monotone across zero", "[tt]")
{
	// Truncation toward zero flattens -1, 0 and 1 onto 0; that is harmless. An ordering
	// inversion would not be — it would make an exhausted entry outrank a fresh one.
	TranspositionTable tt(1);

	int previous = tt.quiescenceEquivalentDepth(-20);
	for (int budget = -19; budget <= 20; ++budget) {
		const int current = tt.quiescenceEquivalentDepth(budget);
		INFO("budget " << budget);
		REQUIRE(current >= previous);
		previous = current;
	}
}

TEST_CASE("TT - a request too small for one bucket still allocates one", "[tt]")
{
	// 0 MiB would compute zero buckets; the table must stay usable rather than
	// producing an index_mask of SIZE_MAX.
	TranspositionTable tt(0);

	REQUIRE(tt.bucket_count() == 1);
	REQUIRE(tt.memory_mb() == 0);

	do_store(tt, KEY_A, 123);
	auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	REQUIRE(result->value == 123);
}
