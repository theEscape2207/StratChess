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

struct TTEntry {
    std::uint64_t key;
    int16_t value;
    int16_t depth;
    Move best_move;
    BoundType bound;
    NodeType node_type;  // Track node type for better replacement
    uint8_t age;

    TTEntry() : key(0), value(0), depth(0), best_move(),
        bound(BoundType::EXACT), node_type(NodeType::ALL_NODE), age(0) {
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

    void new_search() {
        current_age.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<TTEntry> probe(std::uint64_t key) const {
        size_t index = key % table.size();
        const auto& bucket = table[index];

        for (const auto& entry : bucket.entries) {
            if (entry.key == key) {
                return entry;
            }
        }
        return std::nullopt;
    }

    void store(std::uint64_t key, int16_t value, int16_t depth,
        Move best_move, BoundType bound, NodeType node_type) {
        std::scoped_lock lock(tt_mutex);

        size_t index = key % table.size();
        auto& bucket = table[index];
        uint8_t age = current_age.load(std::memory_order_relaxed);

        int replace_idx = 0;
        int worst_score = std::numeric_limits<int>::max();

        for (int i = 0; i < BUCKET_SIZE; ++i) {
            auto& entry = bucket.entries[i];

            if (entry.key == key) {
                replace_idx = i;
                break;
            }

            // Enhanced replacement: PV nodes get +512 bonus
            int age_diff = (age - entry.age) & 0xFF;
            int pv_bonus = (entry.node_type == NodeType::PV_NODE) ? 512 : 0;
            int score = entry.depth * 256 + pv_bonus - age_diff * 512;

            if (score < worst_score) {
                worst_score = score;
                replace_idx = i;
            }
        }

        auto& entry = bucket.entries[replace_idx];
        entry.key = key;
        entry.value = value;
        entry.depth = depth;
        entry.best_move = best_move;
        entry.bound = bound;
        entry.node_type = node_type;
        entry.age = age;
    }

    size_t count_entries() const {
        size_t count = 0;
        for (const auto& bucket : table) {
            for (const auto& entry : bucket.entries) {
                if (entry.key != 0) ++count;
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