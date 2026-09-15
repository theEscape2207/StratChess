#include "StdAfx.h"
#include "PlayerFactory.h"

#include "Eval.h"
#include "PlayerHuman.h"
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

	SearchTuning map_tuning(const std::optional<Config::SearchTuningConfig>& configured)
	{
		SearchTuning tuning;
		if (!configured)
			return tuning;

		const auto& source = *configured;
		tuning.min_nodes_threshold = source.min_nodes_threshold;
		tuning.min_completion_ratio = source.min_completion_ratio;
		tuning.min_pv_ratio = source.min_pv_ratio;
		tuning.score_draw_threshold = source.score_draw_threshold;
		tuning.delta_pruning_margin = source.delta_pruning_margin;
		tuning.aspiration_initial_delta = source.aspiration_initial_delta;
		tuning.aspiration_max_retries = source.aspiration_max_retries;
		tuning.aspiration_enabled = source.aspiration_enabled;
		tuning.lmr_min_depth = source.lmr_min_depth;
		tuning.lmr_min_move_index = source.lmr_min_move_index;
		tuning.lmr_enabled = source.lmr_enabled;
		tuning.null_move_enabled = source.null_move_enabled;
		tuning.null_move_reduction = source.null_move_reduction;
		tuning.null_move_min_depth = source.null_move_min_depth;
		tuning.see_pruning_enabled = source.see_pruning_enabled;
		return tuning;
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
		return std::make_unique<PlayerHuman>(board);
	}

	AIPerplexConfig search_config;
	search_config.default_depth = config.depth;
	search_config.threads = config.threads.value_or(1);
	search_config.tuning = map_tuning(config.search_tuning);
	search_config.verbose_logging = options.verbose_search_logging;

	auto player = std::make_unique<SearchPlayer>(board, search_config, search_description(config.depth));
	player->search_.StartNewGame();
	return player;
}
