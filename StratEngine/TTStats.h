#pragma once
#include <cstdint>

// Transposition-table probe/store counters. MEASUREMENT ONLY: they change no search or
// replacement decision, so a stats build must stay node-identical to the shipping one.
//
// Set by CMake: -DSTRAT_TT_STATS=1. At 0 the engine executes no counting code; the counters remain
// as cold members of ThreadData and SearchResult. Every use is `if constexpr`, never #ifdef: a
// discarded branch in non-template code is still type-checked, and that needs the members to exist.
//
// Read them from a workload that runs past the opening -- a self-play game or one long `go movetime`
// from a middlegame. A per-position bench barely fills the table, so every Hash size reads alike.
#ifndef STRAT_TT_STATS
#	define STRAT_TT_STATS 0
#endif
inline constexpr bool kTTStatsCompiled = STRAT_TT_STATS != 0;

// What TranspositionTable::store() did. Returned rather than counted inside the table, which has no
// per-thread state: the caller records it into its own ThreadData. The shipping build ignores it.
enum class TTStoreOutcome : uint8_t {
	Declined,  // same key, and the stored entry outranked the incoming one
	Filled,    // took an empty slot
	Refreshed, // overwrote the entry holding the same key
	Evicted,   // overwrote a different key; in a stats build, one written before this search began
	// Overwrote a different key written since this search began -- the hashfull() window. Only a
	// stats build tells the two evictions apart; the shipping build reports every one as Evicted.
	EvictedCurrentSearch,
};

struct TTStats {
	// A hit is a key match, whatever the entry's phase or depth; a cutoff is a hit that returned.
	int64_t main_probes = 0;
	int64_t main_hits = 0;
	int64_t main_cutoffs = 0;
	int64_t qs_probes = 0;
	int64_t qs_hits = 0;
	int64_t qs_cutoffs = 0;

	int64_t stores_declined = 0;
	int64_t stores_filled = 0;
	int64_t stores_refreshed = 0;
	int64_t evicted_stale = 0;
	int64_t evicted_current = 0;

	void record(TTStoreOutcome outcome) noexcept
	{
		switch (outcome) {
		case TTStoreOutcome::Declined:
			++stores_declined;
			break;
		case TTStoreOutcome::Filled:
			++stores_filled;
			break;
		case TTStoreOutcome::Refreshed:
			++stores_refreshed;
			break;
		case TTStoreOutcome::Evicted:
			++evicted_stale;
			break;
		case TTStoreOutcome::EvictedCurrentSearch:
			++evicted_current;
			break;
		}
	}

	int64_t stores() const noexcept
	{
		return stores_declined + stores_filled + stores_refreshed + evicted_stale + evicted_current;
	}

	void add(const TTStats& other) noexcept
	{
		main_probes += other.main_probes;
		main_hits += other.main_hits;
		main_cutoffs += other.main_cutoffs;
		qs_probes += other.qs_probes;
		qs_hits += other.qs_hits;
		qs_cutoffs += other.qs_cutoffs;
		stores_declined += other.stores_declined;
		stores_filled += other.stores_filled;
		stores_refreshed += other.stores_refreshed;
		evicted_stale += other.evicted_stale;
		evicted_current += other.evicted_current;
	}
};
