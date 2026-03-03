#pragma once

#include "Move.h"
#include "PieceHelper.h"

// Small collection of helpers to construct Moves in a clear, centralized way.
// Phase 3: MovPiece has been removed from the Move struct. Factory functions no longer
// accept or store the moving piece; callers obtain it via Board::GetEffectiveMovPiece().
namespace MoveFactory {

	// General make-move helper. `captured` defaults to NO_PIECE.
	inline Move MakeMove(eSquare from, eSquare to, MoveType moveType = MoveType::QUIET, ePiece captured = ePiece::NO_PIECE) noexcept
	{
		return Move(from, to, moveType, captured);
	}

	inline Move MakeQuiet(eSquare from, eSquare to) noexcept
	{
		return Move(from, to, MoveType::QUIET);
	}

	inline Move MakeCapture(eSquare from, eSquare to, ePiece captured) noexcept
	{
		return Move(from, to, MoveType::CAPTURE, captured);
	}

	// promotedPiece is still needed to derive the correct MoveType flag.
	inline Move MakePromotion(eSquare from, eSquare to, ePiece promotedPiece, ePiece captured = ePiece::NO_PIECE) noexcept
	{
		MoveType type = MoveType::PROMOTION_QUEEN;
		const auto pt = static_cast<ePieceType>(promotedPiece & ~1); // strip color bit
		switch (pt) {
		case QUEEN:  type = MoveType::PROMOTION_QUEEN;  break;
		case ROOK:   type = MoveType::PROMOTION_ROOK;   break;
		case BISHOP: type = MoveType::PROMOTION_BISHOP; break;
		case KNIGHT: type = MoveType::PROMOTION_KNIGHT; break;
		default: break;
		}
		return Move(from, to, type, captured);
	}

	inline Move MakeEnPassant(eSquare from, eSquare to, ePiece captured) noexcept
	{
		return Move(from, to, MoveType::EP_CAPTURE, captured);
	}

} // namespace MoveFactory
