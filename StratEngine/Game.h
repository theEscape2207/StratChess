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
	void CreateGameMoveFile();

	void PrintBoardAndMove(const Move& move) const;
	void PrintGameMoves();
	// Tilfoejer traek til moveList - opdaterer spilvariable
	void AddGameMove(const Move& move);

	void AddFileHeader(std::ostream& file) const;

	IPlayer& GetCurrentPlayer() const noexcept;

	// Takes the player that just moved: GetCurrentPlayer() keys off the side to move, which
	// DoMove has already flipped by the time this runs, and the score belongs to the mover.
	void PrintStateMessage(const IPlayer& mover) const;

	// Inline stuff
	size_t GetBoardCount() const noexcept { return m_GameMoves.size(); }

	bool IsStillPlaying() const noexcept { return game_state_ == GameStates::STILL_PLAYING; }

	static bool HasHumanExited(const Move& move) noexcept { return move.is_null(); }

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

  public:
	Game();
	~Game();

	void unsubscribePlayerEvents();

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
	// The outcome, as reported by the player that last moved or adjudicated by Run() itself.
	GameStates game_state_{GameStates::STILL_PLAYING};
};
