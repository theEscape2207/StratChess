#pragma once

#include "Move.h"
#include "PieceHelper.h"

// Small collection of helpers to construct Moves in a clear, centralized way.
namespace MoveFactory {

	// General make-move helper. `captured` defaults to NO_PIECE.
	inline Move MakeMove(eSquare from, eSquare to, ePiece movPiece, MoveType moveType = MoveType::QUIET, ePiece captured = ePiece::NO_PIECE) noexcept
	{
		Move m(from, to, moveType, movPiece, captured);
		return m;
	}
	inline Move MakeQuiet(eSquare from, eSquare to, ePiece movPiece) noexcept
	{
		return MakeMove(from, to, movPiece, MoveType::QUIET, ePiece::NO_PIECE);
	}

	inline Move MakeCapture(eSquare from, eSquare to, ePiece movPiece, ePiece captured) noexcept
	{
		return MakeMove(from, to, movPiece, MoveType::CAPTURE, captured);
	}

	inline Move MakePromotion(eSquare from, eSquare to, ePiece promotedPiece, ePiece captured = ePiece::NO_PIECE) noexcept
	{
		MoveType type = MoveType::PROMOTION_QUEEN;
		const auto pt = static_cast<ePieceType>(promotedPiece & ~1); // strip color bit
		switch (pt) {
		case QUEEN: type = MoveType::PROMOTION_QUEEN; break;
		case ROOK: type = MoveType::PROMOTION_ROOK; break;
		case BISHOP: type = MoveType::PROMOTION_BISHOP; break;
		case KNIGHT: type = MoveType::PROMOTION_KNIGHT; break;
		default: break;
		}
		return MakeMove(from, to, promotedPiece, type, captured);
	}

	inline Move MakeEnPassant(eSquare from, eSquare to, ePiece movPiece, ePiece captured) noexcept
	{
		return MakeMove(from, to, movPiece, MoveType::EP_CAPTURE, captured);
	}

} // namespace MoveFactory
