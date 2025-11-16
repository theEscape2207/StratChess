#pragma once

#include <iostream>
#include <vector>
#include <limits>
#include <chrono>
#include <optional>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <array>

#include <cassert>

#include "Move.h"

enum class BoundType : uint8_t {
    EXACT = 0,
    LOWER = 1,
    UPPER = 2
};

enum class NodeType : uint8_t {
    PV_NODE = 0,      // Principal variation node
    CUT_NODE = 1,     // Beta cutoff expected
    ALL_NODE = 2      // All moves must be searched
};

enum class SearchPhase { MAIN, QUIESCENCE };

struct TTEntry {
    std::uint64_t key;
    int16_t value;
    int16_t depth;  // Search depth or equivalent
    SearchPhase phase;
    Move best_move;
    BoundType bound;
    NodeType node_type;  // Track node type for better replacement
    uint8_t age;

    TTEntry() : key(0), value(0), depth(0), best_move(),
        bound(BoundType::EXACT), node_type(NodeType::ALL_NODE), age(0), phase(SearchPhase::MAIN) {
    }
};

// Triangular PV Table for principal variation caching
class PVTable {
private:
    std::array<std::array<Move, MAX_PLY>, MAX_PLY> table;
    std::array<int, MAX_PLY> length;

public:
    PVTable() {
        for (int i = 0; i < MAX_PLY; ++i) {
            length[i] = 0;
        }
    }

    void update(int ply, Move move) {
        table[ply][0] = move;
        // Copy rest from next ply (triangular structure)
        for (int i = 0; i < length[ply + 1]; ++i) {
            table[ply][i + 1] = table[ply + 1][i];
        }
        length[ply] = length[ply + 1] + 1;
    }

    void clear_ply(int ply) {
        length[ply] = 0;
    }

    Move get_pv_move(int ply) const {
        return (length[ply] > 0) ? table[ply][0] : Move();
    }

    const Move* get_line(int ply) const {
        return table[ply].data();
    }

    int get_length(int ply) const {
        return length[ply];
    }
};

class TranspositionTable {
private:
    std::mutex tt_mutex;

    static constexpr size_t BUCKET_SIZE = 4;

    struct Bucket {
        TTEntry entries[BUCKET_SIZE];
    };

    std::vector<Bucket> table;
    std::atomic<uint8_t> current_age{ 0 };
    size_t size_mb;

public:
    explicit TranspositionTable(size_t mb = 256) : size_mb(mb) {
        size_t num_buckets = (mb * 1024 * 1024) / sizeof(Bucket);
        table.resize(num_buckets);
    }
    // Helper to check if value is a mate score
    bool is_mate_score(int value) const {
        assert(value < GameValues::Mate+100);
        return std::abs(value) >= (GameValues::Mate_Threshold);
    }

    // Storage: normalize mate scores
    int16_t normalize_for_storage(int16_t value, int ply) {
        if (value >= GameValues::Mate_Threshold) {
            // Winning mate: add ply to "push back" the mate distance
            return value + static_cast<int16_t>(ply);
        }
        else if (value <= -GameValues::Mate_Threshold) {
            // Losing mate: subtract ply
            return value - static_cast<int16_t>(ply);
        }
        return value;  // Non-mate scores unchanged
    }

    // Retrieval: denormalize mate scores
    int16_t denormalize_from_storage(int16_t stored_value, int ply) const {
        if (stored_value >= GameValues::Mate_Threshold) {
            // Store mate-in-N rather than absolute mate value
            return stored_value - static_cast<int16_t>(ply);
        }
        else if (stored_value <= -GameValues::Mate_Threshold) {
            // Add ply
            return stored_value + static_cast<int16_t>(ply);
        }
        return stored_value;
    }

    void newSearchIteration() {
        current_age.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<TTEntry> probe(std::uint64_t key, int current_ply) const {
        size_t index = key % table.size();
        const auto& bucket = table[index];

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

    void store(std::uint64_t key, int16_t value, int16_t depth, int16_t ply,
        Move best_move, BoundType bound, NodeType node_type, SearchPhase phase)
    {
        std::scoped_lock lock(tt_mutex);

        size_t index = key % table.size();
        auto& bucket = table[index];
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
        entry.key = key;
        entry.value = normalize_for_storage(value, ply);
        entry.depth = depth;
        entry.best_move = best_move;
        entry.bound = bound;
        entry.node_type = node_type;
        entry.age = age;
        entry.phase = phase;   
    }

    // Scale quiescence depth down to equivalent main search scale (tunable)
    int quiescenceEquivalentDepth(int quiescencePly) const {
        constexpr double scale = 0.5;  // tuned constant
        return static_cast<int>(quiescencePly * scale);
    }

    // Compute entry score balancing depth, age, node type, and search phase
    // Scoring used for replacement decisions. Higher is better.
    // Provides a bonus for PV entries and a penalty for quiescence entries
    int replacementScore(const TTEntry& entry, int age) const
    {
        int age_diff = (age - entry.age) & 0xFF;
        
        // PV nodes are more valuable to keep
        int pv_bonus = (entry.node_type == NodeType::PV_NODE) ? 512 : 0;
        
        // Scale quiescence depth down to equivalent main search scale (tunable)
        int adjusted_depth = (entry.phase == SearchPhase::MAIN) ? entry.depth : quiescenceEquivalentDepth(entry.depth);
        
        // Quiescence searches sometimes have shallower depth but should still be considered
        int phase_bonus = (entry.phase == SearchPhase::MAIN) ? 0 : 0/*-256*/;
        return adjusted_depth * 256 + pv_bonus + phase_bonus - age_diff * 512;
    }

    size_t count_entries() const {
        size_t count = 0;
        for (const auto& bucket : table) {
            for (const auto& entry : bucket.entries) {
                if (entry.key != 0)
                    ++count;
            }
        }
        return count;
    }

    size_t count_pv_nodes() const {
        size_t count = 0;
        for (const auto& bucket : table) {
            for (const auto& entry : bucket.entries) {
                if (entry.key != 0 && entry.node_type == NodeType::PV_NODE) {
                    ++count;
                }
            }
        }
        return count;
    }
};