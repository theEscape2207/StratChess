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
	// flags() is four bits, and MoveType deliberately leaves 6 and 7 unnamed — move generation
	// never emits them (see MoveFlags in Move.h). The cast is therefore out of range for those two
	// values on purpose: every switch on the result falls through to its own default, which is the
	// behaviour MoveFieldTests freezes for ToCoord. Converting this to a checked cast or asserting
	// the range would break that contract, so the analyzer finding is suppressed rather than fixed.
	[[nodiscard]] inline MoveType AsType(const Move& move) noexcept
	{
		// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
		return static_cast<MoveType>(move.flags());
	}

	// Is the moving piece a pawn? The moving piece is not stored in Move; callers supply it.
	[[nodiscard]] inline bool IsPawnMove(ePiece movPiece) noexcept { return PieceHelper::IsPawn(movPiece); }

	// True if the move captures a piece (CAPTURE, EP_CAPTURE, or any PROMOTION_*_CAPTURE), read
	// purely from flag bit 2 (CAPTURE_BIT).
	[[nodiscard]] inline bool IsCapture(const Move& move) noexcept
	{
		return (move.flags() & MoveFlags::CAPTURE_BIT) != 0;
	}

	// True for all promotion types (quiet and capture), i.e. flag bit 3 set.
	[[nodiscard]] inline bool IsPromote(const Move& move) noexcept
	{
		return (move.flags() & MoveFlags::PROMOTION_BIT) != 0;
	}

	[[nodiscard]] inline bool IsEnPassant(const Move& move) noexcept { return AsType(move) == MoveType::EP_CAPTURE; }

	[[nodiscard]] inline eSquare GetEnPassantSquare(const Move& move, ePiece movPiece) noexcept
	{
		if (AsType(move) != MoveType::DOUBLE_PAWN_PUSH)
			return NO_SQUARE;
		return (PieceHelper::Color(movPiece) == WHITE ? SquareHelper::Calc(move.to(), +ONE_ROW)
		                                              : SquareHelper::Calc(move.to(), -ONE_ROW));
	}

	// content: the captured piece (obtain via Board::GetCapturedPiece before DoMove).
	// Used only inside Board's make/unmake asserts.
	[[nodiscard]] inline bool IsValid(const Move& move, ePiece movPiece, ePiece content) noexcept
	{
		if (move.is_null())
			return false;
		if (move.to() == move.from())
			return false;
		if ((PieceHelper::IsActual(content)) && (PieceHelper::Color(movPiece) == PieceHelper::Color(content)))
			return false;                 // Cannot capture own piece
		if (PieceHelper::IsKing(content)) // Cannot take a King
			return false;
		const MoveType type = AsType(move);
		if (((type == MoveType::EP_CAPTURE) || (type == MoveType::DOUBLE_PAWN_PUSH)) && !IsPawnMove(movPiece))
			return false;
		switch (type) {
		case MoveType::DOUBLE_PAWN_PUSH:
			assert(!PieceHelper::IsActual(content));
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
			default:
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
			default:
				assert(!"Invalid castling 'to'-field");
				break;
			}
			break;
		case MoveType::QUIET:
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
	// content:  the captured piece (obtain via Board::GetCapturedPiece before DoMove; NO_PIECE if quiet).
	//
	// Formula: Captured piece value + (Promotion value diff) - Moving piece/16
	// Rationale: ranks pawn-takes-bishop above queen-takes-bishop; a quiet pawn move scores lower
	// than a quiet rook move (negative, scaled by 1/16 of piece value).
	[[nodiscard]] inline int Value(const Move& move, ePiece movPiece, ePiece content) noexcept
	{
		int captureScore = 0;
		const auto movingPieceScore = PieceHelper::Value(movPiece) >> 4;
		switch (AsType(move)) {
		case MoveType::QUIET:
		case MoveType::DOUBLE_PAWN_PUSH:
		case MoveType::QUEEN_CASTLE:
		case MoveType::KING_CASTLE:
			return -movingPieceScore;
		case MoveType::CAPTURE:
		case MoveType::EP_CAPTURE:
			captureScore = PieceHelper::Value(content);
			// The king's LVA weight is capped at a queen's rather than taken from its 10000 cp
			// notional value, which would score KxR at 500 - 625 = -125 and file the best move in
			// the position below every quiet one. It is capped, not dropped: a legal king capture
			// is unopposed and would deserve the victim outright, but move generation is
			// pseudo-legal, so this is also reached for king captures DoMove will reject. Capping
			// keeps a king capture among the winning captures and behind an equally valuable one by
			// a cheaper attacker, without asserting a legality this function cannot see.
			if (PieceHelper::IsKing(movPiece))
				return captureScore - static_cast<int>(PieceHelper::Value(ePiece::WHITE_QUEEN) >> 4);
			return captureScore - movingPieceScore;
		case MoveType::PROMOTION_KNIGHT:
		case MoveType::PROMOTION_BISHOP:
		case MoveType::PROMOTION_ROOK:
		case MoveType::PROMOTION_QUEEN:
		case MoveType::PROMOTION_KNIGHT_CAPTURE:
		case MoveType::PROMOTION_BISHOP_CAPTURE:
		case MoveType::PROMOTION_ROOK_CAPTURE:
		case MoveType::PROMOTION_QUEEN_CAPTURE: // +: promoted piece value gain; +: captured piece; -: pawn value
			captureScore = PieceHelper::Value(movPiece) - PieceHelper::Value(ePiece::WHITE_PAWN);
			if (PieceHelper::IsActual(content))
				captureScore += PieceHelper::Value(content);
			return captureScore - static_cast<int>(g_iPieceValues[PAWN] >> 4);
		}
		return 0;
	}

	// Optimistic material-gain bound; unlike Value(), it never subtracts the attacker.
	[[nodiscard]] inline int DeltaGain(const Move& move, ePiece movPiece, ePiece content) noexcept
	{
		int gain = PieceHelper::IsActual(content) ? PieceHelper::Value(content) : 0;
		if (IsPromote(move))
			gain += PieceHelper::Value(movPiece) - PieceHelper::Value(ePiece::WHITE_PAWN);
		return gain;
	}

} // namespace MoveHelper
