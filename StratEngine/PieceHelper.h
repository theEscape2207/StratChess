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

#include <cassert>
#include <string>

#include "defines.h"

namespace PieceHelper {
	constexpr bool IsOfType(ePiece piece, ePieceType type) noexcept { return ((piece >> 1) == (type >> 1)); }

	constexpr bool IsOfPiece(ePiece piece, ePiece type) noexcept { return (piece == type); }

	constexpr bool IsActual(ePiece piece) noexcept
	{
		return (ePiece::WHITE_PAWN <= piece) && (piece <= ePiece::BLACK_KING);
	}

	constexpr bool IsPawn(ePiece piece) noexcept { return IsOfType(piece, PAWN); }

	constexpr bool IsKing(ePiece piece) noexcept { return IsOfType(piece, KING); }

	constexpr bool IsNoPiece(ePiece piece) noexcept { return (piece == ePiece::NO_PIECE); }

	constexpr std::string FullName(enum ePiece piece) { return g_cPieceNamesVerbose[piece]; }

	constexpr char ShortName(ePiece piece) noexcept { return g_cPieceNames[piece]; }

	// The pawn of the same colour as `piece`.
	constexpr ePiece AsPawn(ePiece piece) noexcept { return static_cast<ePiece>(piece & 1); }

	constexpr std::string FullPawnName(ePiece piece) { return g_cPieceNamesVerbose[AsPawn(piece)]; }

	constexpr bool IsNotEmpty(ePiece piece) noexcept { return piece != ePiece::NO_PIECE; }

	// Callers are expected to pass an actual piece. The aggregate entries and NO_PIECE index the
	// zero-valued tail of g_iPieceValues, so a stray value reads in bounds and scores nothing
	// rather than reading past the table; the assert catches the caller that got there by mistake.
	constexpr int Value(ePiece piece) noexcept
	{
		assert(IsActual(piece));
		return (g_iPieceValues[piece >> 1]);
	}

	constexpr eColor Color(ePiece piece) noexcept { return static_cast<eColor>(piece & 1); }

	// Valid piece types and colors produce named ePiece values 0 through 13.
	constexpr ePiece AsPiece(ePieceType pieceType, eColor color) noexcept
	{
		// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
		return static_cast<ePiece>(pieceType + static_cast<size_t>(color));
	}

	constexpr ePiece AsPiece(ePiece piece, eColor color) noexcept
	{
		return static_cast<ePiece>(piece + static_cast<size_t>(color));
	}

	// ePieceType steps in twos (KNIGHT == 2, BISHOP == 4, ...), matching ePiece with the colour
	// bit cleared — so the type is recovered by masking bit 0, not by shifting it away. Shifting
	// yields a compact 0-5 index, which is what IsOfType compares but is not an ePieceType value.
	// Only defined for actual pieces; the aggregates and NO_PIECE have no type.
	constexpr ePieceType AsPieceType(ePiece piece) noexcept
	{
		assert(IsActual(piece));
		return static_cast<ePieceType>(piece & ~1);
	}
	// The pawn of `color`, e.g. WHITE -> WHITE_PAWN.
	constexpr ePiece AsPawn(eColor color) noexcept { return static_cast<ePiece>(color); }

	// The pawn of the colour opposite `color`, e.g. WHITE -> BLACK_PAWN.
	constexpr ePiece OppositePawn(eColor color) noexcept
	{
		return static_cast<ePiece>(BLACK_PAWN - static_cast<size_t>(color));
	}
} // namespace PieceHelper
