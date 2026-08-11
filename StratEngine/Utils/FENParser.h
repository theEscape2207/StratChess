#pragma once

#include "defines.h"
#include "GameState.h"
#include "Config.h"
#include <cstdint>
#include <string>
#include <vector>
#include <tuple>
#include <optional>

class Board;

// < FEN > :: = < Piece Placement>
// ' ' < Side to move >
// ' ' < Castling ability >
// ' ' < En passant target square >
// ' ' < Halfmove clock >
// ' ' < Fullmove counter >
class FENParser final
{
public:
	// Standalone structure for FEN game state
	struct FENGameState
	{
		eColor sideToMove{ eColor::WHITE };
		eSquare epSquare{ NO_SQUARE };
		uint8_t castlingRights{ CastlingRights::ALL };
		int halfMoveClock{ 0 };
		int fullMoveCounter{ 1 };
	};

	// Primary interface - parse FEN into standalone structures
	// This is what Board::SetupFromFEN() should call
	static std::optional<std::string> ParseFEN(
		const std::string& fen, 
		FENGameState& outState, 
		std::vector<std::tuple<ePiece, eSquare>>& outPieces) noexcept;

	// Validate FEN metadata against actual board state
	static bool ValidatePositionAgainstFENMetadata(
		const Board& board,
		FENGameState& state) noexcept;

	// Utility: Convert FENGameState to Config::GameConfig (for backward compatibility)
	static void ToGameConfig(const FENGameState& state, Config::GameConfig& outConfig) noexcept;
	
	// Utility: Convert Config::GameConfig to FENGameState
	static FENGameState FromGameConfig(const Config::GameConfig& config) noexcept;

private:
	// Parse only the piece placement field (the part before the first space).
	// Fills outVec with (piece, square) tuples in the same square-indexing used in Board.
	// On success returns std::nullopt. On failure returns an error message.
	static std::optional<std::string> ParsePiecePlacementField(
		const std::string& placement, 
		std::vector<std::tuple<ePiece, eSquare>>& outVec) noexcept;

	// Convert a 2-char square like "e3" to eSquare. Returns NO_SQUARE on invalid input.
	static eSquare SquareFromString(const std::string& s) noexcept;

	// Parse FEN castling string format either "KQkq" or "-" if no more available.
	static std::optional<std::string> PopulateCastlingFlags(
		const std::string& castling, 
		uint8_t& outRights) noexcept;
};
