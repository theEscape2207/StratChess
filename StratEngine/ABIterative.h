#pragma once
#include "PlayerAiIterBase.h"

class Move;

class ABIterative final
	: public PlayerAiIterBase
{
public:
	// Note: NOT to be called directly - only through Factory method
	explicit ABIterative(Board& board, unsigned maxDepth)
		:PlayerAiIterBase(board, maxDepth) { }
	// Implementation/overrides of the IPlayer interface
	Move GetMove( GameInfo& info, const SearchLimits& limits ) override;
	const char* GetType() const noexcept override
	{	return "Iterative AlphaBeta";	}
	
	~ABIterative() = default;
	// Force use of factory by preventing constructor, copy-construction & operator=
	ABIterative(const ABIterative&) = delete;
	ABIterative& operator=(const ABIterative&) = delete;
	ABIterative(ABIterative&&) = delete;
	ABIterative& operator=(ABIterative&&) = delete;
private:
	int Search(int ply, int alpha, int beta, PVLine& pline);
};
