#pragma once
#include "PlayerAiIterBase.h"

class Move;

class AIAgent final : public PlayerAiIterBase {
  public:
	// Implementation/overrides of the IPlayer interface
	SearchResult GetMove(const SearchLimits& limits) override;
	const char* GetType() const noexcept override { return "AI Agent"; }

	// Note: NOT to be called directly - only through Factory method
	explicit AIAgent(Board& board, unsigned md) : PlayerAiIterBase(board, md) {}
	~AIAgent() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	AIAgent(const AIAgent&) = delete;
	AIAgent& operator=(const AIAgent&) = delete;
	AIAgent(AIAgent&&) = delete;
	AIAgent& operator=(AIAgent&&) = delete;

  private:
	int Search(size_t ply, int alpha, int beta, PVLine& pline);
};
