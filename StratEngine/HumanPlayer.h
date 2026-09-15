#pragma once

#include "defines.h"
#include "IPlayer.h"

#include <unordered_map>
#include <sstream>

class Move;
class Board;

class HumanPlayer final : public IPlayer {
  public:
	/* IPlayer implementation */
	SearchResult GetMove(const SearchLimits&) override;
	const char* GetType() const noexcept override { return "Human"; }
	bool IsHuman() const noexcept override { return true; }

	std::string getDescription() const override
	{
		std::stringstream sstream;
		sstream << "\n\tPlayer type:\t" << GetType() << '\n';
		return sstream.str();
	}

	/* End IPlayer implementation */
	explicit HumanPlayer(Board& board) noexcept : board_(board) {}
	~HumanPlayer() final = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	HumanPlayer(const HumanPlayer&) = delete;
	HumanPlayer& operator=(const HumanPlayer&) = delete;
	HumanPlayer(HumanPlayer&&) = delete;
	HumanPlayer& operator=(HumanPlayer&&) = delete;

  private:
	Board& board_;

	/* Helpers */

	using MapPieces = std::unordered_map<char, ePieceType>;

	// The user has specified a promotion and we know that its one of the allowed ones!

	static MapPieces& GetPromoteMap()
	{
		static MapPieces promoteMap{// expects lower case input
		                            {'q', QUEEN},
		                            {'r', ROOK},
		                            {'b', BISHOP},
		                            {'n', KNIGHT}};
		return promoteMap;
	}

	// Validates the user input using regex
	static bool ValidateInput(const std::string& strInput);
	// Returns true if we are trying a promotion; in that case sets promotedType.
	static bool ParseInput(const std::string& input, Move& move, ePieceType& promotedType);

	// We know a lot - ignoring the optional '-' and promote chars
	static eSquare GetSquare(std::string::const_iterator& begin, const std::string::const_iterator& end)
	{
		const int row = *begin++ - 'a';
		const int col = ((8 - ((*begin) - '0')) << 3);
		// If its a '-' ignore it!
		++begin;
		if (begin != end && *begin == '-')
			++begin;
		return static_cast<eSquare>(col + row);
	}

	// Fetches the list of Moves for the Player
	// Returns true if any legal move is found, and false otherwise
	static bool IsAnyLegalMoves(Board& board, MoveList& moveList);
};
