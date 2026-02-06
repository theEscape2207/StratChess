// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "PlayerBase.h"
// For factory constructor
#include "AIBasic.h"
#include "AIAgent.h"
#include "ABIterative.h"
#include "ABIterTrans.h"
#include "AITrans.h"

#include "AIPerplex.h"
#include "PlayerHuman.h"

#include "MoveHelper.h"

// static Factory constructor
std::unique_ptr<PlayerBase> PlayerBase::Create(ePlayerTypes type, unsigned max_depth)
{
	switch (type) {
	case ePlayerTypes::HUMAN:				return std::make_unique<PlayerHuman>();		// no depth necessary
	case ePlayerTypes::ALPHABETA:			return std::make_unique<AIBasic>(max_depth);
	case ePlayerTypes::ABITERATING:			return std::make_unique<ABIterative>(max_depth);
	case ePlayerTypes::AIAGENT:				return std::make_unique<AIAgent>(max_depth);
	case ePlayerTypes::AITRANS:				return std::make_unique<AITrans>(max_depth);
	case ePlayerTypes::ABITERATIVE_TRANS:	return std::make_unique<ABIterTrans>(max_depth);
	case ePlayerTypes::AI_PERPLEX:			return std::make_unique<AIPerplex>(max_depth);
	default:				throw std::invalid_argument("Unknown Player type");	// Oops... we forgot an algo
	}
}

// Updates the BoardInfo with information about En Passant and castling possibilities
// For each move done, update as done above according to input move
void PlayerBase::UpdateBoardInfo(const Move &move, GameInfo& info) noexcept
{
	// Set the En Passant square
	info.epSquare = MoveHelper::GetEnPassantSquare(move);
	info.lastMove = move;

	UpdateCastlingState(move, info);

	UpdateFiftyMovesState(move, info);
}

void PlayerBase::UpdateCastlingState(const Move &move, GameInfo &info) noexcept
{
	if (MoveHelper::IsMovingPiece(move, ePiece::WHITE_KING))
	{
		// Either King in default position (and all options open) or both LC and SC options must be false already
		assert(move.From == e1 || !(info.whiteLongCastle || info.whiteShortCastle));
		info.whiteLongCastle = info.whiteShortCastle = false;
	}
	else if (MoveHelper::IsMovingPiece(move, ePiece::BLACK_KING))
	{
		// Either King in default position (and all options open) or both LC and SC options must be false already
		assert(move.From == e8 || !(info.blackLongCastle || info.blackShortCastle));
		info.blackLongCastle = info.blackShortCastle = false;
	}
	else
	{
		// No action needed
	}
	// Note: The below castle check if's cannot be part of the above king if-else if as technically a 
	// WHITE KING could move and capture a BLACK ROOK while still having black castling options available. 
	// Quite unlikely, though, but possible...

	// Is one of the Rooks moving or being captured? Then disable the castling
	if (info.whiteLongCastle &&
		(MoveHelper::IsPieceMovingFrom(move, ePiece::WHITE_ROOK, a1) ||
			MoveHelper::IsPieceCapturedAt(move, ePiece::WHITE_ROOK, a1)))
	{
		info.whiteLongCastle = false;
	}
	else if (info.whiteShortCastle &&
		(MoveHelper::IsPieceMovingFrom(move, ePiece::WHITE_ROOK, h1) ||
			MoveHelper::IsPieceCapturedAt(move, ePiece::WHITE_ROOK, h1)))
	{
		info.whiteShortCastle = false;
	}
	else
	{
		// No action needed
	}

	// must be separate checks as white rook A1 could capture black rook H1 
	if (info.blackLongCastle &&
		(MoveHelper::IsPieceMovingFrom(move, ePiece::BLACK_ROOK, a8) ||
			MoveHelper::IsPieceCapturedAt(move, ePiece::BLACK_ROOK, a8)))
	{
		info.blackLongCastle = false;
	}
	else if (info.blackShortCastle &&
		(MoveHelper::IsPieceMovingFrom(move, ePiece::BLACK_ROOK, h8) ||
			MoveHelper::IsPieceCapturedAt(move, ePiece::BLACK_ROOK, h8)))
	{
		info.blackShortCastle = false;
	}
	else
	{
		// No action needed
	}
}

void PlayerBase::UpdateFiftyMovesState(const Move & move, GameInfo &info) noexcept
{
	if (MoveHelper::IsCapture(move) || MoveHelper::IsPromote(move) || MoveHelper::IsPawnMove(move))
	{
		info.fiftyCount = 0;	// if so, then reset counter 
		assert(info.gameState == GameStates::STILL_PLAYING);
	}
	else
	{
		if (++info.fiftyCount >= 50)	// Increment and test for fifty moves
			info.gameState = GameStates::DRAW_50_MOVES;	// Should use UpdateGameState?
	}
}
