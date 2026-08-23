#pragma once

#include "Config.h"
#include "IPlayer.h"

#include <memory>

class Board;

struct PlayerCreationOptions {
	bool verbose_search_logging{false};
};

std::unique_ptr<IPlayer> CreatePlayer(const Config::PlayerConfig& config, Board& board,
                                      PlayerCreationOptions options = {});
