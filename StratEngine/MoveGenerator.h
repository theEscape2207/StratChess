#pragma once

#include "Move.h"
#include "GameState.h"

#include "Utils/BitTools.h"

class MoveGenerator final
{
public:
	// Computes all the pseudo legal piece capturing moves on the current board - no check for Check here!
	static void ComputeCaptures(_In_ const GameInfo& info, _Inout_ MoveList& moveList);
	// Computes all the pseudo legal moves on the current board - no check for Check here!
	static void ComputeLegalMoves(_In_ const GameInfo& info, _Inout_ MoveList& moveList);

	// Returns BITBOARD with all squares attacked by 'Color'
	static BITBOARD GetAttackBoard( eColor ) noexcept;		// TODO: This is used directly in Board?!?

	~MoveGenerator() = default;
private:
	MoveGenerator() = default;

	// Fungerer ved at kalde GetAttackBoard() og se om et af dem rammer 'pos'
	static bool IsAttacked(eSquare pos, eColor attackByColor) noexcept;

	static bool IsAttacked(BITBOARD squares, eColor attackByColor) noexcept;

	static void GeneratePawnCaptures(const BITBOARD * const bbBitBoards, const GameInfo & info, MoveList & moveList, eColor color);

	static void GeneratePawnNormalMoves(_In_ const BITBOARD * const bbBitBoards, _In_ eColor color, _Inout_ MoveList& moveList);

	static void GenerateOfficerMoves(const BITBOARD * const bbBitBoards, MoveList& moveList, ePieceType piece, eColor color, bool onlyCaptures );
	
	static void AddOfficerMoves(MoveList& moveList, BITBOARD bbAttack, eSquare from);
	static void AddPawnPromoteMoves( const BITBOARD* bbBitBoards, eColor color, MoveList& moveList );
	static void AddPawnCaptures(MoveList& moveList, const BITBOARD*, Move pawnMove, eColor color);
	static void AddCastleMoves(MoveList& moveList, eColor color, const BITBOARD* bbBitBoards, const GameInfo &info);

	static BITBOARD GetAnyEnPassantAttackingPawns(eColor attackByColor, eSquare epSquare) noexcept;

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
	static BITBOARD GetTowerBitboard(const BITBOARD* bbBitBoards, eSquare from, eColor color) noexcept;
	static BITBOARD GetBishopBitboard(const BITBOARD* bbBitBoards, eSquare from, eColor color) noexcept;

	// TODO: SwapMoves is duplicated in MoveSorter
	static void SwapMoves(MoveList& moveList, size_t first, size_t second) //-V2009
	{
		if(first == second)	// no need to swap anything here :-)
			return;
		std::swap(moveList[first], moveList[second]);
	}

	static constexpr bool IsAnyBackRow(eSquare field) noexcept
	{	
		return ( Rank( field ) == eRowNames::WHITE_BACK_ROW || 
				 Rank( field ) == eRowNames::BLACK_BACK_ROW);
	}
};
