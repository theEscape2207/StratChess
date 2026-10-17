#pragma once

#include "Move.h"
#include "GameState.h"

#include "Utils/BitTools.h"

class Board;

class MoveGenerator final {
  public:
	// Computes all the pseudo legal piece capturing moves on the current board - no check for Check here!
	static void ComputeCaptures(const Board& board, MoveList& moveList);
	// Appends the pseudo-legal moves ComputeCaptures() leaves out -- pushes, officer moves to empty
	// squares and castling -- in their ComputeLegalMoves() order, so ComputeCaptures() followed by
	// ComputeQuiets() is ComputeLegalMoves() stably partitioned by capture-or-promotion.
	static void ComputeQuiets(const Board& board, MoveList& moveList);
	// Computes all the pseudo legal moves on the current board - no check for Check here!
	static void ComputeLegalMoves(const Board& board, MoveList& moveList);
	// Every legal move of a side to move that is in check, and nothing else: exactly the moves of
	// ComputeLegalMoves() that DoMove() accepts, in the same relative order, so the sorter's
	// generation-index tie-break orders them as it orders ComputeLegalMoves(). Precondition:
	// board.InCheck().
	static void ComputeEvasions(const Board& board, MoveList& moveList);

	// Returns BITBOARD with all squares attacked by 'Color'
	static BITBOARD GetAttackBoard(const Board& board, eColor) noexcept;

	// Returns the pieces of BOTH colours attacking `square`, with sliders traced through the
	// supplied `occupancy` rather than through the board's own. The occupancy is a parameter
	// because SEE walks a swap list on a mutated copy of it, uncovering x-ray attackers as each
	// piece leaves the board. Callers that only care about the position as it stands pass
	// bbBitBoards[ALL_PIECES].
	//
	// The result is not masked by `occupancy`: a piece already removed from it is still listed
	// if its bitboard says it attacks the square. Callers that mutate the occupancy must mask.
	// En passant is not modelled — the pawn that could capture en passant does not attack the
	// square its victim stands on.
	static BITBOARD AttackersTo(const BITBOARD* bbBitBoards, eSquare square, BITBOARD occupancy) noexcept;

	~MoveGenerator() = default;

  private:
	MoveGenerator() = default;

	// The generators below emit moves only for pieces on `movers` and only to squares on `targets`.
	// Masking removes moves without reordering the rest, which is what keeps ComputeEvasions() in
	// ComputeLegalMoves() order. `epFrom` is the pawns allowed to capture en passant.
	static void GeneratePawnCaptures(const Board& board, const BITBOARD* const bbBitBoards, MoveList& moveList,
	                                 eColor color, BITBOARD movers, BITBOARD targets, BITBOARD epFrom);

	static void GeneratePawnNormalMoves(const BITBOARD* const bbBitBoards, eColor color, MoveList& moveList,
	                                    BITBOARD movers, BITBOARD targets);

	static void GenerateOfficerMoves(const Board& board, const BITBOARD* const bbBitBoards, MoveList& moveList,
	                                 ePieceType piece, eColor color, BITBOARD movers, BITBOARD targets);

	static void AddOfficerMoves(const Board& board, MoveList& moveList, BITBOARD bbAttack, eSquare from);
	static void AddPawnPromoteMoves(const BITBOARD* bbBitBoards, eColor color, MoveList& moveList, BITBOARD movers,
	                                BITBOARD targets);
	// board is only referenced inside assert() checks — compiled out in Release (NDEBUG).
	static void AddPawnCaptures([[maybe_unused]] const Board& board, MoveList& moveList, const BITBOARD*, Move pawnMove,
	                            eColor color, BITBOARD epFrom);
	static void AddCastleMoves(const Board& board, MoveList& moveList, eColor color, const BITBOARD* bbBitBoards);

	static BITBOARD GetAnyEnPassantAttackingPawns(const BITBOARD* bbBitBoards, eColor attackByColor,
	                                              eSquare epSquare) noexcept;

	// Returns true if an enemy piece is actually standing on move.to(). Distinguishes a normal
	// pawn capture from an en-passant capture: GeneratePawnCaptures tags every diagonal pawn move
	// as MoveType::CAPTURE before it's known whether the target square is occupied, so the move's
	// own type flag can't tell the two apart. Not to be confused with MoveHelper::IsCapture(),
	// which only reads that flag.
	static bool IsEnemyPieceOnTarget(const BITBOARD* bbBitBoards, eColor color, const Move& move) noexcept
	{
		return Bits::isAnyBitSet(bbBitBoards[ePiece::ALL_BLACK_PIECES - static_cast<bool>(color)], g_bbMask[move.to()]);
	}

	static BITBOARD GetOfficerAttackBoard(const BITBOARD* bbBitBoards, eSquare from, ePiece piece) noexcept;
	static BITBOARD GetRookBitboard(const BITBOARD* bbBitBoards, eSquare from, eColor color) noexcept;
	static BITBOARD GetBishopBitboard(const BITBOARD* bbBitBoards, eSquare from, eColor color) noexcept;

	static constexpr bool IsAnyBackRow(eSquare field) noexcept
	{
		return (Rank(field) == eRowNames::WHITE_BACK_ROW || Rank(field) == eRowNames::BLACK_BACK_ROW);
	}
};
