#pragma once

#include "defines.h"
#include "IPlayer.h"

// Forward declare
class Move;
struct GameInfo;

class PlayerBase : public IPlayer
{
public:
	enum class ePlayerTypes{	HUMAN,					// 0
						ALPHABETA,				// 1
						ABITERATING,			// 2
						AIAGENT,				// 3
						AITRANS,				// 4
						ABITERATIVE_TRANS,		// 5
						};
	// Non-virtual
	
	// Returns the current GameState
	/*GameInfo::GameStates GetGameState() const
	{
		return this->gameState_;
	}*/

	/* IPlayer implementation */
	int GetBestScore() const noexcept override
	{
		return _bestScore;
	}

	bool IsHuman() const noexcept override
	{
		return isHuman_;
	}
	/* End IPlayer implementation */

	PlayerBase() = default;
	~PlayerBase() = default;

	// Factory constructor!
	static std::unique_ptr<PlayerBase> Create(ePlayerTypes type, unsigned max_depth);

	PlayerBase(const PlayerBase&) = delete;
	PlayerBase& operator=(const PlayerBase&) = delete;
	PlayerBase(PlayerBase&&) = delete;
	PlayerBase& operator=(PlayerBase&&) = delete;
protected:
	static void UpdateBoardInfo( const Move &move, GameInfo& info ) noexcept;
private:
	// Helpers
	static void UpdateCastlingState(const Move &move, GameInfo &info) noexcept;

	static void UpdateFiftyMovesState(const Move & move, GameInfo &info) noexcept;

protected:

	/*
	 * Protected variables
	 */
	// The current best score
	int _bestScore{ 0 };

	// Only used for printout atm
	std::string type_;
	unsigned depth_{ 0 };

	bool isHuman_{ false };
};
