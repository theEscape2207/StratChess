#pragma once
#include <vector>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <limits>
#include <optional>
#include <cstdint>
#include <memory>
#include <cassert>
#include "Move.h"

enum class BoundType : uint8_t { EXACT = 0, LOWER = 1, UPPER = 2 };

// Node types
// PV_NODE: principal variation node
// CUT_NODE: beta cutoff expected
// ALL_NODE: all moves must be searched
enum class NodeType : uint8_t {
	PV_NODE = 0,  // Principal variation node
	CUT_NODE = 1, // Beta cutoff expected
	ALL_NODE = 2  // All moves must be searched
};

// Search phases
// MAIN: regular search
// QUIESCENCE: quiescence search
enum class SearchPhase : uint8_t { MAIN, QUIESCENCE };

// Transposotion Table Entries
// Stores key, value, depth, best move, bound type, node type, age
// Uses 64-bit keys and 16-bit values/depths for compactness
// Age is used for replacement strategy
// NodeType is stored to improve replacement decisions
// Best move is stored for move ordering
struct TTEntry {
	std::uint64_t key;
	int16_t value;
	int16_t depth; // Search depth or equivalent
	SearchPhase phase;
	BoundType bound;
	NodeType node_type; // Track node type for better replacement
	uint8_t age;
	Move best_move;

	TTEntry()
	    : key(0), value(0), depth(0), phase(SearchPhase::MAIN), bound(BoundType::EXACT), node_type(NodeType::ALL_NODE),
	      age(0), best_move()
	{}
};

// Pinned deliberately. The bucket count is derived from sizeof(TTEntry), so
// growing an entry shrinks the table for the same megabyte request -- which
// changes which positions collide, and therefore changes search results. That
// must be a decision someone makes, not a side effect of adding a field: a
// failure here means re-running the search-change validation tier, not bumping
// the number.
static_assert(sizeof(TTEntry) == 24, "TTEntry size change alters the bucket count and search behaviour");

// Transposition Table class
// Thread-safe with per-bucket locks for concurrent access
// Uses a simple replacement strategy based on depth and age
// Supports normalization of mate scores for correct distance handling
// Provides O(1) diagnostics via atomic counters
// clear() is protected by a global mutex
// Probes use shared locks for concurrent reads
class TranspositionTable {
  private:
	// Global mutex for whole-table operations, so they cannot interleave with
	// each other. Only clear() takes it today; there is no resize().
	std::shared_mutex tt_mutex;

	static constexpr size_t BUCKET_SIZE = 4;

	struct Bucket {
		TTEntry entries[BUCKET_SIZE];
	};

	static_assert(sizeof(Bucket) == BUCKET_SIZE * sizeof(TTEntry),
	              "Bucket must be exactly BUCKET_SIZE entries with no padding");

	std::vector<Bucket> table;
	// per-bucket shared mutexes to allow concurrent probes
	std::unique_ptr<std::shared_mutex[]> bucket_locks;
	size_t index_mask{0};

	std::atomic<uint8_t> current_age{0};

	// What the caller asked for. Kept only so requested_memory_mb() can report
	// it -- it is NOT what was allocated. See memory_mb().
	size_t requested_mb;

	// atomic counters for O(1) diagnostics (avoid scanning entire table)
	std::atomic<size_t> entry_count{0};
	std::atomic<size_t> pv_count{0};

	// helper: round down to nearest power of two >=1
	static size_t floor_pow2(size_t v)
	{
		if (v == 0)
			return 1;
		size_t p = 1;
		while ((p << 1) <= v)
			p <<= 1;
		return p;
	}

  public:
	// The bucket count is a power of two so probe/store can index with a mask
	// instead of a modulo, and it is rounded DOWN rather than to the nearest
	// power of two. Down is deliberate: the argument is a memory budget, so
	// overshooting it would be the worse failure -- a caller capping the table
	// for a constrained machine must not get a larger one than it asked for.
	//
	// The cost is that the allocation is generally smaller than the request, by
	// up to half. At the 256 MiB default: 268435456 / 96 = 2796202 buckets,
	// rounded down to 2^21 = 2097152, so 192 MiB of entries rather than 256.
	// memory_mb() reports what was actually allocated for exactly this reason.
	explicit TranspositionTable(size_t mb = 256) : requested_mb(mb)
	{
		size_t num_buckets = (mb * 1024 * 1024) / sizeof(Bucket);
		if (num_buckets == 0)
			num_buckets = 1;

		// use power-of-two bucket count for fast mask indexing
		size_t buckets = floor_pow2(num_buckets);
		table.resize(buckets);
		bucket_locks = std::make_unique<std::shared_mutex[]>(buckets);
		index_mask = buckets - 1;

		entry_count.store(0, std::memory_order_relaxed);
		pv_count.store(0, std::memory_order_relaxed);
	}
	// Helper to check if value is a mate score
	bool is_mate_score(int value) const
	{
		assert(value < GameValues::Mate + 100);
		return std::abs(value) >= (GameValues::Mate_Threshold);
	}

	// Storage: normalize mate scores. Mate +/- MAX_PLY fits in int16_t.
	int16_t normalize_for_storage(int16_t value, int ply) const noexcept
	{
		if (value >= GameValues::Mate_Threshold) {
			// Winning mate: add ply to "push back" the mate distance
			return static_cast<int16_t>(value + ply);
		} else if (value <= -GameValues::Mate_Threshold) {
			// Losing mate: subtract ply
			return static_cast<int16_t>(value - ply);
		}
		return value; // Non-mate scores unchanged
	}

	// Retrieval: denormalize mate scores. See normalize_for_storage re: the narrowing cast.
	int16_t denormalize_from_storage(int16_t stored_value, int ply) const noexcept
	{
		if (stored_value >= GameValues::Mate_Threshold) {
			// Store mate-in-N rather than absolute mate value
			return static_cast<int16_t>(stored_value - ply);
		} else if (stored_value <= -GameValues::Mate_Threshold) {
			// Add ply
			return static_cast<int16_t>(stored_value + ply);
		}
		return stored_value;
	}

	void newSearchIteration() { current_age.fetch_add(1, std::memory_order_relaxed); }

	std::optional<TTEntry> probe(std::uint64_t key, int current_ply) const
	{
		size_t index = static_cast<size_t>(key) & index_mask;
		const auto& bucket = table[index];

		// shared lock permits many concurrent probes
		std::shared_lock lock(bucket_locks[index]);

		for (const auto& entry : bucket.entries) {
			if (entry.key == key) {
				TTEntry result = entry;
				// Denormalize mate scores for current ply
				if (is_mate_score(result.value)) {
					result.value = denormalize_from_storage(result.value, current_ply);
				}
				return result;
			}
		}
		return std::nullopt;
	}

	void store(std::uint64_t key, int16_t value, int16_t depth, int16_t ply, Move best_move, BoundType bound,
	           NodeType node_type, SearchPhase phase)
	{
		size_t index = static_cast<size_t>(key) & index_mask;
		auto& bucket = table[index];

		// exclusive lock per-bucket
		std::unique_lock lock(bucket_locks[index]);

		uint8_t age = current_age.load(std::memory_order_relaxed);
		size_t replaceIndex = 0;
		int worst_score = std::numeric_limits<int>::max();

		for (size_t i = 0; i < BUCKET_SIZE; ++i) {
			auto& entry = bucket.entries[i];

			if (entry.key == key) {
				// Exact same position - overwrite old record
				replaceIndex = i;
				break;
			}

			int score = replacementScore(entry, age);
			if (score < worst_score) {
				worst_score = score;
				replaceIndex = i;
			}
		}
		auto& entry = bucket.entries[replaceIndex];

		// Track counts: was entry empty? was it PV before?
		bool was_empty = (entry.key == 0);
		NodeType old_node = entry.node_type;

		entry.key = key;
		entry.value = normalize_for_storage(value, ply);
		entry.depth = depth;
		entry.best_move = best_move;
		entry.bound = bound;
		entry.node_type = node_type;
		entry.age = age;
		entry.phase = phase;

		// update atomic counters (relaxed is sufficient under bucket lock)
		if (was_empty) {
			entry_count.fetch_add(1, std::memory_order_relaxed);
			if (node_type == NodeType::PV_NODE)
				pv_count.fetch_add(1, std::memory_order_relaxed);
		} else {
			// not empty: adjust pv_count if node_type changed
			if (old_node == NodeType::PV_NODE && node_type != NodeType::PV_NODE) {
				pv_count.fetch_sub(1, std::memory_order_relaxed);
			} else if (old_node != NodeType::PV_NODE && node_type == NodeType::PV_NODE) {
				pv_count.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	// Scale quiescence depth down to equivalent main search scale (tunable)
	int quiescenceEquivalentDepth(int quiescencePly) const noexcept
	{
		constexpr double scale = 0.5; // tuned constant
		return static_cast<int>(quiescencePly * scale);
	}

	// Compute entry score balancing depth, age, node type, and search phase
	// Scoring used for replacement decisions. Higher is better.
	// Provides a bonus for PV entries and a penalty for quiescence entries
	int replacementScore(const TTEntry& entry, int age) const noexcept
	{
		int age_diff = (age - entry.age) & 0xFF;

		// PV nodes are more valuable to keep
		int pv_bonus = (entry.node_type == NodeType::PV_NODE) ? 512 : 0;

		// Scale quiescence depth down to equivalent main search scale (tunable)
		int adjusted_depth = (entry.phase == SearchPhase::MAIN) ? entry.depth : quiescenceEquivalentDepth(entry.depth);

		// Quiescence searches sometimes have shallower depth but should still be
		// considered -- no phase bonus/penalty either way.
		constexpr int phase_bonus = 0;
		return adjusted_depth * 256 + pv_bonus + phase_bonus - age_diff * 512;
	}

	/*size_t count_entries() const {
        size_t count = 0;
        size_t buckets = table.size();
        for (size_t idx = 0; idx < buckets; ++idx) {
            std::shared_lock lock(bucket_locks[idx]);
            for (const auto& entry : table[idx].entries) {
                if (entry.key != 0)
                    ++count;
            }
        }
        return count;
    }

    size_t count_pv_nodes() const {
        size_t count = 0;
        size_t buckets = table.size();
        for (size_t idx = 0; idx < buckets; ++idx) {
            std::shared_lock lock(bucket_locks[idx]);
            for (const auto& entry : table[idx].entries) {
                if (entry.key != 0 && entry.node_type == NodeType::PV_NODE) {
                    ++count;
                }
            }
        }
        return count;
    }*/

	// O(1) diagnostics using atomics: cheap to call from hot paths
	size_t count_entries() const noexcept { return entry_count.load(std::memory_order_relaxed); }

	size_t count_pv_nodes() const noexcept { return pv_count.load(std::memory_order_relaxed); }

	// Lifecycle operation: callers must ensure no search can store concurrently.
	// tt_mutex serializes clear calls; store() intentionally takes only bucket locks.
	// Returns whether stored entries were removed.
	bool clear()
	{
		std::scoped_lock g(tt_mutex);
		if (entry_count.load(std::memory_order_relaxed) == 0) {
			// With no entries, current_age need not be reset: replacementScore()
			// compares only age differences, and the next entry adopts this age.
			return false;
		}

		size_t buckets = table.size();
		for (size_t idx = 0; idx < buckets; ++idx) {
			std::unique_lock lock(bucket_locks[idx]);
			for (auto& entry : table[idx].entries) {
				entry.key = 0;
				entry.value = 0;
				entry.depth = 0;
				entry.best_move = Move::EmptyMove();
				entry.bound = BoundType::EXACT;
				entry.node_type = NodeType::ALL_NODE;
				entry.age = 0;
				entry.phase = SearchPhase::MAIN;
			}
		}
		current_age.store(0, std::memory_order_relaxed);
		entry_count.store(0, std::memory_order_relaxed);
		pv_count.store(0, std::memory_order_relaxed);
		return true;
	}

	// diagnostics
	size_t bucket_count() const noexcept { return table.size(); }

	// What the constructor was asked for. Reported separately from what was
	// allocated because the two differ -- see the constructor comment.
	size_t requested_memory_mb() const noexcept { return requested_mb; }

	// Bytes holding TT entries.
	size_t entry_bytes() const noexcept { return table.size() * sizeof(Bucket); }

	// Bytes held by the parallel lock array, which cannot hold entries. Sharply
	// platform-dependent: sizeof(std::shared_mutex) is 8 on the MSVC STL
	// (SRWLOCK-based) but 56 on libstdc++ (pthread_rwlock_t), so this is a few
	// percent on the shipping Windows build and over half again the entry
	// memory on Linux.
	size_t lock_bytes() const noexcept { return table.size() * sizeof(std::shared_mutex); }

	// Everything the table allocates, entries plus locks.
	size_t allocated_bytes() const noexcept { return entry_bytes() + lock_bytes(); }

	// Megabytes actually allocated for entries -- NOT the constructor argument.
	// Reporting the request here instead would make the table's single memory
	// diagnostic incapable of revealing the shortfall it is there to describe.
	size_t memory_mb() const noexcept { return entry_bytes() / (size_t{1024} * 1024); }
};
