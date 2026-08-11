// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Config.h"
#include "Board.h"
#include "Game.h"

namespace {

// Case-insensitive comparison of the first n characters. Replaces MSVC's
// _strnicmp, which has no portable equivalent in the standard library.
bool iequals_n(std::string_view a, std::string_view b, std::size_t n)
{
	if (a.size() < n || b.size() < n) {
		return false;
	}
	for (std::size_t i = 0; i < n; ++i) {
		const auto ca = static_cast<unsigned char>(a[i]);
		const auto cb = static_cast<unsigned char>(b[i]);
		if (std::tolower(ca) != std::tolower(cb)) {
			return false;
		}
	}
	return true;
}

} // namespace

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
	// .at(), never operator[]: on a CONST json a missing key is undefined
	// behaviour, not an error. Release happened to carry on and throw something
	// later; a Debug build asserts inside nlohmann. .at() throws out_of_range
	// naming the key, in both configurations.
	const json& game = config.at("game");

	// if nothing is found - use default setup
	const std::string setupType = game.value("setup", "default");
	const std::string FENKey = "FEN";
	if (iequals_n(setupType, FENKey, 3)) {
		spdlog::default_logger()->debug("FEN configuration");

		const std::string FENstring = game.value(FENKey, "");
		if (FENstring.empty()) {
			spdlog::default_logger()->warn(
			    "FEN key found, but no string - selecting default board");
			board.SetDefaultBoard();
			return;
		}
		ReadFEN(FENstring, board);
	} else {
		spdlog::default_logger()->debug("Default board selected");
		board.SetDefaultBoard();
	}
}

void Config::ReadFEN(const std::string& fen, Board& board) const
{
	// A malformed FEN in game_settings.json falls back to the standard opening position, the same
	// way an empty FEN value does in ReadBoardSetup — otherwise the game would start from whatever
	// the board happened to hold, which for a fresh Board is empty.
	if (!board.SetupFromFEN(fen)) {
		spdlog::default_logger()->error(
		    "FEN in configuration could not be parsed - selecting default board: {}", fen);
		board.SetDefaultBoard();
		return;
	}

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
bool Config::CheckBoardSetupData(const std::string& /*strPiece*/,
                                 const std::string& /*regex*/) const
{
	// FIXME: Not yet ported away from Poco
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
		// Say what happens next, not just what failed. Silently continuing on
		// built-in defaults is the failure mode that looks like the settings
		// file was read and ignored.
		std::cerr << "Cannot open " << filename
		          << " -- continuing with built-in defaults (both players type " << DEFAULT_EVAL
		          << ", depth " << DEFAULT_DEPTH << ", standard opening position)\n";
		return;
	}

	// game_settings.json is heavily commented, and comments are not valid JSON, so
	// ignore_comments has to be requested explicitly. The stream operator (>>) does
	// not do it in every nlohmann version -- it happens to in the library's develop
	// branch and does not in the v3.12.0 release -- so relying on that default makes
	// whether the engine can read its own settings depend on which copy of the
	// header the build picked up.
	nlohmann::json config = nlohmann::json::parse(configFile,
	                                              /*cb*/ nullptr,
	                                              /*allow_exceptions*/ true,
	                                              /*ignore_comments*/ true);

	SetupPlayerConfig(config);

	// Read any Board setup from the config file
	ReadBoardSetup(config, board);
}

namespace {
// Parses the "search_limits" block into a SearchLimits (all keys optional).
SearchLimits ParseSearchLimitsBlock(const json& sl)
{
	SearchLimits limits;
	if (sl.contains("depth"))
		limits.depth = sl["depth"].get<int>();
	if (sl.contains("movetime"))
		limits.movetime = std::chrono::milliseconds(sl["movetime"].get<int64_t>());
	if (sl.contains("infinite"))
		limits.infinite = sl["infinite"].get<bool>();
	if (sl.contains("clock")) {
		const auto& c = sl["clock"];
		limits.clock = ClockInfo{std::chrono::milliseconds(c.value("remaining", 0)),
		                         std::chrono::milliseconds(c.value("increment", 0)),
		                         c.value("moves_to_go", 0)};
	}
	return limits;
}

Config::PlayerConfig ParsePlayerConfig(const json& p, int defaultDepth, int defaultEval)
{
	Config::PlayerConfig cfg;
	cfg.type = p.value("type", defaultEval);
	cfg.eval = p.value("eval", 0);

	if (p.contains("search_limits")) {
		cfg.search_limits = ParseSearchLimitsBlock(p["search_limits"]);
	} else {
		// Legacy fallback: "max_depth"/"time_limit" map onto depth/movetime.
		// "time_limit" always resolves to a real movetime (defaulting to
		// 15000ms), matching the old unconditional
		// p.value("time_limit", 15000u) — otherwise a "max_depth"-only
		// legacy config would fall through resolve_limits() into the
		// UCI-style 1h "depth only" budget instead of a real time cap.
		bool usedLegacyKeys = false;
		if (p.contains("max_depth")) {
			cfg.search_limits.depth = p["max_depth"].get<int>();
			usedLegacyKeys = true;
		} else if (p.contains("depth")) {
			cfg.search_limits.depth = p["depth"].get<int>();
		}
		cfg.search_limits.movetime = std::chrono::milliseconds(p.value("time_limit", 15000u));
		if (p.contains("time_limit"))
			usedLegacyKeys = true;
		if (usedLegacyKeys) {
			spdlog::default_logger()->warn(
			    "game_settings.json: player uses legacy \"max_depth\"/\"time_limit\" keys — "
			    "migrate to the \"search_limits\" block");
		}
	}
	cfg.depth = static_cast<unsigned>(cfg.search_limits.depth.value_or(defaultDepth));

	// Parse SearchTuning if present (only meaningful for AI_PERPLEX)
	if (p.contains("search_tuning")) {
		const auto& st = p["search_tuning"];
		Config::SearchTuningConfig t;
		t.min_nodes_threshold = st.value("min_nodes_threshold", static_cast<int64_t>(1000));
		t.min_completion_ratio = st.value("min_completion_ratio", 0.10);
		t.min_pv_ratio = st.value("min_pv_ratio", 0.33);
		t.score_draw_threshold = st.value("score_draw_threshold", 20);
		t.delta_pruning_margin = st.value("delta_pruning_margin", 200);
		t.aspiration_initial_delta = st.value("aspiration_initial_delta", 50);
		t.aspiration_max_retries = st.value("aspiration_max_retries", 4);
		t.aspiration_enabled = st.value("aspiration_enabled", true);
		cfg.search_tuning = t;
	}

	// Parse Lazy SMP thread count if present (optional; default is
	// PlayerAiBase's own default of 1 when the key is absent).
	if (p.contains("threads")) {
		cfg.threads = p["threads"].get<unsigned>();
	}
	return cfg;
}
} // anonymous namespace

void Config::SetupPlayerConfig(const json& config)
{
	// .at() rather than operator[] — see ReadBoardSetup: indexing a const json
	// with an absent key is UB, and every key on this path is absent in a
	// settings file that is merely wrong rather than malformed.
	const json& players = config.at("game").at("players");
	white_ = ParsePlayerConfig(players.at("white"), DEFAULT_DEPTH, DEFAULT_EVAL);
	black_ = ParsePlayerConfig(players.at("black"), DEFAULT_DEPTH, DEFAULT_EVAL);
}

Config::PlayerConfig Config::GetPlayerFromConfig(bool bWhite) const noexcept
{
	if (bWhite) {
		return white_;
	}
	return black_;
}
