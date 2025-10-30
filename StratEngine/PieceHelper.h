// ***************************************************************
//  PieceHelper   version:  1.0   ·  date: 12/28/2014
//  -------------------------------------------------------------
//  
//  -------------------------------------------------------------
//  Copyright (C) 2014 - All Rights Reserved
// ***************************************************************
// 
// ***************************************************************

#pragma once

// remove annoying level 4 warnings
#pragma warning(push)
#pragma warning( disable : 4505 )	// Unreferenced local function has been removed

#include <string>

#include "defines.h"


namespace PieceHelper
{
	/*
	*	methods
	*/

	static constexpr bool IsOfType(_In_ ePiece piece, _In_ ePieceType type) noexcept
	{
		return ((piece >> 1) == (type >> 1));
	}

	static constexpr bool IsOfPiece(_In_ ePiece piece, _In_ ePiece type) noexcept
	{
		return (piece == type);
	}

	static constexpr bool IsActual(_In_ ePiece piece) noexcept
	{
		return (ePiece::WHITE_PAWN <= piece) && (piece <= ePiece::BLACK_KING);
	}

	static constexpr bool IsPawn(_In_ ePiece piece) noexcept
	{
		return IsOfType(piece, PAWN);
	}

	static constexpr bool IsKing(_In_ ePiece piece) noexcept
	{
		return IsOfType(piece, KING);
	}

	static constexpr bool IsNoPiece(_In_ ePiece piece) noexcept
	{
		return (piece == ePiece::NO_PIECE);
	}

	static constexpr std::string FullName(_In_ enum ePiece piece)
	{
		return g_cPieceNamesVerbose[piece];
	}

	static constexpr char ShortName(_In_ ePiece piece) noexcept
	{
		return g_cPieceNames[piece];
	}

	/// <summary>
	/// Helper method that returns the corresponding Pawn from the input Piece
	/// </summary>
	/// <param name="piece">The Piece</param>
	/// <returns>The corresponding Pawn of same color</returns>
	static constexpr ePiece AsPawn(_In_ ePiece piece) noexcept
	{
		return static_cast<ePiece>(piece & 1);
	}

	static constexpr std::string FullPawnName(_In_ ePiece piece)
	{
		return g_cPieceNamesVerbose[AsPawn(piece)];
	}

	static constexpr bool IsNotEmpty(_In_ ePiece piece) noexcept
	{
		return piece != ePiece::NO_PIECE;
	}

	static constexpr unsigned int Value(_In_ ePiece piece) noexcept
	{
		return (g_iPieceValues[piece >> 1]);
	}

	static constexpr eColor Color(_In_ ePiece piece) noexcept
	{
		return static_cast<eColor>(piece & 1);
	}

} // namespace PieceHelper

#pragma warning (pop)

