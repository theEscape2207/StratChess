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

namespace MoveHelper
{
	/*
	*	MovPiece methods
	*/

	[[nodiscard]] static inline bool IsMoveType(_In_ const Move& move, ePieceType type) noexcept
	{
		return PieceHelper::IsOfType(move.MovPiece, type);
	}

	[[nodiscard]] static inline bool IsMovingPiece(_In_ const Move& move, ePiece type) noexcept
	{
		return PieceHelper::IsOfPiece(move.MovPiece, type); 
	}

	// Is this piece moving from this square?
	[[nodiscard]] static inline bool IsPieceMovingFrom(_In_ const Move& move, ePiece type, eSquare square) noexcept
	{
		return (PieceHelper::IsOfPiece(move.MovPiece, type) && (move.From == square));
	}

	// Is this piece capture from this square?
	[[nodiscard]] static inline bool IsPieceCapturedAt(_In_ const Move& move, ePiece type, eSquare square) noexcept
	{
		return (PieceHelper::IsOfPiece(move.Content, type) && (move.To == square));
	}

	// Is the moving piece a pawn
	[[nodiscard]] static inline bool IsPawnMove(_In_ const Move& move ) noexcept
	{
		return PieceHelper::IsPawn(move.MovPiece);
	}

	[[nodiscard]] static inline bool IsKingMove(_In_ const Move& move ) noexcept
	{
		return PieceHelper::IsKing(move.MovPiece);
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
		switch( move.Type )
		{
		case MoveType::Capture:
		case MoveType::En_Passant:
		case MoveType::PromoteCapture:
			assert( PieceHelper::IsActual(move.Content) );
			return true;
		case MoveType::Promote:
			assert(!PieceHelper::IsActual(move.Content));
			return false;
		default: 
			assert(!PieceHelper::IsActual(move.Content));
			return false;
		}
	}

	[[nodiscard]] static bool IsCapture(_In_ MoveType type) noexcept
	{
		switch (type)
		{
		case MoveType::Capture:
		case MoveType::En_Passant:
		case MoveType::PromoteCapture:
			return true;
		case MoveType::Promote:
			return false;
		default:
			return false;
		}
	}

	[[nodiscard]] static inline bool IsPromote(_In_ const Move& move ) noexcept
	{
		return (move.Type == MoveType::Promote) || 
			(move.Type == MoveType::PromoteCapture);
	}

	[[nodiscard]] static inline bool IsEnPassant(_In_ const Move& move) noexcept
	{
		return move.Type == MoveType::En_Passant;
	}

	[[nodiscard]] static inline bool IsCastling(_In_ const Move& move) noexcept
	{
		return move.Type == MoveType::Castling;
	}

	[[nodiscard]] static eSquare GetEnPassantSquare(_In_ const Move& move ) noexcept
	{
		if(move.Type != MoveType::PawnTwoForward)
			return NO_SQUARE;
		return ( PieceHelper::Color( move.MovPiece) == WHITE ? 
			SquareHelper::Calc(move.To, +ONE_ROW) : 
			SquareHelper::Calc(move.To, -ONE_ROW));
	}

	[[nodiscard]] static inline bool IsEmpty(_In_ const Move& move ) noexcept
	{
		return (move.To == NO_SQUARE) || (move.From == NO_SQUARE);
	}

	[[nodiscard]] static bool IsValid(_In_ const Move& move ) noexcept
	{
		if( IsEmpty( move ) )
			return false;
		if( move.To == move.From )
			return false;
		if( PieceHelper::IsActual( move.Content) && (PieceHelper::Color(move.MovPiece) == PieceHelper::Color(move.Content) ) )
			return false;
		if( PieceHelper::IsKing(move.Content))	// Cannot take a King
			return false;
		if( ((move.Type == MoveType::En_Passant) || (move.Type == MoveType::PawnTwoForward)) && !IsPawnMove(move))
			return false;
		switch (move.Type)
		{
		case MoveType::PawnTwoForward:
			assert(!PieceHelper::IsActual( move.Content));	// ingen slag
			assert(IsPawnMove(move));
			break;
		case MoveType::En_Passant:
			assert(IsPawnMove(move));
			break;
		case MoveType::Castling:
			assert(move.From == e1 || move.From == e8);	// must be in starting position
			switch (move.To)
			{
			case g1:	// Short castling
			case g8:
				break;
			case c1:	// Long castling
			case c8:
				break;
			default:	// Unknown castling ? ;-)
				assert(!"Invalid castling 'to'-field");
				break;
			}
			break;
		case MoveType::Normal:	// Default case
		case MoveType::Capture:
		case MoveType::Promote:
		case MoveType::PromoteCapture:
			break;
		}
		return true;
	}
} // namespace MoveHelper

#pragma warning (pop)
