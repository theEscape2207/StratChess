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

	// Is this piece capture from this square?
	[[nodiscard]] static inline bool IsPieceCapturedAt(_In_ const Move& move, ePiece type, eSquare square) noexcept
	{
		return (PieceHelper::IsOfPiece(move.Content, type) && (move.to() == square));
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
	// Description: A move can be a capture upon
	// FullName:    public MoveHelper::IsCapture
	// Returns:     bool - returns true if the move is a Capture (also through En Passant and Promotes) and false otherwise
	// Parameter:   const Move& move - The Move
	// Remark:      Code is duplicated due to asserts being added
	//				TODO: Should refactor later to avoid code-duplication
	//************************************
	[[nodiscard]] static bool IsCapture(_In_ const Move& move ) noexcept
	{
		switch(AsType(move))
		{
		case MoveType::CAPTURE:
		case MoveType::EP_CAPTURE:
			assert( PieceHelper::IsActual(move.Content) );
			return true;
		case MoveType::QUIET:
		case MoveType::DOUBLE_PAWN_PUSH:
		case MoveType::KING_CASTLE:
		case MoveType::QUEEN_CASTLE:
			assert(!PieceHelper::IsActual(move.Content));
			return false;
		case MoveType::PROMOTION_KNIGHT:
		case MoveType::PROMOTION_BISHOP:
		case MoveType::PROMOTION_ROOK:
		case MoveType::PROMOTION_QUEEN:
			// Promotion can be both a capture and a non-capture
			return PieceHelper::IsActual(move.Content);
		default:
			assert(!PieceHelper::IsActual(move.Content));
			return false;
		}
	}

	[[nodiscard]] static inline bool IsPromote(_In_ const Move& move ) noexcept
	{
		return (AsType(move) >= MoveType::PROMOTION_KNIGHT);	// All promotion types are >= PROMOTION_KNIGHT
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

	[[nodiscard]] static bool IsValid(_In_ const Move& move, _In_ ePiece movPiece) noexcept
	{
		if( IsEmpty( move ) )
			return false;
		if( move.to() == move.from())
			return false;
		if ((PieceHelper::IsActual(move.Content)) && (PieceHelper::Color(movPiece) == PieceHelper::Color(move.Content)))
			return false;	// Cannot capture own piece
		if( PieceHelper::IsKing(move.Content))	// Cannot take a King
			return false;
		MoveType type = AsType(move);
		if( ((type == MoveType::EP_CAPTURE) || (type == MoveType::DOUBLE_PAWN_PUSH)) && !IsPawnMove(move, movPiece))
			return false;
		switch (type)
		{
		case MoveType::DOUBLE_PAWN_PUSH:
			assert(!PieceHelper::IsActual( move.Content));	// ingen slag
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
			break;
		}
		return true;
	}
} // namespace MoveHelper

#pragma warning (pop)
