#pragma once

#include <nlohmann/json.hpp>
#include "SearchLimits.h"
#include "SearchTuning.h"

class Board;

class Config final {
  public:
	static constexpr int DEFAULT_DEPTH = 4;
	static constexpr unsigned SEARCH_PLAYER_TYPE = 1; // PlayerType::Search, asserted in PlayerFactory.cpp
	static constexpr unsigned DEFAULT_PLAYER_TYPE = SEARCH_PLAYER_TYPE;

	struct PlayerConfig {
		unsigned type{DEFAULT_PLAYER_TYPE};
		unsigned depth{DEFAULT_DEPTH};             // default max depth mapped by CreatePlayer
		SearchLimits search_limits;                // per-move search constraints, parsed from "search_limits"
		std::optional<SearchTuning> search_tuning; // only for PlayerType::Search
		std::optional<unsigned> threads;           // Lazy SMP thread count; CreatePlayer clamps AIPerplex to [1,32]
	};

	void ReadConfigFile(const std::string& filename, Board& board);

	PlayerConfig GetPlayerFromConfig(bool bWhite) const noexcept;

  private:
	void SetupPlayerConfig(const nlohmann::json& config);
	void ReadBoardSetup(const nlohmann::json& config, Board& board) const;

	// For FEN support
	void ReadFEN(const std::string& fen, Board& board) const;

	PlayerConfig white_;
	PlayerConfig black_;
};
