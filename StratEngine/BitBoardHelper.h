#pragma once

#include "defines.h"

#include "Utils/BitTools.h"
#include <cassert>
#include <cstdint>
#include <ostream>

// Only pull in <iostream> for debug builds to avoid heavy header overhead in release
#if !defined(NDEBUG)
#	include <iostream>
#endif // !defined(NDEBUG)

namespace BitBoardHelper {
	/// @brief Prints a bitboard as an 8x8 grid of 0s and 1s.
	///        LSB (a1) is bottom-left, MSB (h8) is top-right.
	///        Intended for debugging only.
	inline void print_bitboard(std::ostream& out, BITBOARD bb) noexcept
	{
		for (std::uint8_t i = 0; i < ALL_SQUARES; ++i) {
			out << ((bb >> i) & 1u);
			if ((i & 7) == 7)
				out << '\n';
		}
		out << '\n';
	}

	/// @brief Clears bits on the board specified by the mask.
	/// @returns true if bits were successfully cleared, false if mask had no overlap.
	inline bool clear_bits(BITBOARD& board, BITBOARD mask) noexcept
	{
		if (!Bits::isAnyBitSet(board, mask)) // Verify intersection of board and mask is non-empty
		{
#if !defined(NDEBUG)
			print_bitboard(std::cerr, board);
			print_bitboard(std::cerr, mask);
#endif
			assert(Bits::isAnyBitSet(board, mask) && "clear_bits: attempting to clear unset bits");
			return false;
		}
		board = Bits::clearBits(board, mask);
		return true;
	}

	/// @brief Sets bits on the board specified by the mask.
	///        Asserts that target bits are currently clear.
	inline void set_bits(BITBOARD& board, BITBOARD mask) noexcept
	{
		// Verify that the piece being placed does not already exist on the board before adding it
		assert(Bits::areAllBitsClear(board, mask) && "set_bits: attempting to set already-set bits");
		board = Bits::setBits(board, mask);
	}

}; //namespace BitBoardHelper
