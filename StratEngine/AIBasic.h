#pragma once
#include "PlayerAI.h"

class Move;

class AIBasic final : public PlayerAiBase {

  public:
	// Implementation/overrides of the IPlayer interface
	Move GetMove(GameInfo& info, const SearchLimits& limits) override;
	const char* GetType() const noexcept override
	{
		return "Basic Alpha Beta";
	}

	// Note: NOT to be called directly - only through Factory method
	explicit AIBasic(Board& board, unsigned md) : PlayerAiBase(board, md) {}
	~AIBasic() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	AIBasic(AIBasic&) = delete;
	AIBasic& operator=(AIBasic&) = delete;
	AIBasic(AIBasic&&) = delete;
	AIBasic& operator=(AIBasic&&) = delete;

  private:
	int Search(size_t ply, int alpha, int beta);
};
