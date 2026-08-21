#pragma once

#include "defines.h"
#include "IPlayer.h"
#include "GameState.h"

// Forward declare
class Move;
class Board;

class PlayerBase : public IPlayer {
  public:
	enum class ePlayerTypes {
		HUMAN,             // 0
		ALPHABETA,         // 1
		ABITERATING,       // 2
		AIAGENT,           // 3
		AITRANS,           // 4 — ARCHIVED (TT bugs, use AI_PERPLEX)
		ABITERATIVE_TRANS, // 5 — ARCHIVED (TT bugs, use AI_PERPLEX)
		AI_PERPLEX,        // 6
	};
	// Non-virtual

	/* IPlayer implementation */
	int GetBestScore() const noexcept { return _bestScore; }

	bool IsHuman() const noexcept override { return isHuman_; }
	/* End IPlayer implementation */

	PlayerBase() = default;
	~PlayerBase() = default;

	// Un-hide IPlayer::GetMove(limits) — without this, the non-virtual overload
	// below would hide it from name lookup on derived classes.
	using IPlayer::GetMove;

	// Convenience overload: no per-call limits — use the engine's configured
	// defaults (time_limit_, max_depth_). Not virtual; forwards to the real
	// GetMove(limits) override.
	SearchResult GetMove() { return GetMove(SearchLimits{}); }

	PlayerBase(const PlayerBase&) = delete;
	PlayerBase& operator=(const PlayerBase&) = delete;
	PlayerBase(PlayerBase&&) = delete;
	PlayerBase& operator=(PlayerBase&&) = delete;

  protected:
	/*
	 * Protected variables
	 */
	// The current best score
	int _bestScore{0};

	// Only used for printout atm
	std::string type_;
	unsigned depth_{0};

	bool isHuman_{false};
};
