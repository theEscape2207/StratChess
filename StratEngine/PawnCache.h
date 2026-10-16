#pragma once

#include "Eval.h"
#include "defines.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

// Per-thread memo of the pawn-structure terms (#837): eval_pawns without its blockade
// adjustment, keyed on the two pawn bitboards, and each colour's eval_king_pawn_cover keyed
// additionally on that colour's king square. A full tag compare makes a collision cost a miss,
// never a wrong score, and a miss simply overwrites the slot.
//
// Every value here is a pure function of its key, so an entry can never disagree with a
// recompute: that is why a write from a frame the search later aborts is harmless, and why the
// table is never cleared (Docs/EngineContracts.md -> aborted-frame rule).
//
// Owned by the caller, one per search thread (ThreadData); Evaluator itself stays stateless.
class PawnCache {
  public:
	// Shipped size, in entries (64 bytes each). Set by the #837 size sweep.
	static constexpr std::size_t kDefaultEntries = 4096;

	struct alignas(64) Entry {
		// The tag. ~0 in both is the empty slot: no legal position has a square holding a
		// white and a black pawn at once, so no real tag matches it, while the all-zero tag
		// stays a valid pawnless structure.
		BITBOARD wp = ~0ULL;
		BITBOARD bp = ~0ULL;
		// Both colours' passed pawns; intersect with wp or bp to split them.
		BITBOARD passers = 0;
		// Per colour: eval_pawns' phase-neutral score and its UNBLOCKED passer sums.
		int16_t pawn_score[NUM_COLORS]{};
		int16_t passed_mg[NUM_COLORS]{};
		int16_t passed_eg[NUM_COLORS]{};
		// Per colour: eval_king_pawn_cover's mg values (shelter, storm, files); eg is always 0.
		// Valid only for the king square beside it. NO_SQUARE is the empty cover, since 0 is a8.
		int16_t cover[NUM_COLORS][3]{};
		uint8_t king_sq[NUM_COLORS] = {NO_SQUARE, NO_SQUARE};

		bool Matches(BITBOARD white_pawns, BITBOARD black_pawns) const noexcept
		{
			return wp == white_pawns && bp == black_pawns;
		}

		// Replaces the pawn part and empties BOTH covers in the same step, so a cover can never
		// outlive the pawn structure it was computed from -- even when the caller then skips
		// the cover fill (phase 0, missing king).
		void StorePawns(BITBOARD white_pawns, BITBOARD black_pawns, const PawnStructure (&s)[NUM_COLORS]) noexcept
		{
			wp = white_pawns;
			bp = black_pawns;
			passers = s[WHITE].passers | s[BLACK].passers;
			for (const eColor c : {WHITE, BLACK}) {
				pawn_score[c] = static_cast<int16_t>(s[c].score);
				passed_mg[c] = static_cast<int16_t>(s[c].passed_mg);
				passed_eg[c] = static_cast<int16_t>(s[c].passed_eg);
				king_sq[c] = NO_SQUARE;
			}
		}

		PawnStructure Pawns(eColor color) const noexcept
		{
			return PawnStructure{pawn_score[color], passed_mg[color], passed_eg[color],
			                     passers & (color == WHITE ? wp : bp)};
		}

		void StoreCover(eColor color, eSquare king, const KingPawnCover& c) noexcept
		{
			cover[color][0] = static_cast<int16_t>(c.shelter.mg);
			cover[color][1] = static_cast<int16_t>(c.storm.mg);
			cover[color][2] = static_cast<int16_t>(c.files.mg);
			king_sq[color] = static_cast<uint8_t>(king);
		}

		KingPawnCover Cover(eColor color) const noexcept
		{
			return KingPawnCover{ScorePair{cover[color][0], 0}, ScorePair{cover[color][1], 0},
			                     ScorePair{cover[color][2], 0}};
		}
	};
	static_assert(sizeof(Entry) == 64 && alignof(Entry) == 64, "one entry is one cache line");
	static_assert(NUM_SQUARES <= NO_SQUARE && NO_SQUARE <= UINT8_MAX, "king_sq holds every square and NO_SQUARE");

	// `entries` must be a power of two; 1 is allowed (tests use it to force a collision on every
	// structure change).
	explicit PawnCache(std::size_t entries = kDefaultEntries)
	    : table_(std::make_unique<Entry[]>(entries)), bits_(std::countr_zero(entries))
	{
		assert(entries != 0 && (entries & (entries - 1)) == 0);
	}

	// The one slot this structure can live in; the caller checks Matches().
	Entry& Slot(BITBOARD white_pawns, BITBOARD black_pawns) noexcept
	{
		const uint64_t h = (white_pawns ^ (black_pawns * 0x9E3779B97F4A7C15ULL)) * 0xD6E8FEB86659FD93ULL;
		// Top `bits_` bits. Two shifts so bits_ == 0 never shifts by 64.
		return table_[static_cast<std::size_t>((h >> (63 - bits_)) >> 1)];
	}

  private:
	std::unique_ptr<Entry[]> table_;
	int bits_;
};
