// TTReplacementTests.cpp — Catch2 unit tests for TranspositionTable: replacement ranking across phases and same-key store rules.
//
// See Docs/TestDesign.md for the rationale.

#include <catch2/catch_test_macros.hpp>
#include "TTTestFixture.h"

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

	tt.newSearch(); // matches the search: age is never 0 at store time

	// An exhausted quiescence entry: the most evictable thing the engine can produce.
	tt.store(KEY_A, 10, /*depth=*/-5, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE,
	         kNoStaticEval);

	// Three more distinct keys. A four-entry bucket holds all four without evicting anything.
	tt.store(KEY_B, 20, 4, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_MISS, 30, 6, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_C, 40, 8, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);

	CHECK(tt.probe(KEY_A, 0).has_value());
	CHECK(tt.probe(KEY_B, 0).has_value());
	CHECK(tt.probe(KEY_MISS, 0).has_value());
	CHECK(tt.probe(KEY_C, 0).has_value());
}

TEST_CASE("TT - a quiescence store evicts the weakest main entry, not an arbitrary one", "[tt]")
{
	// The phase ranking pinned above, driven through store() rather than read off
	// replacementScore(): the scoring function is only worth anything if the storage path
	// actually consults it, and only a full bucket forces it to choose.
	TranspositionTable tt(0);
	REQUIRE(tt.bucket_count() == 1);

	tt.newSearch();

	const uint64_t deep = KEY_A;
	const uint64_t middling = KEY_B;
	const uint64_t shallow = KEY_MISS;
	const uint64_t shallowest = KEY_C;

	tt.store(deep, 10, 12, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(middling, 20, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(shallow, 30, 4, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(shallowest, 40, 1, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);

	// Bucket is full; this one has to displace something.
	tt.store(KEY_D, 50, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE, kNoStaticEval);

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
// same-key store could destroy a main entry's hash move, the ties the ranking alone would
// resolve the wrong way, and the cases that must still overwrite.

static const Move HASH_MOVE = Move(e2, e4, MoveFlags::QUIET);
static const Move OTHER_MOVE = Move(g1, f3, MoveFlags::QUIET);

TEST_CASE("TT - a same-key quiescence store does not displace a main entry", "[tt]")
{
	// The dominant failure this fixes: pvs() mines main entries for a hash move even when
	// they are too shallow to cut off, and refuses to mine a quiescence entry at all, so a
	// quiescence store landing on a PV node's key erased its move without evicting anything.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 12, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_A, 60, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE, kNoStaticEval);

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
	tt.newSearch();

	tt.store(KEY_A, 500, 12, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_A, 60, 4, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->depth == 12);
	CHECK(result->value == 500);
	CHECK(result->bound == BoundType::EXACT);
	CHECK(result->node_type == NodeType::PV_NODE);
	CHECK(result->best_move == HASH_MOVE);
}

TEST_CASE("TT - a deeper same-key store displaces a PV entry within one search", "[tt]")
{
	// The PV bonus (512, two plies) ranks which of two positions to keep; it is no claim that a
	// shallower search of one position beats a deeper one. Every depth of a search shares one
	// age, so nothing else would let a node that left the PV, or failed high under aspiration,
	// replace its own shallower PV entry and hash move until the next search.
	auto stored_then = [](SearchPhase phase, int16_t stored_depth, int16_t incoming_depth, Move incoming_move) {
		TranspositionTable tt(0);
		tt.newSearch();
		tt.store(KEY_A, 500, stored_depth, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, phase, kNoStaticEval);
		tt.store(KEY_A, 60, incoming_depth, 0, incoming_move, BoundType::LOWER, NodeType::CUT_NODE, phase,
		         kNoStaticEval);
		return tt.probe(KEY_A, 0).value();
	};

	const TTEntry deeper = stored_then(SearchPhase::MAIN, 8, 9, OTHER_MOVE);
	CHECK(deeper.value == 60);
	CHECK(deeper.best_move == OTHER_MOVE);
	// A deeper store with no move (a null-move cutoff) still keeps the PV entry's move as its hint.
	const TTEntry moveless = stored_then(SearchPhase::MAIN, 8, 9, no_move());
	CHECK(moveless.value == 60);
	CHECK(moveless.best_move == HASH_MOVE);

	// Quiescence budgets, including the negative ones the halving rounds onto one rank.
	CHECK(stored_then(SearchPhase::QUIESCENCE, 8, 9, no_move()).value == 60);
	CHECK(stored_then(SearchPhase::QUIESCENCE, -1, 0, no_move()).value == 60);
	CHECK(stored_then(SearchPhase::QUIESCENCE, -3, -2, no_move()).value == 60);

	// Equal depth is not a deeper claim: the ranking and the exactness step still keep the PV score.
	CHECK(stored_then(SearchPhase::MAIN, 8, 8, OTHER_MOVE).value == 500);
}

TEST_CASE("TT - an evicting store with no move does not inherit the evicted entry's move", "[tt]")
{
	// The move is carried forward only where the key matches and it therefore describes this
	// very position. Doing it on the eviction path would file a hint for one position under
	// another's key, which no probe could tell apart from a real one.
	TranspositionTable tt(0); // one bucket, so every key collides
	REQUIRE(tt.bucket_count() == 1);
	tt.newSearch();

	// The weakest of the four, and the only one carrying a move.
	tt.store(KEY_A, 10, 1, 0, HASH_MOVE, BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_B, 20, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_MISS, 30, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_C, 40, 8, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);

	// Bucket is full; this displaces the depth-1 entry.
	tt.store(KEY_D, 50, 12, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);

	CHECK_FALSE(tt.probe(KEY_A, 0).has_value()); // it really was an eviction
	const auto result = tt.probe(KEY_D, 0);
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
	// PVS re-searches the same node at the same depth with a wider window, and the second
	// result is the one worth keeping. This pair is settled by exactness rather than by the
	// terminal overwrite -- the re-search upgrades a bound to an exact score, which is the
	// direction exactness allows. The terminal itself is pinned separately, below.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::EXACT, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->value == 60);
	CHECK(result->bound == BoundType::EXACT);
	CHECK(result->best_move == OTHER_MOVE);
}

TEST_CASE("TT - an accepted same-key store with no move keeps the stored hash move", "[tt]")
{
	// pvs()'s own null-move cutoff and terminal mate stores write Move::EmptyMove() at full
	// depth, so they win the slot on their own merits and erase the hash move legitimately --
	// here on depth, two plies of it against the PV bonus's 512. The move is a pure ordering
	// hint produced for this same key, so carrying it forward costs nothing and keeps the
	// node's ordering.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_A, 60, 10, 0, no_move(), BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->value == 60); // the store was accepted
	CHECK(result->depth == 10);
	CHECK(result->best_move == HASH_MOVE); // but the move survived it
}

TEST_CASE("TT - an accepted same-key store with a move replaces the stored one", "[tt]")
{
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_A, 60, 10, 0, OTHER_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->best_move == OTHER_MOVE);
}

TEST_CASE("TT - a declined same-key store leaves the stored entry whole", "[tt]")
{
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 12, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
	REQUIRE(tt.store(KEY_A, 60, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE,
	                 kNoStaticEval) == TTStoreOutcome::Declined);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->value == 500);
	CHECK(result->depth == 12);
	CHECK(result->phase == SearchPhase::MAIN);
	CHECK(result->node_type == NodeType::PV_NODE);
	CHECK(result->best_move == HASH_MOVE);
}

TEST_CASE("TT - a same-key store still wins once the stored entry is generations old", "[tt]")
{
	// Age is the axis allowed to override the phase ranking, on this path as on the eviction
	// path: an entry from several searches ago is stale, and a same-key store is the
	// strongest evidence there is that the position is being searched again now.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 4, 0, HASH_MOVE, BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	for (int i = 0; i < 8; ++i)
		tt.newSearch();

	tt.store(KEY_A, 60, 15, 0, no_move(), BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE, kNoStaticEval);

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

TEST_CASE("TT - a shallower same-key PV store does not displace a deeper entry", "[tt]")
{
	// The mirror of the case above, and the one the ranking cannot decide on its own: depth 8
	// PV and depth 10 non-PV both score 2560, so "at least as high" would hand the slot to the
	// shallower claim. Depth decides a tie, in both directions.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 10, 0, HASH_MOVE, BoundType::EXACT, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);

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
	// Negative budgets are reachable, not hypothetical: quiescence() keeps going below zero
	// while in check, so every pairing of the three is a position the search can present.
	auto kept_depth = [](int16_t stored_depth, int16_t incoming_depth) {
		TranspositionTable tt(0);
		tt.newSearch();
		tt.store(KEY_A, 500, stored_depth, 0, HASH_MOVE, BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::QUIESCENCE,
		         kNoStaticEval);
		tt.store(KEY_A, 60, incoming_depth, 0, OTHER_MOVE, BoundType::EXACT, NodeType::ALL_NODE,
		         SearchPhase::QUIESCENCE, kNoStaticEval);
		return tt.probe(KEY_A, 0).value().depth;
	};

	CHECK(kept_depth(1, 0) == 1);
	CHECK(kept_depth(1, -1) == 1);
	CHECK(kept_depth(0, -1) == 0);
	CHECK(kept_depth(-1, 0) == 0); // and the deeper budget still takes the slot
	CHECK(kept_depth(0, 1) == 1);
}

TEST_CASE("TT - a same-key bound does not overwrite an exact score of the same depth", "[tt]")
{
	// The ranking does not look at the bound at all, so nothing but this stops a bound from
	// taking an exact entry's slot on freshness. probe() returns an exact score outright where
	// a bound only narrows the window, so the trade is never worth making.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::EXACT, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->bound == BoundType::EXACT);
	CHECK(result->value == 500);

	// Deeper still wins: exactness settles a tie, it does not outrank search.
	tt.store(KEY_A, 60, 9, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);
	CHECK(tt.probe(KEY_A, 0).value().value == 60);
}

TEST_CASE("TT - a same-key store that nothing separates from the stored entry wins", "[tt]")
{
	// The terminal case, and the one every other same-key test now exits before reaching: the
	// aspiration retries. newSearch() runs once per search, not once per attempt, so a
	// root that fails high twice stores the same depth, phase, node type and bound under the
	// same age. Declining the second would freeze the entry on the first, narrower window's
	// value for the rest of the iteration.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->value == 60);
	CHECK(result->best_move == OTHER_MOVE);
}

TEST_CASE("TT - a tie across phases leaves the main entry in the slot", "[tt]")
{
	// The step that guards the hash move pvs() mines, pinned with the one input that reaches
	// it: the scores meet across phases only once age is involved. A two-generation-old MAIN
	// depth 1 entry scores 256 - 1024, and a quiescence budget 10 marked PV_NODE scores
	// 5 * 256 + 512 - 2560. Both -768, and quiescence() cannot serve pvs() a move.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 1, 0, HASH_MOVE, BoundType::EXACT, NodeType::ALL_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.newSearch();
	tt.newSearch();

	tt.store(KEY_A, 60, 10, 0, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::QUIESCENCE, kNoStaticEval);

	const auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->phase == SearchPhase::MAIN);
	CHECK(result->value == 500);
	CHECK(result->best_move == HASH_MOVE);
}

TEST_CASE("TT - a same-key bound does not downgrade the previous search's exact score", "[tt]")
{
	// The reachable form of the exactness step, and the one trade this policy makes. In the
	// main search an exact score means a PV node -- a null window cannot produce one -- so the
	// PV bonus and one generation of age cancel exactly and the two meet at 2048. The previous
	// search's exact value is kept for one more generation.
	TranspositionTable tt(0);
	tt.newSearch();

	tt.store(KEY_A, 500, 8, 0, HASH_MOVE, BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
	tt.newSearch();
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);

	auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->bound == BoundType::EXACT);
	CHECK(result->value == 500);

	// The stickiness is bounded to that one generation: at two the stored score falls to
	// 2048 - 512 and the bound wins outright, on the score rather than on any tie-break.
	tt.newSearch();
	tt.store(KEY_A, 60, 8, 0, OTHER_MOVE, BoundType::LOWER, NodeType::CUT_NODE, SearchPhase::MAIN, kNoStaticEval);

	result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	CHECK(result->bound == BoundType::LOWER);
	CHECK(result->value == 60);
}
