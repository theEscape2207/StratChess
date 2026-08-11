// ***************************************************************
//  MoveHelper   version:  1.0     date: 12/30/2006
//  -------------------------------------------------------------
//
//  -------------------------------------------------------------
//  Copyright (C) 2006 - All Rights Reserved
// ***************************************************************
//
// ***************************************************************

#pragma once

#include "Move.h"
#include "PieceHelper.h"
#include "SquareHelper.h"
#include <cassert>

namespace MoveHelper {
[[nodiscard]] static inline MoveType AsType(const Move& move) noexcept
{
	return static_cast<MoveType>(move.flags());
}

/*
 *	Moving-piece predicates — the moving piece is not stored in Move; callers supply it explicitly.
 */

// Is this piece moving from this square?
[[nodiscard]] static inline bool IsPieceMovingFrom(const Move& move, ePiece movPiece, ePiece type,
                                                   eSquare square) noexcept
{
	return (PieceHelper::IsOfPiece(movPiece, type) && (move.from() == square));
}

// Is this piece captured at this square?
// content: the captured piece (obtain via Board::GetCapturedPiece before DoMove).
[[nodiscard]] static inline bool IsPieceCapturedAt(const Move& move, ePiece content, ePiece type,
                                                   eSquare square) noexcept
{
	return (PieceHelper::IsOfPiece(content, type) && (move.to() == square));
}

// Is the moving piece a pawn?
[[nodiscard]] static inline bool IsPawnMove(ePiece movPiece) noexcept
{
	return PieceHelper::IsPawn(movPiece);
}

/*
 *	Move type methods
 */

//************************************
// Method:      IsCapture
// Returns:     true if the move captures a piece (CAPTURE, EP_CAPTURE, or any PROMOTION_*_CAPTURE).
// Determined purely from flag bit 2 (CAPTURE_BIT).
//************************************
[[nodiscard]] static inline bool IsCapture(const Move& move) noexcept
{
	return (move.flags() & MoveFlags::CAPTURE_BIT) != 0;
}

//************************************
// Method:      IsPromote
// Returns:     true for all promotion types (quiet and capture), i.e. flag bit 3 set.
//************************************
[[nodiscard]] static inline bool IsPromote(const Move& move) noexcept
{
	return (move.flags() & MoveFlags::PROMOTION_BIT) != 0;
}

[[nodiscard]] static inline bool IsEnPassant(const Move& move) noexcept
{
	return AsType(move) == MoveType::EP_CAPTURE;
}

[[nodiscard]] static inline bool IsCastling(const Move& move) noexcept
{
	MoveType type = AsType(move);
	return (type == MoveType::QUEEN_CASTLE) || (type == MoveType::KING_CASTLE);
}

[[nodiscard]] static inline eSquare GetEnPassantSquare(const Move& move, ePiece movPiece) noexcept
{
	if (AsType(move) != MoveType::DOUBLE_PAWN_PUSH)
		return NO_SQUARE;
	return (PieceHelper::Color(movPiece) == WHITE ? SquareHelper::Calc(move.to(), +ONE_ROW)
	                                              : SquareHelper::Calc(move.to(), -ONE_ROW));
}

[[nodiscard]] static inline bool is_null(const Move& move) noexcept
{
	return move.is_null();
}

// content: the captured piece (obtain via Board::GetCapturedPiece before DoMove).
// Used only inside assert() (Board.cpp:301, Board.cpp:473), so it is unreferenced in Release.
[[nodiscard]] [[maybe_unused]] static bool IsValid(const Move& move, ePiece movPiece,
                                                   ePiece content) noexcept
{
	if (is_null(move))
		return false;
	if (move.to() == move.from())
		return false;
	if ((PieceHelper::IsActual(content)) &&
	    (PieceHelper::Color(movPiece) == PieceHelper::Color(content)))
		return false;                 // Cannot capture own piece
	if (PieceHelper::IsKing(content)) // Cannot take a King
		return false;
	MoveType type = AsType(move);
	if (((type == MoveType::EP_CAPTURE) || (type == MoveType::DOUBLE_PAWN_PUSH)) &&
	    !IsPawnMove(movPiece))
		return false;
	switch (type) {
	case MoveType::DOUBLE_PAWN_PUSH:
		assert(!PieceHelper::IsActual(content)); // ingen slag
		assert(IsPawnMove(movPiece));
		break;
	case MoveType::EP_CAPTURE:
		assert(IsPawnMove(movPiece));
		break;
	case MoveType::KING_CASTLE:
		assert(move.from() == e1 || move.from() == e8); // must be in starting position
		switch (move.to()) {
		case g1: // Short castling
		case g8:
			break;
		default: // Unknown castling ? ;-)
			assert(!"Invalid castling 'to'-field");
			break;
		}
		break;
	case MoveType::QUEEN_CASTLE:
		assert(move.from() == e1 || move.from() == e8); // must be in starting position
		switch (move.to()) {
		case c1: // Long castling
		case c8:
			break;
		default: // Unknown castling ? ;-)
			assert(!"Invalid castling 'to'-field");
			break;
		}
		break;
	case MoveType::QUIET: // Default case
	case MoveType::CAPTURE:
	case MoveType::PROMOTION_KNIGHT:
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_QUEEN:
	case MoveType::PROMOTION_KNIGHT_CAPTURE:
	case MoveType::PROMOTION_BISHOP_CAPTURE:
	case MoveType::PROMOTION_ROOK_CAPTURE:
	case MoveType::PROMOTION_QUEEN_CAPTURE:
		break;
	}
	return true;
}

// MVV-LVA score for a move. Used for capture ordering in Sort and quiescence search.
// movPiece: the effective moving piece (obtain via Board::GetEffectiveMovPiece before DoMove).
// content:  the captured piece (obtain via Board::GetCapturedPiece before DoMove; NO_PIECE if
// quiet).
//
// Formula: Captured piece value + (Promotion value diff) - Moving piece/16
// Rationale: ranks pawn-takes-bishop above queen-takes-bishop; a quiet pawn move scores lower
// than a quiet rook move (negative, scaled by 1/16 of piece value).
[[nodiscard]] static inline int Value(const Move& move, ePiece movPiece, ePiece content) noexcept
{
	int captureScore = 0;
	const auto movingPieceScore = PieceHelper::Value(movPiece) >> 4;
	const auto type = static_cast<MoveType>(move.flags());
	switch (type) {
	case MoveType::QUIET:
	case MoveType::DOUBLE_PAWN_PUSH:
	case MoveType::QUEEN_CASTLE:
	case MoveType::KING_CASTLE:
		return (movingPieceScore *
		        -1); // TODO: Check what value this provides castling? Not used atm
	case MoveType::CAPTURE:
	case MoveType::EP_CAPTURE:
		captureScore = PieceHelper::Value(content);
		return captureScore - movingPieceScore;
	case MoveType::PROMOTION_KNIGHT:
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_QUEEN:
	case MoveType::PROMOTION_KNIGHT_CAPTURE:
	case MoveType::PROMOTION_BISHOP_CAPTURE:
	case MoveType::PROMOTION_ROOK_CAPTURE:
	case MoveType::PROMOTION_QUEEN_CAPTURE: // +: promoted piece value gain; +: captured piece; -:
	                                        // pawn value
		captureScore = PieceHelper::Value(movPiece) - PieceHelper::Value(ePiece::WHITE_PAWN);
		if (PieceHelper::IsActual(content))
			captureScore += PieceHelper::Value(content);
		return captureScore - static_cast<int>(g_iPieceValues[PAWN] >> 4);
	}
	return 0;
}

} // namespace MoveHelper
