#pragma once

#include "Config.h"
#include "IPlayer.h"

#include <memory>

class Board;

// The numeric "type" value in game_settings.json.
enum class PlayerType : unsigned {
	Human = 0,
	Search = 1,
};

struct PlayerCreationOptions {
	bool verbose_search_logging{false};
};

// Throws std::invalid_argument for a type that is not a PlayerType.
std::unique_ptr<IPlayer> CreatePlayer(const Config::PlayerConfig& config, Board& board,
                                      PlayerCreationOptions options = {});
