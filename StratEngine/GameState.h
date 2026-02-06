#pragma once

#include "MoveHelper.h"
#include "Move.h"
#include <cassert>

// Castling rights bit flags
namespace CastlingRights {
	constexpr uint8_t NONE = 0;
	constexpr uint8_t WHITE_KINGSIDE = 1 << 0;
	constexpr uint8_t WHITE_QUEENSIDE = 1 << 1;
	constexpr uint8_t BLACK_KINGSIDE = 1 << 2;
	constexpr uint8_t BLACK_QUEENSIDE = 1 << 3;
	constexpr uint8_t WHITE_BOTH = WHITE_KINGSIDE | WHITE_QUEENSIDE;
	constexpr uint8_t BLACK_BOTH = BLACK_KINGSIDE | BLACK_QUEENSIDE;
	constexpr uint8_t ALL = WHITE_BOTH | BLACK_BOTH;
}

// Possible game states
enum class GameStates {
	STILL_PLAYING,			// 0
	WHITE_WON,				// 1
	BLACK_WON,				// 2
	DRAW_PAT,				// 3
	DRAW_50_MOVES,			// 4
	HUMAN_EXITED			// 5
};

// Contains information about the board situation
struct GameInfo
{
	GameStates	gameState{ GameStates::STILL_PLAYING };
	eSquare		epSquare{ NO_SQUARE };
	uint8_t		castlingRights{ CastlingRights::ALL };
	Move		lastMove;
	int			fiftyCount{ 0 };
	int			fullMoveCount{ 0 };

	GameInfo() noexcept = default;
	~GameInfo() noexcept = default;

	void Reset() noexcept
	{
		gameState = GameStates::STILL_PLAYING;
		epSquare = NO_SQUARE;
		castlingRights = CastlingRights::ALL;
		lastMove.Clear();
		fiftyCount = 0;
		fullMoveCount = 1;
	}

	// Updates the BoardInfo with information about En Passant and castling possibilities
	// For each move done, update as done above according to input move
	void UpdateBoardInfo(const Move& move) noexcept
	{
		// Set the En Passant square
		epSquare = MoveHelper::GetEnPassantSquare(move);
		lastMove = move;

		UpdateCastlingState(move);

		UpdateFiftyMovesState(move);
	}

	void UpdateCastlingState(const Move& m) noexcept
	{
		auto sideToMove = PieceHelper::Color(m.MovPiece);
		if (MoveHelper::IsKingMove(m)) {
			castlingRights &= ~(sideToMove == eColor::WHITE
				? CastlingRights::WHITE_BOTH
				: CastlingRights::BLACK_BOTH);
		}

		// Strip rights based on from/to squares - covers rook moves,
		// rook captures, and any other piece capturing a rook
		static constexpr std::array<std::pair<eSquare, uint8_t>, 4> rookSquares = { {
			{ a1, CastlingRights::WHITE_QUEENSIDE },
			{ h1, CastlingRights::WHITE_KINGSIDE  },
			{ a8, CastlingRights::BLACK_QUEENSIDE },
			{ h8, CastlingRights::BLACK_KINGSIDE  }
		} };

		for (const auto& [sq, flag] : rookSquares) {
			if (m.from() == sq || m.to() == sq)
				castlingRights &= ~flag;
		}
	}

	void UpdateFiftyMovesState(const Move& move) noexcept
	{
		if (MoveHelper::IsCapture(move) || MoveHelper::IsPawnMove(move))
		{
			fiftyCount = 0;	// if so, then reset counter 
			assert(gameState == GameStates::STILL_PLAYING);
		}
		else
		{
			if (++fiftyCount >= 50)	// Increment and test for fifty moves
				gameState = GameStates::DRAW_50_MOVES;	// Should use UpdateGameState?
		}
	}
};
