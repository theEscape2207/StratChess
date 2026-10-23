// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "Common.h"
#include "Config.h"
#include "Board.h"
#include "SearchTuningSchema.h"

using json = nlohmann::json;

void Config::ReadBoardSetup(const json& config, Board& board) const
{
	// .at(), never operator[]: on a CONST json a missing key is undefined
	// behaviour, not an error. Release happened to carry on and throw something
	// later; a Debug build asserts inside nlohmann. .at() throws out_of_range
	// naming the key, in both configurations.
	const json& game = config.at("game");

	// No "FEN" key means the standard opening position.
	const std::string FENstring = game.value("FEN", "");
	if (FENstring.empty()) {
		if (game.contains("FEN"))
			spdlog::default_logger()->warn("FEN key found, but no string - selecting default board");
		board.SetDefaultBoard();
		return;
	}
	ReadFEN(FENstring, board);
}

void Config::ReadFEN(const std::string& fen, Board& board) const
{
	// A malformed FEN in game_settings.json falls back to the standard opening position, the same
	// way an empty FEN value does in ReadBoardSetup — otherwise the game would start from whatever
	// the board happened to hold, which for a fresh Board is empty.
	if (!board.SetupFromFEN(fen)) {
		spdlog::default_logger()->error("FEN in configuration could not be parsed - selecting default board: {}", fen);
		board.SetDefaultBoard();
		return;
	}
}

void Config::ReadConfigFile(const std::string& filename, Board& board)
{
	spdlog::default_logger()->debug("Reading Config File");

	std::ifstream configFile(filename);
	if (!configFile) {
		// Say what happens next, not just what failed. Silently continuing on
		// built-in defaults is the failure mode that looks like the settings
		// file was read and ignored.
		std::cerr << "Cannot open " << filename << " -- continuing with built-in defaults (both players type "
		          << DEFAULT_PLAYER_TYPE << " (Search), depth " << DEFAULT_DEPTH << ", standard opening position)\n";
		return;
	}

	// game_settings.json is heavily commented, and comments are not valid JSON, so
	// ignore_comments has to be requested explicitly. The stream operator (>>) does
	// not do it in every nlohmann version -- it happens to in the library's develop
	// branch and does not in the v3.12.0 release -- so relying on that default makes
	// whether the engine can read its own settings depend on which copy of the
	// header the build picked up.
	const nlohmann::json config = nlohmann::json::parse(configFile,
	                                                    /*cb*/ nullptr,
	                                                    /*allow_exceptions*/ true,
	                                                    /*ignore_comments*/ true);

	SetupPlayerConfig(config);

	// Read any Board setup from the config file
	ReadBoardSetup(config, board);
}

namespace {
	// A configured search depth. UCI ignores depth <= 0; here it is an error, since resolve_limits()
	// would otherwise read a negative depth as a huge one and clamp it to MAX_PLY - 1.
	int ReadDepth(const json& value, const char* key)
	{
		const int depth = value.get<int>();
		if (depth <= 0)
			throw std::invalid_argument(std::string("game_settings.json: ") + key + " must be positive, got " +
			                            std::to_string(depth));
		return depth;
	}

	// Parses the "search_limits" block into a SearchLimits (all keys optional).
	SearchLimits ParseSearchLimitsBlock(const json& sl)
	{
		SearchLimits limits;
		if (sl.contains("depth"))
			limits.depth = ReadDepth(sl["depth"], "search_limits.depth");
		if (sl.contains("movetime"))
			limits.movetime = std::chrono::milliseconds(sl["movetime"].get<int64_t>());
		if (sl.contains("infinite"))
			limits.infinite = sl["infinite"].get<bool>();
		if (sl.contains("clock")) {
			const auto& c = sl["clock"];
			limits.clock = ClockInfo{std::chrono::milliseconds(c.value("remaining", 0)),
			                         std::chrono::milliseconds(c.value("increment", 0)), c.value("moves_to_go", 0)};
		}
		return limits;
	}

	Config::PlayerConfig ParsePlayerConfig(const json& p, const char* side)
	{
		Config::PlayerConfig cfg;
		cfg.type = p.value("type", Config::DEFAULT_PLAYER_TYPE);

		if (p.contains("search_limits")) {
			cfg.search_limits = ParseSearchLimitsBlock(p["search_limits"]);
		} else if (cfg.type == Config::SEARCH_PLAYER_TYPE) {
			throw std::invalid_argument(std::string("game_settings.json: players.") + side +
			                            ": a Search player needs a search_limits block");
		}
		cfg.depth = static_cast<unsigned>(cfg.search_limits.depth.value_or(Config::DEFAULT_DEPTH));

		// Parse SearchTuning if present (only meaningful for PlayerType::Search)
		if (p.contains("search_tuning")) {
			SearchTuning tuning;
			if (const auto error = SearchTuningSchema::ParseJson(p.at("search_tuning"), tuning))
				throw std::invalid_argument("game_settings.json: search_tuning." + error->field + ": " +
				                            error->message);
			cfg.search_tuning = tuning;
		}

		// Parse Lazy SMP thread count if present (optional; the factory defaults to 1).
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
	white_ = ParsePlayerConfig(players.at("white"), "white");
	black_ = ParsePlayerConfig(players.at("black"), "black");
}

Config::PlayerConfig Config::GetPlayerFromConfig(bool bWhite) const noexcept
{
	if (bWhite) {
		return white_;
	}
	return black_;
}
