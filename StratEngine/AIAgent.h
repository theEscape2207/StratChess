#pragma once
#include "PlayerAiIterBase.h"

class Move;

class AIAgent final
	: public PlayerAiIterBase
{
public:
	// Implementation/overrides of the IPlayer interface
	Move GetMove(_Inout_ GameInfo& info) override;
	const char* GetType() const noexcept override
	{	return "AI Agent";	}

	// Note: NOT to be called directly - only through Factory method
	explicit AIAgent(_In_ unsigned md) : PlayerAiIterBase(md) {}
	~AIAgent() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	AIAgent(const AIAgent&) = delete;
	AIAgent& operator=(const AIAgent&) = delete;
	AIAgent(AIAgent&&) = delete;
	AIAgent& operator=(AIAgent&&) = delete;
private:
	int Search( _In_ size_t ply, _In_ int alpha, _In_ int beta, _Inout_ PVLine& pline );
};
