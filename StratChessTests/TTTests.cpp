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

// ── Same-key replacement ──────────────────────────────────────────────────────
//
// A store for a key already in the bucket is scored against the entry it would replace,
// by the same ranking that decides evictions, and a tie in that ranking is settled on the
// raw phase, depth and bound it quantises away. These pin what that buys: the two ways a
// same-key store used to destroy a main entry's hash move, measured at 21 of 197 PV nodes
// per #319, the ties the ranking alone would resolve the wrong way, and the cases that
// must still overwrite.

static const Move HASH_MOVE = Move(e2, e4, MoveFlags::QUIET);
static const Move OTHER_MOVE = Move(g1, f3, MoveFlags::QUIET);

TEST_CASE("TT - a same-key quiescence store does not displace a main entry", "[tt]")
{
	// The dominant failure this fixes: pvs() mines main entries for a hash move even when
	// they are too shallow to cut off, and refuses to mine a quiescence entry at all, so a
	// quiescence store landing on a PV node's key erased its move without evicting anything.
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 12, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	tt.store(KEY_A, 60, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE);

	// Every field, not just the two that name the failure: a store that wrote part of the
	// entry before deciding to decline would pass a narrower check.
	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->phase == SearchPhase::MAIN);
	CHECK(result->value == 500);
	CHECK(result->depth == 12);
	CHECK(result->bound == BoundType::EXACT);
	CHECK(result->node_type == NodeType::PV_NODE);
	CHECK(result->best_move == HASH_MOVE);
}

TEST_CASE("TT - a shallower same-key main store does not displace a deeper one", "[tt]")
{
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 12, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	tt.store(KEY_A, 60, 4, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->depth == 12);
	CHECK(result->value == 500);
	CHECK(result->bound == BoundType::EXACT);
	CHECK(result->node_type == NodeType::PV_NODE);
	CHECK(result->best_move == HASH_MOVE);
}

TEST_CASE("TT - on a same-key store the PV bonus is worth two plies of depth", "[tt]")
{
	// A consequence of reusing one ranking for both paths, pinned because it is the case a
	// reader gets wrong: the PV bonus is 512 and a ply is 256, so a deeper non-PV store has
	// to be two plies deeper to outrank a PV entry -- at exactly two the two score the same
	// 2560 and depth settles it. Declining the one-ply case costs a cutoff, never soundness,
	// and one generation of age (-512) cancels the bonus exactly.
	auto stored_then = [](int16_t incoming_depth) {
		TranspositionTable tt(0);
		tt.newSearchIteration();
		tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
		tt.store(KEY_A, 60, incoming_depth, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);
		return tt.probe(KEY_A, 0).value().value;
	};

	CHECK(stored_then(9) == 500); // one ply deeper: declined
	CHECK(stored_then(10) == 60); // two plies deeper: accepted
}

TEST_CASE("TT - an evicting store with no move does not inherit the evicted entry's move", "[tt]")
{
	// The move is carried forward only where the key matches and it therefore describes this
	// very position. Doing it on the eviction path would file a hint for one position under
	// another's key, which no probe could tell apart from a real one.
	TranspositionTable tt(0); // one bucket, so every key collides
	REQUIRE(tt.bucket_count() == 1);
	tt.newSearchIteration();

	// The weakest of the four, and the only one carrying a move.
	tt.store(KEY_A, 10, 1, 0, HASH_MOVE, BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);
	tt.store(KEY_B, 20, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);
	tt.store(KEY_MISS, 30, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);
	tt.store(KEY_A + 1, 40, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);

	// Bucket is full; this displaces the depth-1 entry.
	tt.store(KEY_A + 2, 50, 12, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);

	CHECK_FALSE(tt.probe(KEY_A, 0).has_value()); // it really was an eviction
	const auto result = tt.probe(KEY_A + 2, 0);
	REQUIRE(result.has_value());
	CHECK(result->best_move.is_null());
}

TEST_CASE("TT - the two replacementScore overloads agree", "[tt]")
{
	// store() ranks an entry that is not in the table yet against one that is, so the two
	// spellings of the ranking have to be the same function. Nothing else would notice them
	// drifting apart until a replacement decision started going the wrong way.
	TranspositionTable tt(1);

	for (const int16_t depth : {int16_t{-5}, int16_t{0}, int16_t{1}, int16_t{8}, int16_t{15}}) {
		for (const SearchPhase phase : {SearchPhase::MAIN, SearchPhase::QUIESCENCE}) {
			for (const NodeType type : {NodeType::ALL_NODE, NodeType::CUT_NODE, NodeType::PV_NODE}) {
				for (const int age_diff : {0, 1, 8}) {
					// entry_with() leaves age at 0, so probing at `age_diff` is that difference.
					const TTEntry stored = entry_with(depth, phase, type);
					INFO("depth " << depth << " phase " << static_cast<int>(phase) << " type " << static_cast<int>(type)
					              << " age_diff " << age_diff);
					CHECK(tt.replacementScore(stored, age_diff) == tt.replacementScore(depth, phase, type, age_diff));
				}
			}
		}
	}
}

TEST_CASE("TT - a same-key store at equal depth wins", "[tt]")
{
	// PVS re-searches the same node at the same depth with a wider window. The second
	// result is the one worth keeping, so a store that nothing separates from the entry it
	// lands on overwrites rather than being declined. Exactness is the one thing that
	// separates them, pinned below.
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::EXACT, NodeType::CUT_NODE, SearchPhase::MAIN);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->value == 60);
	CHECK(result->bound == BoundType::EXACT);
	CHECK(result->best_move == OTHER_MOVE);
}

TEST_CASE("TT - an accepted same-key store with no move keeps the stored hash move", "[tt]")
{
	// pvs()'s own null-move cutoff and terminal mate stores write Move::EmptyMove() at full
	// depth, so they outrank the entry they land on and overwrite it legitimately. The move
	// is a pure ordering hint produced for this same key, so carrying it forward costs
	// nothing and keeps the node's ordering.
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	tt.store(KEY_A, 60, 10, 0, no_move(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->value == 60); // the store was accepted
	CHECK(result->depth == 10);
	CHECK(result->best_move == HASH_MOVE); // but the move survived it
}

TEST_CASE("TT - an accepted same-key store with a move replaces the stored one", "[tt]")
{
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	tt.store(KEY_A, 60, 10, 0, OTHER_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->best_move == OTHER_MOVE);
}

TEST_CASE("TT - a declined same-key store leaves the counters alone", "[tt]")
{
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 12, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
	REQUIRE(tt.count_entries() == 1);
	REQUIRE(tt.count_pv_nodes() == 1);

	tt.store(KEY_A, 60, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE);

	CHECK(tt.count_entries() == 1);
	CHECK(tt.count_pv_nodes() == 1); // the PV entry is still there, so it is still counted
}

TEST_CASE("TT - a same-key store still wins once the stored entry is generations old", "[tt]")
{
	// Age is the axis allowed to override the phase ranking, on this path as on the eviction
	// path: an entry from several iterations ago is stale, and a same-key store is the
	// strongest evidence there is that the position is being searched again now.
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 4, 0, HASH_MOVE, BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN);
	for (int i = 0; i < 8; ++i)
		tt.newSearchIteration();

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

TEST_CASE("TT - a shallower same-key PV store does not displace a deeper entry", "[tt]")
{
	// The mirror of the case above, and the one the ranking cannot decide on its own: depth 8
	// PV and depth 10 non-PV both score 2560, so "at least as high" would hand the slot to the
	// shallower claim. Depth decides a tie, in both directions.
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 10, 0, HASH_MOVE, BoundType::EXACT, NodeType::CUT_NODE, SearchPhase::MAIN);
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->depth == 10);
	CHECK(result->value == 500);
	CHECK(result->node_type == NodeType::CUT_NODE);
}

TEST_CASE("TT - a shallower same-key quiescence store does not displace a deeper one", "[tt]")
{
	// The quiescence discount halves and truncates towards zero, so budgets 1, 0 and -1 all
	// rank at 0. quiescence() admits an entry on raw depth (`entry->depth >= qsearch_budget`),
	// so an equal rank overwriting would drop a budget-1 entry that the next budget-1 probe
	// needs and leave one only budget 0 can read.
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 1, 0, HASH_MOVE, BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE);
	tt.store(KEY_A, 60, 0, 0, OTHER_MOVE, BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->depth == 1);
	CHECK(result->value == 500);
}

TEST_CASE("TT - a same-key bound does not overwrite an exact score of the same depth", "[tt]")
{
	// The ranking does not look at the bound at all, so nothing but this stops a bound from
	// taking an exact entry's slot on freshness. probe() returns an exact score outright where
	// a bound only narrows the window, so the trade is never worth making.
	TranspositionTable tt(0);
	tt.newSearchIteration();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::EXACT, NodeType::CUT_NODE, SearchPhase::MAIN);
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->bound == BoundType::EXACT);
	CHECK(result->value == 500);

	// Deeper still wins: exactness settles a tie, it does not outrank search.
	tt.store(KEY_A, 60, 9, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN);
	CHECK(tt.probe(KEY_A, 0).value().value == 60);
}
