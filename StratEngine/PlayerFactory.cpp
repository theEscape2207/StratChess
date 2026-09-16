#include "StdAfx.h"
#include "PlayerFactory.h"

#include "Eval.h"
#include "HumanPlayer.h"
#include "SearchPlayer.h"

#include <sstream>
#include <spdlog/spdlog.h>
#include <stdexcept>

static_assert(Config::DEFAULT_PLAYER_TYPE == static_cast<unsigned>(PlayerType::Search));

namespace {

	std::string search_description(unsigned depth)
	{
		std::ostringstream out;
		out << "\n\tEngine type:\tPerplexity Transpositional AlphaBeta\n\tDepth:\t\t" << depth << '\n';
		return out.str();
	}

} // namespace

std::unique_ptr<IPlayer> CreatePlayer(const Config::PlayerConfig& config, Board& board, PlayerCreationOptions options)
{
	const auto type = static_cast<PlayerType>(config.type);
	if (type != PlayerType::Human && type != PlayerType::Search) {
		std::ostringstream message;
		message << "unknown player type " << config.type << "; valid: 0 (Human), 1 (Search)";
		throw std::invalid_argument(message.str());
	}

	if (type == PlayerType::Human) {
		if (config.search_tuning)
			spdlog::warn("search_tuning in game_settings.json is ignored for a Human player");
		return std::make_unique<HumanPlayer>(board);
	}

	AIPerplexConfig search_config;
	search_config.default_depth = config.depth;
	search_config.threads = config.threads.value_or(1);
	search_config.tuning = config.search_tuning.value_or(SearchTuning{});
	search_config.verbose_logging = options.verbose_search_logging;

	auto player = std::make_unique<SearchPlayer>(board, search_config, search_description(config.depth));
	player->search_.StartNewGame();
	return player;
}
