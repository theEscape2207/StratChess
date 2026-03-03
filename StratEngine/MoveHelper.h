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

// remove annoying level 4 warnings
#pragma warning(push)
#pragma warning( disable : 4505 )	// Unreferenced local function has been removed

#include "Move.h"
#include "PieceHelper.h"
#include "SquareHelper.h"
#include <cassert>

namespace MoveHelper
{
	[[nodiscard]] static inline MoveType AsType(_In_ const Move& move) noexcept
	{
		return static_cast<MoveType>(move.flags());
	}
	/*
	*	MovPiece methods — callers supply the moving piece explicitly (Phase 3: MovPiece removed from Move)
	*/

	[[nodiscard]] static inline bool IsMoveType([[maybe_unused]] _In_ const Move& move, _In_ ePiece movPiece, ePieceType type) noexcept
	{
		return PieceHelper::IsOfType(movPiece, type);
	}

	[[nodiscard]] static inline bool IsMovingPiece([[maybe_unused]] _In_ const Move& move, _In_ ePiece movPiece, ePiece type) noexcept
	{
		return PieceHelper::IsOfPiece(movPiece, type);
	}

	// Is this piece moving from this square?
	[[nodiscard]] static inline bool IsPieceMovingFrom(_In_ const Move& move, _In_ ePiece movPiece, ePiece type, eSquare square) noexcept
	{
		return (PieceHelper::IsOfPiece(movPiece, type) && (move.from() == square));
	}

	// Is this piece captured at this square?
	// content: the captured piece (obtain via Board::GetCapturedPiece before DoMove; Phase 4: no Content field).
	[[nodiscard]] static inline bool IsPieceCapturedAt(_In_ const Move& move, _In_ ePiece content, ePiece type, eSquare square) noexcept
	{
		return (PieceHelper::IsOfPiece(content, type) && (move.to() == square));
	}

	// Is the moving piece a pawn
	[[nodiscard]] static inline bool IsPawnMove([[maybe_unused]] _In_ const Move& move, _In_ ePiece movPiece) noexcept
	{
		return PieceHelper::IsPawn(movPiece);
	}

	[[nodiscard]] static inline bool IsKingMove([[maybe_unused]] _In_ const Move& move, _In_ ePiece movPiece) noexcept
	{
		return PieceHelper::IsKing(movPiece);
	}

	/*
	*	Move type methods
	*/

	//************************************
	// Method:      IsCapture
	// Returns:     true if the move captures a piece (CAPTURE, EP_CAPTURE, or any PROMOTION_*_CAPTURE).
	// Phase 4: determined purely from flag bit 2 (CAPTURE_BIT); no Content field needed.
	//************************************
	[[nodiscard]] static inline bool IsCapture(_In_ const Move& move) noexcept
	{
		return (move.flags() & MoveFlags::CAPTURE_BIT) != 0;
	}

	//************************************
	// Method:      IsPromote
	// Returns:     true for all promotion types (quiet and capture), i.e. flag bit 3 set.
	//************************************
	[[nodiscard]] static inline bool IsPromote(_In_ const Move& move) noexcept
	{
		return (move.flags() & MoveFlags::PROMOTION_BIT) != 0;
	}

	[[nodiscard]] static inline bool IsEnPassant(_In_ const Move& move) noexcept
	{
		return AsType(move) == MoveType::EP_CAPTURE;
	}

	[[nodiscard]] static inline bool IsCastling(_In_ const Move& move) noexcept
	{
		MoveType type = AsType(move);
		return (type == MoveType::QUEEN_CASTLE)
			|| (type == MoveType::KING_CASTLE);
	}

	[[nodiscard]] static inline eSquare GetEnPassantSquare(_In_ const Move& move, _In_ ePiece movPiece) noexcept
	{
		if(AsType(move) != MoveType::DOUBLE_PAWN_PUSH)
			return NO_SQUARE;
		return ( PieceHelper::Color(movPiece) == WHITE ?
			SquareHelper::Calc(move.to(), +ONE_ROW) :
			SquareHelper::Calc(move.to(), -ONE_ROW));
	}

	[[nodiscard]] static inline bool IsEmpty(_In_ const Move& move ) noexcept
	{
		return move.IsEmpty();
	}

	// content: the captured piece (obtain via Board::GetCapturedPiece; Phase 4: no Content field).
	[[nodiscard]] static bool IsValid(_In_ const Move& move, _In_ ePiece movPiece, _In_ ePiece content) noexcept
	{
		if( IsEmpty( move ) )
			return false;
		if( move.to() == move.from())
			return false;
		if ((PieceHelper::IsActual(content)) && (PieceHelper::Color(movPiece) == PieceHelper::Color(content)))
			return false;	// Cannot capture own piece
		if( PieceHelper::IsKing(content))	// Cannot take a King
			return false;
		MoveType type = AsType(move);
		if( ((type == MoveType::EP_CAPTURE) || (type == MoveType::DOUBLE_PAWN_PUSH)) && !IsPawnMove(move, movPiece))
			return false;
		switch (type)
		{
		case MoveType::DOUBLE_PAWN_PUSH:
			assert(!PieceHelper::IsActual(content));	// ingen slag
			assert(IsPawnMove(move, movPiece));
			break;
		case MoveType::EP_CAPTURE:
			assert(IsPawnMove(move, movPiece));
			break;
		case MoveType::KING_CASTLE:
			assert(move.from() == e1 || move.from() == e8);	// must be in starting position
			switch (move.to())
			{
			case g1:	// Short castling
			case g8:
				break;
			default:	// Unknown castling ? ;-)
				assert(!"Invalid castling 'to'-field");
				break;
			}
			break;
		case MoveType::QUEEN_CASTLE:
			assert(move.from() == e1 || move.from() == e8);	// must be in starting position
			switch (move.to())
			{
			case c1:	// Long castling
			case c8:
				break;
			default:	// Unknown castling ? ;-)
				assert(!"Invalid castling 'to'-field");
				break;
			}
			break;
		case MoveType::QUIET:	// Default case
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
} // namespace MoveHelper

#pragma warning (pop)
