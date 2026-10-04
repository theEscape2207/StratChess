// ***************************************************************
//  SquareHelper   version:  1.0   ·  date: 2018-09-22
//  -------------------------------------------------------------
//  Helper functions for working with the eSquare enum
//  -------------------------------------------------------------
//  Copyright (C) 2018 - All Rights Reserved
// ***************************************************************

#pragma once

#include "defines.h"

namespace SquareHelper {
	// The square `offset` indices away.
	constexpr eSquare Calc(eSquare square, int offset) noexcept { return static_cast<eSquare>(square + offset); }

	// The square one rank behind `to` from `color`'s point of view.
	constexpr eSquare PreviousRow(eSquare to, eColor color) noexcept
	{
		return (color == eColor::WHITE ? SquareHelper::Calc(to, +ONE_ROW) : SquareHelper::Calc(to, -ONE_ROW));
	}
} // namespace SquareHelper
