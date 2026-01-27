#pragma once

#include <sstream>
#include <fstream>

#include "Move.h"
#include "Config.h"
#include "Utils\Logger.h"

class IPlayer;

class Game final
{
	friend std::ostream & operator<<(std::ostream &, const Game &game);
	
	void Init();
	void LoadConfigFileSettings();
	std::unique_ptr<IPlayer> SetPlayerParams(const Config::PlayerConfig &config);
	void SetGameParams(const Config::GameConfig& config) noexcept;
	void CreateGameMoveFile();
	
	
	void PrintBoardAndMove(const Move& move) const;
	void PrintGameMoves();
	// Tilfoejer traek til moveList - opdaterer spilvariable
	void AddGameMove(const Move& move);

	void AddFileHeader(std::ostream& file) const;

	//void ReadBoardSetup(const const AutoPtr<XMLConfiguration>& );

	IPlayer& GetCurrentPlayer() const noexcept;
		
	void PrintStateMessage( ) const;

	// Inline stuff
	size_t GetBoardCount() const noexcept { return m_GameMoves.size();	}

	bool IsStillPlaying() const noexcept {	return gameInfo_.gameState == GameStates::STILL_PLAYING; 	}

	static bool HasHumanExited(const Move& move) noexcept {	return move.IsEmpty();	}


	/*
	* Event Methods
	*/
	// A New move has been committed to the current PVLine
	// Currently we just print it to screen
	//************************************
	// Method:      onNewPVLineMove
	// Description: 
	// FullName:    private Game::onNewPVLineMove
	// Returns:     void - 
	// Parameter:   const void*  - 
	// Parameter:   const PVLine& newLine - 
	// Remark:      
	// ************************************
	void onNewPVLineMove(const void* /*pSender*/, const PVLine& newLine)
	{
		std::stringstream sstream;
		sstream << newLine;
		spdlog::default_logger()->warn(sstream.str());
	}

	
	// ************************************
	// Method:      OnGameStateChanged
	// Description: Event receiver - gets called when the overall game state changes
	// FullName:    private Game::OnGameStateChanged
	// Returns:     void - 
	// Parameter:   const void*  - pointer to the sender object
	// Parameter:   const GameInfo::GameStates& newState - 
	// Remark:      
	//************************************
	void OnGameStateChanged(const void* /*pSender*/, const GameStates& newState ) noexcept
	{
		gameInfo_.gameState = newState;
	}

public:
	Game();
	~Game();

	void unsubscribePlayerEvents();
		
	//static void LogMessage(const std::string &text, Poco::Message::Priority );
	void SetCustomGame(const Config::GameConfig& config) noexcept
	{
		customGame_ = true;
		SetGameParams(config);
	}

	void Run();

	Game(const Game&) = delete;
	Game& operator=(const Game&) = delete;
	Game(Game&&) = delete;
	Game& operator=(Game&&) = delete;
private:
	std::unique_ptr<IPlayer> m_pPlayers[2];
	//unsigned m_iBoardCount;

	// Data structure for keeping 
	std::vector<Move> m_GameMoves;

	// For printing Game Moves to File
	std::ofstream movesFile;
	GameInfo gameInfo_;
	bool customGame_{ false };
};
