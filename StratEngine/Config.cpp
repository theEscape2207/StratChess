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

void Config::ReadBoardSetup(const json& config) const
{
	// if nothing is found - use default setup
	const std::string setupType = config["game"].value("setup", "default");
	const std::string FENKey = "FEN";
	if (0 == _strnicmp(setupType.c_str(), FENKey.c_str(), 3))
	{
		spdlog::default_logger()->info("FEN configuration");

		const std::string FENstring = config["game"].value(FENKey, "");
		if (FENstring.empty())
		{
			spdlog::default_logger()->warn("FEN key found, but no string - selecting default board");
			Board::Instance().SetDefaultBoard();
			return;
		}
		ReadFEN(FENstring);
	}
	else
	{
		spdlog::default_logger()->info("Default board selected");
		Board::Instance().SetDefaultBoard();
	}
}

void Config::ReadFEN(const std::string& fen) const
{
	Board& board = Board::Instance();
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
void Config::ReadConfigFile(const std::string& filename)
{
	spdlog::default_logger()->info("Reading Config File");

	std::ifstream configFile(filename);
	if (!configFile) {
		std::cerr << "Cannot open game_settings.json\n";
		return;		// FIXME: add error handling
	}

	nlohmann::json config;
	configFile >> config;

	SetupPlayerConfig(config);

	// Read any Board setup from the config file
	ReadBoardSetup(config);
}

void Config::SetupPlayerConfig(const json& config)
{
	// Read white player - with default settings
	white_.depth = config["game"]["players"]["white"].value("depth", DEFAULT_DEPTH);
	white_.type = config["game"]["players"]["white"].value("type", DEFAULT_EVAL);	// default aiagent
	white_.eval = config["game"]["players"]["white"].value("eval", 0);	// default SIMPLE

	black_.depth = config["game"]["players"]["black"].value("depth", DEFAULT_DEPTH);
	black_.type = config["game"]["players"]["black"].value("type", DEFAULT_EVAL);	// default aiagent
	black_.eval = config["game"]["players"]["black"].value("eval", 0);	// default SIMPLE
}

Config::PlayerConfig Config::GetPlayerFromConfig(bool bWhite) const noexcept
{
	if (bWhite)
	{
		return white_;
	}
	return black_;
}
