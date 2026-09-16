#pragma once

#include <nlohmann/json.hpp>
#include "GameState.h" // For CastlingRights, eColor, eSquare
#include "SearchLimits.h"
#include "SearchTuning.h"
#include <cstdint>

class Board;

class Config final {
	static const int DEFAULT_DEPTH = 4;

  public:
	static constexpr unsigned DEFAULT_PLAYER_TYPE = 1; // PlayerType::Search, asserted in PlayerFactory.cpp

	struct PlayerConfig {
		unsigned type{DEFAULT_PLAYER_TYPE};
		unsigned depth{DEFAULT_DEPTH}; // default max depth mapped by CreatePlayer
		SearchLimits
		    search_limits; // per-move search constraints, parsed from "search_limits" (or legacy max_depth/time_limit)
		std::optional<SearchTuning> search_tuning; // only for PlayerType::Search
		std::optional<unsigned> threads;           // Lazy SMP thread count; CreatePlayer clamps AIPerplex to [1,32]
	};

	struct GameConfig {
		// Set Side to move
		eColor sideToMove{eColor::WHITE};
		// Set ep square
		eSquare epSquare{NO_SQUARE};
		// Castling availability
		uint8_t castlingRights{CastlingRights::ALL};
		// Halfmove clock - moves since pawn moves or capture
		int num50moves{0};
		// Fullmove number - number of moves done by both black and white
		int fullMoveCounter{0};
	};
	//void LoadConfigFileSettings();

	void ReadConfigFile(const std::string& /*filename*/, Board& board);

	PlayerConfig GetPlayerFromConfig(bool bWhite) const noexcept;

  public:
	~Config() = default;

  private:
	bool CheckBoardSetupData(const std::string& strPiece, const std::string& regex) const;
	void SetupPlayerConfig(const nlohmann::json& config);
	void ReadBoardSetup(const nlohmann::json& config, Board& board) const;

	// For FEN support
	void ReadFEN(const std::string& fen, Board& board) const;

	PlayerConfig white_;
	PlayerConfig black_;
};
