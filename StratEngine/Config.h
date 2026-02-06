#pragma once

#include <nlohmann/json.hpp>
#include "GameState.h"	// For CastlingRights, eColor, eSquare

class Game;

class Config final
{
	static const int DEFAULT_DEPTH = 4;
	static const int DEFAULT_EVAL = 3;


public:
	struct PlayerConfig
	{
		unsigned type{ DEFAULT_EVAL };
		unsigned depth{ DEFAULT_DEPTH };
		unsigned eval{ 0 };
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
