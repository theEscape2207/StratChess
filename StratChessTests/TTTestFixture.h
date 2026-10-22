// TTTestFixture.h — shared test infrastructure for the TT*Tests.cpp files: the
// TranspositionTable friend fixture and the store helpers and keys they share.
//
// See Docs/TestDesign.md for the rationale.

#pragma once

#include "TranspositionTable.h"
#include "defines.h"

#include <array>
#include <bit>
#include <cstdint>

class TranspositionTableTestFixture {
  public:
	using Entry = TranspositionTable::PackedEntry;
	using Words = std::array<std::uint64_t, 2>;

	static Entry first_entry(const TranspositionTable& table)
	{
		return TranspositionTable::load(table.table[0].slots[0]);
	}

	static Words encode(const Entry& entry) { return TranspositionTable::encode(entry); }
	static std::uint64_t payload_word(const Entry& entry) { return std::bit_cast<Words>(entry)[1]; }
	static constexpr std::uint64_t slot_mix() { return TranspositionTable::SLOT_MIX; }

	// Writes one word of the table's first slot, as one half of a store would.
	static void write_word(TranspositionTable& table, size_t word, std::uint64_t value)
	{
		auto& target = table.table[0].slots[0];
		(word == 0 ? target.key_xor_data : target.data).store(value, std::memory_order_relaxed);
	}

	// The bucket a stored key actually landed in, found by scanning rather than by bucket_index().
	static const char* bucket_holding(const TranspositionTable& table, std::uint64_t key)
	{
		for (const auto& bucket : table.table) {
			for (const auto& slot : bucket.slots) {
				if (TranspositionTable::load(slot).matches(key)) {
					return reinterpret_cast<const char*>(&bucket);
				}
			}
		}
		return nullptr;
	}
};

// ── Helpers ───────────────────────────────────────────────────────────────────

inline Move no_move() { return Move::EmptyMove(); }

// Convenience wrapper: stores a value at the given key with sensible defaults.
inline void do_store(TranspositionTable& tt, uint64_t key, int16_t value, int16_t depth = 5, int16_t ply = 0)
{
	tt.store(key, value, depth, ply, no_move(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN, kNoStaticEval);
}

// The table keeps key bits 16-63 and lets the bucket index stand in for the rest, so distinct test
// keys must differ above bit 15 -- in a one-bucket table they would otherwise be one position.
inline constexpr uint64_t KEY_A = 1'000'001ULL << 16;
inline constexpr uint64_t KEY_B = 2'000'002ULL << 16;
inline constexpr uint64_t KEY_C = 3'000'003ULL << 16;
inline constexpr uint64_t KEY_D = 4'000'004ULL << 16;
inline constexpr uint64_t KEY_MISS = 9'876'543ULL << 16;
