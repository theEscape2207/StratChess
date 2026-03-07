#pragma once

#include <nlohmann/json.hpp>
#include "GameState.h"	// For CastlingRights, eColor, eSquare

class Game;

class Config final
{
	static const int DEFAULT_DEPTH = 4;
	static const int DEFAULT_EVAL = 3;

public:
	// Mirrors AIPerplex::SearchTuning — only applied when player type is AI_PERPLEX
	struct SearchTuningConfig {
		int64_t min_nodes_threshold{ 1000 };
		double  min_completion_ratio{ 0.10 };
		double  min_pv_ratio{ 0.33 };
		int     score_draw_threshold{ 20 };
		int     delta_pruning_margin{ 200 };
		int     aspiration_initial_delta{ 50 };
		int     aspiration_max_retries{ 4 };
		bool    aspiration_enabled{ true };
	};

	struct PlayerConfig
	{
		unsigned type{ DEFAULT_EVAL };
		unsigned depth{ DEFAULT_DEPTH };          // max_depth — read from "max_depth" or fallback "depth"
		unsigned eval{ 0 };
		uint32_t time_limit_ms{ 15000 };          // milliseconds; applied to all AI types
		std::optional<SearchTuningConfig> search_tuning;  // only for AI_PERPLEX (type 6)
	};

	struct GameConfig
	{
		// Set Side to move
		eColor sideToMove{ eColor::WHITE };
		// Set ep square
		eSquare epSquare{ NO_SQUARE };
		// Castling availability
		uint8_t castlingRights{ CastlingRights::ALL };
		// Halfmove clock - moves since pawn moves or capture
		int num50moves{ 0 };
		// Fullmove number - number of moves done by both black and white
		int fullMoveCounter{ 0 };
	};
	explicit Config(Game* game) noexcept : pGame_(game) {}
	//void LoadConfigFileSettings();

	void ReadConfigFile(const std::string& /*filename*/);

	PlayerConfig GetPlayerFromConfig(bool bWhite) const noexcept;

public:
	~Config() = default;

private:
	bool CheckBoardSetupData(const std::string& strPiece, const std::string& regex) const;
	void SetupPlayerConfig(const nlohmann::json& config);
	void ReadBoardSetup(const nlohmann::json& config) const;

	// For FEN support
	void ReadFEN(const std::string& fen) const;

	Game* pGame_;

	PlayerConfig white_;
	PlayerConfig black_;
};
