#pragma once

#include <sstream>
#include <fstream>

#include "Move.h"
#include "GameState.h"
#include "Config.h"
#include "Board.h"
#include "Utils/FENParser.h"
#include "Utils/Logger.h"

class IPlayer;

class Game final {
	friend std::ostream& operator<<(std::ostream&, const Game& game);

	void Init();
	void LoadConfigFileSettings();
	std::unique_ptr<IPlayer> SetPlayerParams(const Config::PlayerConfig& config);
	void SetGameParams(const GameInfo& info) noexcept;
	void CreateGameMoveFile();

	void PrintBoardAndMove(const Move& move) const;
	void PrintGameMoves();
	// Tilfoejer traek til moveList - opdaterer spilvariable
	void AddGameMove(const Move& move);

	void AddFileHeader(std::ostream& file) const;

	IPlayer& GetCurrentPlayer() const noexcept;

	void PrintStateMessage() const;

	// Inline stuff
	size_t GetBoardCount() const noexcept
	{
		return m_GameMoves.size();
	}

	bool IsStillPlaying() const noexcept
	{
		return !gameInfo_.GameEnded();
	}

	static bool HasHumanExited(const Move& move) noexcept
	{
		return move.is_null();
	}

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
	void OnGameStateChanged(const void* /*pSender*/, const GameStates& newState) noexcept
	{
		gameInfo_.gameState = newState;
	}

  public:
	Game();
	~Game();

	void unsubscribePlayerEvents();

	void SetCustomGame(const GameInfo& info) noexcept
	{
		customGame_ = true;
		SetGameParams(info);
	}

	void Run();

	Game(const Game&) = delete;
	Game& operator=(const Game&) = delete;
	Game(Game&&) = delete;
	Game& operator=(Game&&) = delete;

  private:
	// Must be declared (and thus constructed/destroyed) before m_pPlayers —
	// players hold a Board& reference into it that must outlive them.
	Board board_;

	std::unique_ptr<IPlayer> m_pPlayers[2];

	// Per-player search constraints, built once from game_settings.json
	// ("search_limits" block) and passed on every GetMove() call — mirrors
	// the color indexing of m_pPlayers (board_.GetCurrentColor()).
	SearchLimits player_limits_[2];

	// Data structure for keeping
	std::vector<Move> m_GameMoves;

	// For printing Game Moves to File
	std::ofstream movesFile_;
	GameInfo gameInfo_;
	bool customGame_{false};
};
