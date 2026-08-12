// The code is a header file that defines a namespace `Bits` containing utility functions for manipulating bitboards,
// which are represented as 64-bit unsigned integers (`std::uint64_t`).

// Original code copyrighted by Eduardo Velasquez and Sveinn Sigfredsson, with modifications for C++20 and chess engine optimizations.

#pragma once
#include "defines.h"
#include <cstdint>

namespace Bits {

	// =========================================================================
	// Query Operations
	// =========================================================================
	// Returns true if any bit specified by the mask is set (1) in the value
	[[nodiscard]] constexpr bool isAnyBitSet(BITBOARD value, BITBOARD mask) noexcept { return (value & mask) != 0; }

	// Returns true if all bits specified by the mask are set (1) in the value - currently unused
	[[nodiscard]] constexpr bool areAllBitsSet(BITBOARD value, BITBOARD mask) noexcept
	{
		return (value & mask) == mask;
	}

	// Returns true if all bits specified by the mask are clear (0) in the value
	[[nodiscard]] constexpr bool areAllBitsClear(BITBOARD value, BITBOARD mask) noexcept { return (value & mask) == 0; }

	// =========================================================================
	// Functional Modification Operations (Preferred in chess engines)
	// =========================================================================
	// Set bits specified by the mask
	[[nodiscard]] constexpr BITBOARD setBits(BITBOARD value, BITBOARD mask) noexcept { return value | mask; }

	// Clear bits specified by the mask
	[[nodiscard]] constexpr BITBOARD clearBits(BITBOARD value, BITBOARD mask) noexcept { return value & ~mask; }

	// Clears the least-significant set bit (Kernighan's trick). Preferred over
	// clearBits(value, g_bbMask[countr_zero(value)]) in bitboard-iteration loops -
	// no table lookup needed since the bit being cleared is always the lsb of 'value' itself.
	[[nodiscard]] constexpr BITBOARD clearLsb(BITBOARD value) noexcept { return value & (value - 1); }

	// Apply a mask to a value, returning only the bits that are set in both
	[[nodiscard]] constexpr BITBOARD applyMask(BITBOARD value, BITBOARD mask) noexcept { return value & mask; }

	// Set bits if true, clear if false (branchless-friendly)
	[[nodiscard]] constexpr BITBOARD setBitsConditionally(BITBOARD value, BITBOARD mask, bool enable) noexcept
	{
		return (value & ~mask) | (enable ? mask : 0);
	}

	// Add bits, remove bits
	[[nodiscard]] constexpr BITBOARD setClearBits(BITBOARD value, BITBOARD add, BITBOARD remove) noexcept
	{
		return (value | add) & ~remove;
	}
} // namespace Bits

// Compile-time smoke tests - zero runtime cost, catch logic regressions in the above at build time
static_assert(Bits::isAnyBitSet(0b1010, 0b0010) == true, "isAnyBitSet failed");
static_assert(Bits::isAnyBitSet(0b1010, 0b0001) == false, "isAnyBitSet failed");
static_assert(Bits::areAllBitsSet(0b1111, 0b1010) == true, "areAllBitsSet failed");
static_assert(Bits::areAllBitsSet(0b1101, 0b1010) == false, "areAllBitsSet failed");
static_assert(Bits::areAllBitsClear(0b1100, 0b0011) == true, "areAllBitsClear failed");
static_assert(Bits::areAllBitsClear(0b1100, 0b0100) == false, "areAllBitsClear failed");
static_assert(Bits::setBits(0b1010, 0b0001) == 0b1011, "setBits failed");
static_assert(Bits::clearBits(0b1011, 0b0001) == 0b1010, "clearBits failed");
static_assert(Bits::clearLsb(0b1010) == 0b1000, "clearLsb failed");
static_assert(Bits::clearLsb(0b1100) == 0b1000, "clearLsb failed");
static_assert(Bits::applyMask(0b1110, 0b1010) == 0b1010, "applyMask failed");
static_assert(Bits::setBitsConditionally(0b1010, 0b0001, true) == 0b1011, "setBitsConditionally failed");
static_assert(Bits::setBitsConditionally(0b1011, 0b0001, false) == 0b1010, "setBitsConditionally failed");
static_assert(Bits::setClearBits(0b1000, 0b0010, 0b1000) == 0b0010, "setClearBits failed");
