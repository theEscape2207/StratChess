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
#if defined(_MSC_VER)
#	pragma warning(push)
#	pragma warning(disable : 4505) // Unreferenced local function has been removed
#endif

#include <string>

#include "defines.h"

namespace PieceHelper {
	/*
	*	methods
	*/

	static inline constexpr bool IsOfType(ePiece piece, ePieceType type) noexcept
	{
		return ((piece >> 1) == (type >> 1));
	}

	static inline constexpr bool IsOfPiece(ePiece piece, ePiece type) noexcept { return (piece == type); }

	static inline constexpr bool IsActual(ePiece piece) noexcept
	{
		return (ePiece::WHITE_PAWN <= piece) && (piece <= ePiece::BLACK_KING);
	}

	static inline constexpr bool IsPawn(ePiece piece) noexcept { return IsOfType(piece, PAWN); }

	static inline constexpr bool IsKing(ePiece piece) noexcept { return IsOfType(piece, KING); }

	static inline constexpr bool IsNoPiece(ePiece piece) noexcept { return (piece == ePiece::NO_PIECE); }

	static inline constexpr std::string FullName(enum ePiece piece) { return g_cPieceNamesVerbose[piece]; }

	static inline constexpr char ShortName(ePiece piece) noexcept { return g_cPieceNames[piece]; }

	/// <summary>
	/// Helper method that returns the corresponding Pawn from the input Piece
	/// </summary>
	/// <param name="piece">The Piece</param>
	/// <returns>The corresponding Pawn of same color</returns>
	static inline constexpr ePiece AsPawn(ePiece piece) noexcept { return static_cast<ePiece>(piece & 1); }

	static inline constexpr std::string FullPawnName(ePiece piece) { return g_cPieceNamesVerbose[AsPawn(piece)]; }

	static inline constexpr bool IsNotEmpty(ePiece piece) noexcept { return piece != ePiece::NO_PIECE; }

	static inline constexpr unsigned int Value(ePiece piece) noexcept { return (g_iPieceValues[piece >> 1]); }

	static inline constexpr eColor Color(ePiece piece) noexcept { return static_cast<eColor>(piece & 1); }

	// It is always possible to go from PieceType to Piece
	static inline constexpr ePiece AsPiece(ePieceType pieceType, eColor color) noexcept
	{
		return static_cast<ePiece>(pieceType + static_cast<size_t>(color));
	}

	static inline constexpr ePiece AsPiece(ePiece piece, eColor color) noexcept
	{
		return static_cast<ePiece>(piece + static_cast<size_t>(color));
	}

	static inline constexpr ePieceType AsPieceType(ePiece piece) noexcept
	{
		return static_cast<ePieceType>(piece >> 1);
	}
	// Returns the Pawn of the opposite color. E.g. color=WHITE_ -> BLACK_PAWN
	static inline constexpr ePiece AsPawn(eColor color) noexcept { return static_cast<ePiece>(color); }

	// Returns the Pawn of the opposite color. E.g. color=WHITE_ -> BLACK_PAWN
	static inline constexpr ePiece OppositePawn(eColor color) noexcept
	{
		return static_cast<ePiece>(BLACK_PAWN - static_cast<size_t>(color));
	}
} // namespace PieceHelper

#if defined(_MSC_VER)
#	pragma warning(pop)
#endif
