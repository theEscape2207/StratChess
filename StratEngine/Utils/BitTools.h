/////////////////////////////////////////////////////////////////////////////
// @doc
// @module BitTools.h | BitTools
//
// @normal Copyright (c) 2001 by Eduardo Velasquez
// @normal Copyright (c) 2004 by Sveinn Sigfredsson
//
// Date:	Friday, December 21, 2001
//
// Revision: 041125 SSI: Added toogleBits() and toogleBitByPos() template functions
//						 Changed all ASSERT MFC macros to use assert() instead
//						 Added const modifiers on _all_ parameters
//						 Made a lot of parameters by-ref instead of by-val
//
///////////////

#pragma once

#include <cassert>
#include <limits>

namespace Bits
{
	/////////////////////////////////////////////////////////////////////////////
	// Bit functions based on bitmask

	// Returns true if any of the bits in mask is set in value. Defined as: (value & mask) != 0
	template<class T>
	static inline bool isAnyBitSet(const T& value, const T& mask) noexcept
	{
		return (value & mask) != 0;
	}

	template<class T, class U>
	static inline bool isAnyBitSet(const T& value, const U& mask) noexcept
	{
		return (value & mask) != 0;
	}

	// Returns true if all the bits in mask are set in value. Defined as: (value & mask) == mask
	template<class T, class U>
	static inline bool areAllBitsSet(const T& value, const U& mask) noexcept
	{
		return (value & mask) == mask;
	}

	// Returns true if all the bits in mask are cleared in value. Defined as: (value & mask) == 0
	template<class T, class U>
	static inline bool areAllBitsClear(const T& value, const U& mask) noexcept
	{
		return (value & mask) == 0;
	}

	// Returns value with the mask bits set. Defined as: value | mask
	template<class T, class U>
	static inline T setBits(const T& value, const U& mask) noexcept
	{
		return value | mask;
	}

	// Changes value with the mask bits set. Defined as: value |= mask
	template<class T>
	static inline void setBitsRef	(T& value, const T& mask) noexcept
	{
		value |= mask;
	}

	// Returns value with the all the bits set except the mask bits. Defined as: value | ~mask
	template<class T, class U>
	static inline T setBitsExcept(const T& value, const U& mask) noexcept
	{
		return value | ~mask;
	}

	// Returns value with the mask bits cleared. Defined as: value & ~mask
	template<class T, class U>
	static inline T clearBits(const T& value, const U& mask) noexcept
	{
		return value & ~mask;
	}

	// Changes value with the mask bits cleared. Defined as: value &= ~mask
	template<class T>
	static inline void clearBitsRef(T& value, const T& mask) noexcept
	{
		value &= ~mask;
	}

	// Returns value with the all the bits cleared except those marked by the mask bits. Defined as: value & mask
	template <class T, class U>
	static inline T clearBitsExcept(const T& value, const U& mask) noexcept
	{
		return value & mask;
	}

	// 041125: Added: both parameters are of the same type
	// Its giving better performance for larger types avoiding the copy
	// Updates value with the all the bits cleared except those marked by the mask bits. Defined as: value &= mask
	template <class T>
	static inline void clearBitsExceptRef(T& value, const T& mask) noexcept
	{
		value &= mask;
	}

	// Returns value with the mask bits set or cleared depending on the value of set.
	// A bit of a bastard, really...
	template <class T, class U, class V>
	static inline T setBits(const T& value, const U& mask, const V& bSet) noexcept
	{
		return bSet != 0 ? setBits(value, mask) : clearBits(value, mask);
	}

	// Returns value with the add bits set and the remove bits cleared. Defined as: (value | add) & ~remove
	template <class T, class U>
	static inline T setClearBits(const T& value, const U& add, const U& remove) noexcept
	{
		return (value | add) & ~remove;
	}

	// XOR: Changes all bits in value with the mask bits. Defined as: value ^= mask
	template <class T, class U>
	static inline void toogleBits(T& value, const U& mask) noexcept
	{
		value ^= mask;
	}


	/////////////////////////////////////////////////////////////////////////////
	// Bit functions based on bit position

	// Returns value with the nth bit set. Defined as value | (1 << n)
	template <class T>
	static inline T setBitByPos(const T& value, const unsigned char bit) noexcept
	{
		assert(bit < std::numeric_limits<T>::digits);
		return value | (1 << bit);
	}

	// Returns value with the nth bit cleared. Defined as value & ~(1 << n)
	template <class T>
	static inline T clearBitByPos(const T& value, const unsigned char bit) noexcept
	{
		assert(bit < std::numeric_limits<T>::digits);
		return value & ~(1 << bit);
	}

	template <class T>
	static inline T toogleBitByPos(const T& value, const unsigned char bit) noexcept
	{
		assert(bit < std::numeric_limits<T>::digits);
		return value ^ (1 << bit);
	}

	// Returns true if value has the the nth bit set. Defined as (value & (1 << n)) != 0
	template <class T>
	static inline bool isBitSetByPos(const T& value, const unsigned char bit) noexcept
	{
		assert(bit < std::numeric_limits<T>::digits);
		return (value & (1 << bit)) != 0;
	}

	// Returns true if value has the the nth bit cleared. Defined as (value & (1 << n)) == 0
	template <class T>
	static inline bool isBitClearByPos(const T& value, const unsigned char bit) noexcept
	{
		return isBitSetByPos(value, bit) == false;
	}

}; //namespace BitTools
