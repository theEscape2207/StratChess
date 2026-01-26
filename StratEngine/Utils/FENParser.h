#pragma once

#include "Config.h"
#include <string>
#include <vector>
#include <tuple>

class FENParser final
{
public:
	// Parse a full FEN string into piece placement and metadata.
	// On success returns std::nullopt. On failure returns an error message.
	// - outPieces is filled with tuples (ePiece, eSquare) suitable for Board::SetupBoard.
	// - outConfig is filled with parsed GameConfig metadata (color, castling flags, ep square, counters).
	static std::optional<std::string> ParseFEN(const std::string& fen, Config::GameConfig& outConfig, std::vector<std::tuple<ePiece, eSquare>>& outPieces) noexcept;

	// Validate board state (after Board::SetupBoard has been applied) against the parsed FEN metadata.
	// Adjusts outConfig to remove inconsistent castling rights and invalid en-passant squares.
	// Returns true if metadata was adjusted/validated successfully (position usable).
	static bool ValidatePositionAgainstFENMetadata(Config::GameConfig& outConfig) noexcept;

private:
	// Parse only the piece placement field (the part before the first space).
	// Fills outVec with (piece, square) tuples in the same square-indexing used in Board.
	// On success returns std::nullopt. On failure returns an error message.
	static std::optional<std::string> ParsePiecePlacementField(const std::string& placement, std::vector<std::tuple<ePiece, eSquare>>& outVec) noexcept;

	// Convert a 2-char square like "e3" to eSquare. Returns NO_SQUARE on invalid input.
	static eSquare SquareFromString(const std::string& s) noexcept;

	// Parse FEN castling string format either "KQkq" or "-" if no more available. Returns false on invalid format.
	static std::optional<std::string> populateCastlingFlags(const std::string& castling, Config::GameConfig& outConfig) noexcept;
};