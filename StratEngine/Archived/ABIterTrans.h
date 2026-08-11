#pragma once
#include "PlayerAiIterBase.h"

class Move;

class ABIterTrans final
	: public PlayerAiIterBase
{
public:
	// Implementation/overrides of the IPlayer interface
	Move GetMove( GameInfo& info) override;

	const char* GetType() const noexcept override
	{ return "Transpositional Iterative AlphaBeta"; }
	
	// Note: NOT to be called directly - only through Factory method
	explicit ABIterTrans(unsigned md) : PlayerAiIterBase(md) {}
	~ABIterTrans() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	ABIterTrans(const ABIterTrans&) = delete;
	ABIterTrans& operator=(const ABIterTrans&) = delete;
	ABIterTrans(ABIterTrans&&) = delete;
	ABIterTrans& operator=(ABIterTrans&&) = delete;
private:
	int Search( size_t ply, int alpha, int beta, PVLine& pline );
};
