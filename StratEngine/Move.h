#pragma once

#include <deque>		// For PVLine

#include "PieceHelper.h"

class Move final
{
	// Prints content to stream - Implemented in .cpp
	friend std::ostream& operator<<(std::ostream&, _In_ const Move& move);
public:
	// Copy constructor
	Move(_In_ const Move& rhs) noexcept = default;

	explicit Move( _In_  ePiece movPiece ) noexcept
		: MovPiece(movPiece)
	{
	}

	Move() noexcept = default;	// default constructor

	// Move constructor
	Move(Move&& other) noexcept
	{
		*this = std::move(other);
	}

	// Move assignment constructor
	Move& operator=(Move&& other) noexcept
	{
		if (this != &other)
		{
			// Copy from the other object
			From = other.From;
			To = other.To;
			Type = other.Type;
			MovPiece = other.MovPiece;
			Content = other.Content;
			IsCheck = other.IsCheck;

			// We do not care about the other object now 
		}
		return *this;
	}

	Move(eSquare to, eSquare from, MoveType type, ePiece movPiece, ePiece content) noexcept
		:From(from), To(to), Type(type), MovPiece(movPiece), Content(content)
	{
	}

	~Move() noexcept = default;

	// Copy assignment operator
	Move& operator= (_In_ const Move &rhs) noexcept = default;
		
	bool operator!() const noexcept {
		return IsEmpty();
	}

	bool IsEmpty() const noexcept {
		return (To == NO_SQUARE || From == NO_SQUARE);
	}

	// IsGreater operator: Bruges til at sortere slagene
	friend bool operator> (_In_ const Move& lhs, _In_ const Move &rhs) noexcept
	{
		return Value(lhs) > Value(rhs);
	}

	// Return an evaluation of the material value consequences of the Move
	// Note: Currently only used for capture move sorting, i.e. Normal, PawnTwoForward and Castling are not being used!
	static int Value(_In_ const Move& move) noexcept
	{
		// Algorithm:
		// ----------
		// Primarily: return material value of any captured piece minus the value of the moving piece
		// That way we can rank the interesting captures and get the pawn-takes-bishop before queen-takes-bishop
		// Note: We are dividing the move value by 16 (first binary value over Queen factor) to reflect that
		// 1) queen-takes-queen is way more interesting avenue than bishop takes pawn
		// 2) queen-takes-pawn (900 and 100) is more interesting than pawn-moves-normally (100) and 
		// 3) queen-takes-queen (900-900) is more interest than pawn-takes-pawn (100-100), but also
		// 4) Pawn move (0-100/16) is more interesting than Rook move (0 - 500/16)
		// Formula: Captured piece value + (Promotion value diff) - Moving piece/16
		// Example: Queen takes Pawn: 100 - 900/16 ~= +43.75, Pawn move: 0 - 100/16 ~= -6.6
		// Best move is then pawn-takes-queen and gets promoted to a Queen: 900 + (900 - 100) - 100/16 = ~1693
		
		int captureScore = 0;
		const auto movingPieceScore = PieceHelper::Value(move.MovPiece) >> 4; 
		switch(move.Type)
		{
		case MoveType::Normal:
		case MoveType::PawnTwoForward:
		case MoveType::Castling:
			return (movingPieceScore*-1);	// TODO: Check what value this provides castling? Not used atm
		case MoveType::Capture:
		case MoveType::En_Passant:
			captureScore = PieceHelper::Value(move.Content);
			//std::assert(captureScore == PieceHelper::Value(ePiece::WHITE_PAWN));
			return captureScore - movingPieceScore;
		case MoveType::PromoteCapture: // +: Queen (etc) and any Capture value; -: Pawn value
			captureScore = PieceHelper::Value(move.Content) + PieceHelper::Value(move.MovPiece) - PieceHelper::Value(ePiece::WHITE_PAWN);
			return captureScore - static_cast<int>(g_iPieceValues[PAWN] >> 4); // Promote is encoded differently. Subtract the known moving piece value
		case MoveType::Promote: // +: Queen (etc); -: Pawn value
			captureScore = PieceHelper::Value(move.MovPiece) - PieceHelper::Value(ePiece::WHITE_PAWN);
			return captureScore - static_cast<int>(g_iPieceValues[PAWN] >> 4); // Promote is encoded differently. Subtract the known moving piece value
		/*default:
			assert(!"missing a Move state"); */
		}
		return 0;
	}

	bool operator==(_In_ const Move &rhs) const noexcept
	{
		return IsSameAs(rhs);
	}

	bool operator!=(_In_ const Move &rhs ) const noexcept
	{
		return !IsSameAs(rhs);
	}

	bool IsSameAs(_In_ const Move &rhs) const noexcept
	{
		return ((To == rhs.To) && (From == rhs.From));
	}

	void Clear() noexcept
	{
		SetMove(NO_SQUARE, NO_SQUARE, MoveType::Normal, ePiece::NO_PIECE, ePiece::NO_PIECE, false);
	}

	std::string Output() const;

	
// Public Variable
	eSquare From{ NO_SQUARE };		// Den gamle placering [a8(0)-h1(63)]: NO_SQUARE = -1
	eSquare To{ NO_SQUARE };			// Den nye placering   [a8(0)-h1(63)]: NO_SQUARE = -1
	MoveType Type	{ MoveType::Normal };		// Type of move
	ePiece MovPiece{ NO_PIECE };		// BrikType og -farve 
	ePiece Content{ NO_PIECE };		// Den slagne brik	
	bool IsCheck{ false };		// Is this move a Checking move? TODO: Shouldn't really be in the Move itself (unless we add a flags field with extra metadata)

	static const Move& EmptyMove() noexcept
	{
		static const Move move;
		return move;
	}

private:
	void SetMove(eSquare to, eSquare from, MoveType moveType,
		ePiece iMovPiece, ePiece iContent, bool inCheck) noexcept
	{
		To = to;
		From = from;
		Type = moveType;
		MovPiece = iMovPiece;
		Content = iContent;
		IsCheck = inCheck;
	}
};
// End Class Move

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
	GameStates gameState{ GameStates::STILL_PLAYING };
	eSquare epSquare{ NO_SQUARE };
	bool whiteLongCastle {true};
	bool whiteShortCastle{ true };
	bool blackLongCastle{ true };
	bool blackShortCastle{ true };
	Move lastMove;
	int fiftyCount{ 0 };
	int fullMoveCount{ 0 };
	GameInfo() noexcept = default;
};

using MoveList = std::vector<Move>;

// Principal variation line
// Keeps the best variant in the vector
using PVLine = std::deque<Move>;
std::ostream& operator<<(std::ostream&, _In_ const PVLine&);

