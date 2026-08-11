#pragma once

#include "Move.h"
#include "PieceHelper.h"

// Small collection of helpers to construct Moves in a clear, centralized way.
// Neither the moving piece nor the captured piece is stored in the Move struct.
//   Captured-piece information is tracked in Board::capturedHistory_[] during DoMove/UndoMove.
//   Whether a move captures is encoded in the MoveType flag (CAPTURE_BIT, bit 2).
namespace MoveFactory {

// General make-move helper. Only the move type matters now — no captured piece stored.
inline Move MakeMove(eSquare from, eSquare to, MoveType moveType = MoveType::QUIET) noexcept
{
	return Move(from, to, moveType);
}

inline Move MakeQuiet(eSquare from, eSquare to) noexcept
{
	return Move(from, to, MoveType::QUIET);
}

// CAPTURE flag encodes the capture semantics; the actual captured piece is retrieved
// from the board (Board::GetCapturedPiece) when needed for sorting or undo.
inline Move MakeCapture(eSquare from, eSquare to) noexcept
{
	return Move(from, to, MoveType::CAPTURE);
}

// promotedPiece is still needed to derive the correct MoveType flag.
// isCapture=true selects the PROMOTION_*_CAPTURE variant (bits 3+2 both set).
inline Move MakePromotion(eSquare from, eSquare to, ePiece promotedPiece,
                          bool isCapture = false) noexcept
{
	MoveType type = isCapture ? MoveType::PROMOTION_QUEEN_CAPTURE : MoveType::PROMOTION_QUEEN;
	const auto pt = static_cast<ePieceType>(promotedPiece & ~1); // strip color bit
	switch (pt) {
	case QUEEN:
		type = isCapture ? MoveType::PROMOTION_QUEEN_CAPTURE : MoveType::PROMOTION_QUEEN;
		break;
	case ROOK:
		type = isCapture ? MoveType::PROMOTION_ROOK_CAPTURE : MoveType::PROMOTION_ROOK;
		break;
	case BISHOP:
		type = isCapture ? MoveType::PROMOTION_BISHOP_CAPTURE : MoveType::PROMOTION_BISHOP;
		break;
	case KNIGHT:
		type = isCapture ? MoveType::PROMOTION_KNIGHT_CAPTURE : MoveType::PROMOTION_KNIGHT;
		break;
	default:
		break;
	}
	return Move(from, to, type);
}

// EP_CAPTURE flag encodes the en-passant semantics; the actual captured pawn is derived
// from the board position (OppositePawn(sideToMove)) when needed.
inline Move MakeEnPassant(eSquare from, eSquare to) noexcept
{
	return Move(from, to, MoveType::EP_CAPTURE);
}

} // namespace MoveFactory
