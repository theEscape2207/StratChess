/////////////////////////////////////////////////////////////////////////////
// @doc
// @module BitBoardHelper.h | BitBoardHelper
//
// @normal Copyright (c) 2006 by Sveinn Sigfredsson
//
// Date:	Tuesday, December 12, 2006
//
// Revision: 
//
///////////////

#pragma once

// remove annoying level 4 warnings
#pragma warning(push)
#pragma warning( disable : 4505 )	// Unreferenced local function has been removed

#include "defines.h"

#include "Utils/BitTools.h"
#include <cassert>

namespace BitBoardHelper
{
	// Udskriver et BitBoard til skaermen 
	static void PrintBitboardBinary(BITBOARD bb, std::ostream& stream)
	{
		for (unsigned int i=0; i<ALL_SQUARES; ++i)
		{
			stream << ((bb >> i) & UNIT);
			if ((i & 7) == 7) {
				stream << std::endl;
			}
		}
		stream << std::endl;
	}

	// Clears the bits at the board specified by the mask
	static bool ClearBitboardMask(BITBOARD& board, BITBOARD mask)
	{
		// Forventer at brikken, der skal fjernes faktisk findes paa braedtet!!
		if ( !Bits::isAnyBitSet(board, mask) ) 
		{
			PrintBitboardBinary( board, std::cout );
			PrintBitboardBinary( mask, std::cout );
			assert(Bits::isAnyBitSet(board, mask)); //	force assert
			return false;
		}
		Bits::clearBitsRef(board, mask);
		return true;
	}

	// Saetter Bit(s) og tjekker om der allerede staar en brik paa felterne indeholdt i 'mask'
	static void SetBitboardMask(BITBOARD& board, BITBOARD mask) noexcept
	{
		// Forventer at brikken, der skal indsaettes ikke allerede findes paa braedtet!!
		assert(Bits::areAllBitsClear(board, mask));
		Bits::setBitsRef(board, mask);
	}

}; //namespace BitBoardHelper

#pragma warning(pop)	// Unreferenced local function has been removed
