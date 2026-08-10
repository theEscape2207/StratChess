#pragma once
#include "PlayerAI.h"

class Move;

class AITrans final
	: public PlayerAiBase
{
public:
	// Implementation/overrides of the IPlayer interface
	Move GetMove(GameInfo& info) override;
	const char* GetType() const noexcept override
	{	return "Transpositional AlphaBeta";	}
	
	// Note: NOT to be called directly - only through Factory method (needed to be public due to usage of make_unique)
	explicit AITrans(unsigned md) : PlayerAiBase(md) {}
	~AITrans() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	AITrans(const AITrans&) = delete;
	AITrans& operator=(const AITrans&) = delete;
	AITrans(AITrans&&) = delete;
	AITrans& operator=(AITrans&&) = delete;
private:
	int Search(size_t ply, int alpha, int beta);
};
