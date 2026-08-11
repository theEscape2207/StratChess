#pragma once

#include "defines.h"

#include <array>
#include <bit>
#include <immintrin.h>

// clang-format off
// Whole file exempt. This is dense bit-manipulation whose readability comes from
// its hand-chosen line breaks — one logical step per line in the deposit/attack
// loops, and the magic tables kept narrow. Reformatting grew it by ~40 lines
// without making any of it easier to follow.

// PEXT-based ("fancy magic") sliding-piece attack generation for rooks and bishops.
// See .claude/plans/magic-bitboards-sliding-piece-attacks.md for the design rationale.
//
// All tables are generated at compile time. _pext_u64/_pdep_u64 are not constexpr-evaluable,
// so table generation uses depositBits() — a manual bit-deposit that reproduces _pdep_u64's
// semantics (distribute the low N bits of `index` into the N set-bit positions of `mask`,
// ascending). At runtime the tables are indexed with the real _pext_u64 intrinsic, whose
// result for a given occupancy is exactly the `index` that produced that occupancy subset
// during generation — pext and depositBits are inverses of each other by construction.

namespace magic {

	constexpr BITBOARD depositBits(unsigned int index, BITBOARD mask) noexcept
	{
		BITBOARD result = 0;
		unsigned int bit = 0;
		while (mask)
		{
			const BITBOARD lsb = mask & (~mask + 1);
			if (index & (1u << bit))
				result |= lsb;
			mask &= mask - 1;
			++bit;
		}
		return result;
	}

	// Relevant-occupancy mask: squares reachable from `sq` along rook/bishop rays, excluding
	// `sq` itself and the outer board edge in each direction — the edge square is always
	// reachable regardless of its occupancy, so it never affects which attack-table entry applies.
	constexpr BITBOARD rookMask(eSquare sq) noexcept
	{
		const int r = static_cast<int>(Rank(sq));
		const int f = static_cast<int>(File(sq));
		BITBOARD mask = 0;
		for (int rr = r - 1; rr >= 1; --rr) mask |= (UNIT << (rr * 8 + f));
		for (int rr = r + 1; rr <= 6; ++rr) mask |= (UNIT << (rr * 8 + f));
		for (int ff = f - 1; ff >= 1; --ff) mask |= (UNIT << (r * 8 + ff));
		for (int ff = f + 1; ff <= 6; ++ff) mask |= (UNIT << (r * 8 + ff));
		return mask;
	}

	constexpr BITBOARD bishopMask(eSquare sq) noexcept
	{
		const int r = static_cast<int>(Rank(sq));
		const int f = static_cast<int>(File(sq));
		BITBOARD mask = 0;
		for (int rr = r - 1, ff = f - 1; rr >= 1 && ff >= 1; --rr, --ff) mask |= (UNIT << (rr * 8 + ff));
		for (int rr = r - 1, ff = f + 1; rr >= 1 && ff <= 6; --rr, ++ff) mask |= (UNIT << (rr * 8 + ff));
		for (int rr = r + 1, ff = f - 1; rr <= 6 && ff >= 1; ++rr, --ff) mask |= (UNIT << (rr * 8 + ff));
		for (int rr = r + 1, ff = f + 1; rr <= 6 && ff <= 6; ++rr, ++ff) mask |= (UNIT << (rr * 8 + ff));
		return mask;
	}

	// True attack set for a given FULL occupancy (edges included, stops at and includes the
	// first blocker in each direction). Used only for compile-time table generation.
	constexpr BITBOARD rookAttacksSlow(eSquare sq, BITBOARD occupied) noexcept
	{
		const int r = static_cast<int>(Rank(sq));
		const int f = static_cast<int>(File(sq));
		BITBOARD attacks = 0;
		for (int rr = r - 1; rr >= 0; --rr) { const BITBOARD b = UNIT << (rr * 8 + f); attacks |= b; if (occupied & b) break; }
		for (int rr = r + 1; rr <= 7; ++rr) { const BITBOARD b = UNIT << (rr * 8 + f); attacks |= b; if (occupied & b) break; }
		for (int ff = f - 1; ff >= 0; --ff) { const BITBOARD b = UNIT << (r * 8 + ff); attacks |= b; if (occupied & b) break; }
		for (int ff = f + 1; ff <= 7; ++ff) { const BITBOARD b = UNIT << (r * 8 + ff); attacks |= b; if (occupied & b) break; }
		return attacks;
	}

	constexpr BITBOARD bishopAttacksSlow(eSquare sq, BITBOARD occupied) noexcept
	{
		const int r = static_cast<int>(Rank(sq));
		const int f = static_cast<int>(File(sq));
		BITBOARD attacks = 0;
		for (int rr = r - 1, ff = f - 1; rr >= 0 && ff >= 0; --rr, --ff) { const BITBOARD b = UNIT << (rr * 8 + ff); attacks |= b; if (occupied & b) break; }
		for (int rr = r - 1, ff = f + 1; rr >= 0 && ff <= 7; --rr, ++ff) { const BITBOARD b = UNIT << (rr * 8 + ff); attacks |= b; if (occupied & b) break; }
		for (int rr = r + 1, ff = f - 1; rr <= 7 && ff >= 0; ++rr, --ff) { const BITBOARD b = UNIT << (rr * 8 + ff); attacks |= b; if (occupied & b) break; }
		for (int rr = r + 1, ff = f + 1; rr <= 7 && ff <= 7; ++rr, ++ff) { const BITBOARD b = UNIT << (rr * 8 + ff); attacks |= b; if (occupied & b) break; }
		return attacks;
	}

	constexpr std::array<BITBOARD, ALL_SQUARES> makeRookMasks() noexcept
	{
		std::array<BITBOARD, ALL_SQUARES> result{};
		for (unsigned int sq = 0; sq < ALL_SQUARES; ++sq)
			result[sq] = rookMask(static_cast<eSquare>(sq));
		return result;
	}

	constexpr std::array<BITBOARD, ALL_SQUARES> makeBishopMasks() noexcept
	{
		std::array<BITBOARD, ALL_SQUARES> result{};
		for (unsigned int sq = 0; sq < ALL_SQUARES; ++sq)
			result[sq] = bishopMask(static_cast<eSquare>(sq));
		return result;
	}

	inline constexpr auto g_bbRookMask = makeRookMasks();
	inline constexpr auto g_bbBishopMask = makeBishopMasks();

	inline constexpr std::size_t ROOK_TABLE_SIZE = 4096;   // 2^12 — covers the widest rook mask (12 bits)
	inline constexpr std::size_t BISHOP_TABLE_SIZE = 512;  // 2^9  — covers the widest bishop mask (9 bits)

	constexpr std::array<std::array<BITBOARD, ROOK_TABLE_SIZE>, ALL_SQUARES> makeRookAttacks() noexcept
	{
		std::array<std::array<BITBOARD, ROOK_TABLE_SIZE>, ALL_SQUARES> result{};
		for (unsigned int sq = 0; sq < ALL_SQUARES; ++sq)
		{
			const BITBOARD mask = g_bbRookMask[sq];
			const unsigned int bits = std::popcount(mask);
			const std::size_t numSubsets = std::size_t{ 1 } << bits;
			for (std::size_t idx = 0; idx < numSubsets; ++idx)
			{
				const BITBOARD occ = depositBits(static_cast<unsigned int>(idx), mask);
				result[sq][idx] = rookAttacksSlow(static_cast<eSquare>(sq), occ);
			}
		}
		return result;
	}

	constexpr std::array<std::array<BITBOARD, BISHOP_TABLE_SIZE>, ALL_SQUARES> makeBishopAttacks() noexcept
	{
		std::array<std::array<BITBOARD, BISHOP_TABLE_SIZE>, ALL_SQUARES> result{};
		for (unsigned int sq = 0; sq < ALL_SQUARES; ++sq)
		{
			const BITBOARD mask = g_bbBishopMask[sq];
			const unsigned int bits = std::popcount(mask);
			const std::size_t numSubsets = std::size_t{ 1 } << bits;
			for (std::size_t idx = 0; idx < numSubsets; ++idx)
			{
				const BITBOARD occ = depositBits(static_cast<unsigned int>(idx), mask);
				result[sq][idx] = bishopAttacksSlow(static_cast<eSquare>(sq), occ);
			}
		}
		return result;
	}

	inline constexpr auto g_bbRookAttacks = makeRookAttacks();
	inline constexpr auto g_bbBishopAttacks = makeBishopAttacks();

} // namespace magic

inline BITBOARD RookAttacks(eSquare sq, BITBOARD occupied) noexcept
{
	const BITBOARD relevant = occupied & magic::g_bbRookMask[sq];
	return magic::g_bbRookAttacks[sq][static_cast<std::size_t>(_pext_u64(relevant, magic::g_bbRookMask[sq]))];
}

inline BITBOARD BishopAttacks(eSquare sq, BITBOARD occupied) noexcept
{
	const BITBOARD relevant = occupied & magic::g_bbBishopMask[sq];
	return magic::g_bbBishopAttacks[sq][static_cast<std::size_t>(_pext_u64(relevant, magic::g_bbBishopMask[sq]))];
}
// clang-format on
