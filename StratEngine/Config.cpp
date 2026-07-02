// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Config.h"
#include "Board.h"
#include "Game.h"

using json = nlohmann::json;

//***************************************
// Method:      ReadBoardSetup
// Description: 
// FullName:    private Game::ReadBoardSetup const
// Returns:     void - 
// Parameter:   const json& config - 
// Remark:      
//***************************************

void Config::ReadBoardSetup(const json& config, Board& board) const
{
	// if nothing is found - use default setup
	const std::string setupType = config["game"].value("setup", "default");
	const std::string FENKey = "FEN";
	if (0 == _strnicmp(setupType.c_str(), FENKey.c_str(), 3))
	{
		spdlog::default_logger()->debug("FEN configuration");

		const std::string FENstring = config["game"].value(FENKey, "");
		if (FENstring.empty())
		{
			spdlog::default_logger()->warn("FEN key found, but no string - selecting default board");
			board.SetDefaultBoard();
			return;
		}
		ReadFEN(FENstring, board);
	}
	else
	{
		spdlog::default_logger()->debug("Default board selected");
		board.SetDefaultBoard();
	}
}

void Config::ReadFEN(const std::string& fen, Board& board) const
{
	board.SetupFromFEN(fen);

	GameInfo info = board.GetGameInfo(); // Get the final gameInfo from board

	// Done: hand off to game with validated config
	pGame_->SetCustomGame(info);
}

//***************************************
// Method:      CheckBoardSetupData
// Description: 
// FullName:    private Game::CheckBoardSetupData const
// Returns:     bool - 
// Parameter:   const std::string& strPiece - 
// Parameter:   const std::string& regexStr - 
// Remark:      
//***************************************
bool Config::CheckBoardSetupData(const std::string& /*strPiece*/, const std::string& /*regex*/) const
{
	//FIXME: Not yet ported away from Poco
	/*const Poco::RegularExpression regPiece(regex);
	if (!regPiece.match(strPiece))
	{
		std::stringstream str;

		str << "Invalid board setup data found: Piece on square "
			<< strPiece.c_str() << " with type: " << strPiece.c_str() << std::endl;
		spdlog::default_logger()->warn(str.str());
		return false;
	}*/
	return true;
}
void Config::ReadConfigFile(const std::string& filename, Board& board)
{
	spdlog::default_logger()->debug("Reading Config File");

	std::ifstream configFile(filename);
	if (!configFile) {
		std::cerr << "Cannot open game_settings.json\n";
		return;		// FIXME: add error handling
	}

	nlohmann::json config;
	configFile >> config;

	SetupPlayerConfig(config);

	// Read any Board setup from the config file
	ReadBoardSetup(config, board);
}

namespace {
	Config::PlayerConfig ParsePlayerConfig(const json& p, int defaultDepth, int defaultEval)
	{
		Config::PlayerConfig cfg;
		cfg.type = p.value("type", defaultEval);
		cfg.eval = p.value("eval", 0);
		cfg.time_limit_ms = p.value("time_limit", 15000u);

		// Prefer "max_depth"; fall back to legacy "depth" key
		if (p.contains("max_depth"))
			cfg.depth = p["max_depth"].get<unsigned>();
		else
			cfg.depth = p.value("depth", static_cast<unsigned>(defaultDepth));

		// Parse SearchTuning if present (only meaningful for AI_PERPLEX)
		if (p.contains("search_tuning")) {
			const auto& st = p["search_tuning"];
			Config::SearchTuningConfig t;
			t.min_nodes_threshold    = st.value("min_nodes_threshold",    static_cast<int64_t>(1000));
			t.min_completion_ratio   = st.value("min_completion_ratio",   0.10);
			t.min_pv_ratio           = st.value("min_pv_ratio",           0.33);
			t.score_draw_threshold   = st.value("score_draw_threshold",   20);
			t.delta_pruning_margin   = st.value("delta_pruning_margin",   200);
			t.aspiration_initial_delta = st.value("aspiration_initial_delta", 50);
			t.aspiration_max_retries   = st.value("aspiration_max_retries",   4);
			t.aspiration_enabled       = st.value("aspiration_enabled",       true);
			cfg.search_tuning = t;
		}
		return cfg;
	}
} // anonymous namespace

void Config::SetupPlayerConfig(const json& config)
{
	white_ = ParsePlayerConfig(config["game"]["players"]["white"], DEFAULT_DEPTH, DEFAULT_EVAL);
	black_ = ParsePlayerConfig(config["game"]["players"]["black"], DEFAULT_DEPTH, DEFAULT_EVAL);
}

Config::PlayerConfig Config::GetPlayerFromConfig(bool bWhite) const noexcept
{
	if (bWhite)
	{
		return white_;
	}
	return black_;
}
