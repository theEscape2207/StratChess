// TacticalTestHelpers.h — shared helpers for TacticalTests.cpp and TacticalFullTests.cpp
#pragma once
#include "AIPerplex.h"
#include "Board.h"
#include "Eval.h"
#include <memory>

// One row in the tactical test table.
struct TacticalCase {
	const char* label; // shown in Catch2 failure output via INFO()
	const char* fen;
	eSquare expected_from;
	eSquare expected_to;
	unsigned depth;
};

// Create a fresh concrete AIPerplex configured for tactical test use. The board is
// deliberately supplied to Search() rather than retained by the engine.
inline std::unique_ptr<AIPerplex> make_tactical_engine(unsigned depth)
{
	auto ai = std::make_unique<AIPerplex>(AIPerplexConfig{.default_depth = depth, .verbose_logging = false});
	AIPerplex::SetVerboseLogging(false); // suppress after ctor re-enables it
	return ai;
}

// Transitional overload for TacticalTests.cpp, which is migrated with the
// remaining PlayerAiBase callers in Task 5.
inline std::unique_ptr<PlayerBase> make_tactical_engine(Board& board, unsigned depth)
{
	auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth, board);
	AIPerplex::SetVerboseLogging(false);
	ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
	return ai;
}

// Down-casts a tactical engine to AIPerplex so tests can reach tuning_ via
// the public tuning() accessor (e.g. to toggle null_move_enabled).
inline AIPerplex& as_perplex(std::unique_ptr<AIPerplex>& ai) { return *ai; }
inline AIPerplex& as_perplex(std::unique_ptr<PlayerBase>& ai) { return *static_cast<AIPerplex*>(ai.get()); }
