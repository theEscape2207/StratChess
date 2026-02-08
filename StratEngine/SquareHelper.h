// ***************************************************************
//  SquareHelper   version:  1.0   ·  date: 2018-09-22
//  -------------------------------------------------------------
//  Helper functions for working with the eSquare enum
//  -------------------------------------------------------------
//  Copyright (C) 2018 - All Rights Reserved
// ***************************************************************

#pragma once

// remove annoying level 4 warnings
#pragma warning(push)
#pragma warning( disable : 4505 )	// Unreferenced local function has been removed

namespace SquareHelper
{
	/*
	*	methods
	*/
	// Calculate the new eSquare position 
	static inline constexpr eSquare Calc(_In_ eSquare square, _In_ int offset) noexcept
	{
		return static_cast<eSquare>(square + offset);
	}

	// Helper for finding the previous row eSquare position depending on color
	static inline constexpr eSquare PreviousRow(eSquare To, eColor color) noexcept
	{
		return (color == eColor::WHITE ?
			SquareHelper::Calc(To, +ONE_ROW) :
			SquareHelper::Calc(To, -ONE_ROW));
	}
} // namespace SquareHelper

#pragma warning (pop)
