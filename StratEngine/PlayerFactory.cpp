#include "StdAfx.h"
#include "PlayerFactory.h"

#include "ABIterative.h"
#include "AIAgent.h"
#include "AIBasic.h"
#include "PlayerBase.h"
#include "PlayerHuman.h"
#include "SearchPlayer.h"

#include <sstream>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace {

	EvalManager::EvalTypes evaluator_type(unsigned configured)
	{
		return static_cast<EvalManager::EvalTypes>(configured);
	}

	const char* evaluator_name(EvalManager::EvalTypes type)
	{
		switch (type) {
		case EvalManager::EvalTypes::NONE:
			return "None";
		case EvalManager::EvalTypes::SIMPLE:
			return "Simple";
		case EvalManager::EvalTypes::COMPLEX:
			return "Complex";
		default:
			throw std::invalid_argument("Unknown Eval type");
		}
	}

	std::string search_description(unsigned depth, EvalManager::EvalTypes evaluator)
	{
		std::ostringstream out;
		out << "\n\tEngine type:\tPerplexity Transpositional AlphaBeta\n\tDepth:\t\t" << depth
		    << "\n\tEvaluation:\t" << evaluator_name(evaluator) << '\n';
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
		return tuning;
	}

} // namespace

std::unique_ptr<IPlayer> CreatePlayer(const Config::PlayerConfig& config, Board& board, PlayerCreationOptions options)
{
	const auto type = static_cast<PlayerBase::ePlayerTypes>(config.type);
	if (config.search_tuning && type != PlayerBase::ePlayerTypes::AI_PERPLEX) {
		spdlog::warn("search_tuning in game_settings.json is ignored for player type {} "
		             "(only supported by AI_PERPLEX)",
		             config.type);
	}
	if (type == PlayerBase::ePlayerTypes::HUMAN)
		return std::make_unique<PlayerHuman>(board);

	if (type == PlayerBase::ePlayerTypes::AI_PERPLEX) {
		const EvalManager::EvalTypes evaluator = evaluator_type(config.eval);
		AIPerplexConfig search_config;
		search_config.evaluator = evaluator;
		search_config.default_depth = config.depth;
		search_config.threads = config.threads.value_or(1);
		search_config.tuning = map_tuning(config.search_tuning);
		search_config.verbose_logging = options.verbose_search_logging;

		auto player = std::make_unique<SearchPlayer>(board, std::move(search_config),
		                                             search_description(config.depth, evaluator));
		player->search_.StartNewGame();
		return player;
	}

	std::unique_ptr<PlayerAiBase> player;
	switch (type) {
	case PlayerBase::ePlayerTypes::ALPHABETA:
		player = std::make_unique<AIBasic>(board, config.depth);
		break;
	case PlayerBase::ePlayerTypes::ABITERATING:
		player = std::make_unique<ABIterative>(board, config.depth);
		break;
	case PlayerBase::ePlayerTypes::AIAGENT:
		player = std::make_unique<AIAgent>(board, config.depth);
		break;
	case PlayerBase::ePlayerTypes::AITRANS:
		throw std::invalid_argument("AITrans is archived (TT bugs). Use AI_PERPLEX instead.");
	case PlayerBase::ePlayerTypes::ABITERATIVE_TRANS:
		throw std::invalid_argument("ABIterTrans is archived (TT bugs). Use AI_PERPLEX instead.");
	case PlayerBase::ePlayerTypes::HUMAN:
	case PlayerBase::ePlayerTypes::AI_PERPLEX:
		break;
	default:
		throw std::invalid_argument("Unknown Player type");
	}

	if (!player)
		throw std::invalid_argument("Unknown Player type");
	player->SetEvalEngine(evaluator_type(config.eval));
	if (config.threads)
		player->SetThreads(*config.threads);
	player->StartNewGame();
	return player;
}
