// TacticalTestHelpers.h — shared helpers for TacticalTests.cpp and TacticalFullTests.cpp
#pragma once
#include "AIPerplex.h"
#include "Eval.h"
#include <memory>

// One row in the tactical test table.
struct TacticalCase {
    const char* label;        // shown in Catch2 failure output via INFO()
    const char* fen;
    eSquare     expected_from;
    eSquare     expected_to;
    unsigned    depth;
};

// Create a fresh AIPerplex at the given depth, configured for test use.
// Call before ai->GetMove() — AIPerplex reads board state at search time, not at construction time.
inline std::unique_ptr<PlayerBase> make_tactical_engine(unsigned depth)
{
    auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth);
    AIPerplex::SetVerboseLogging(false);          // suppress after ctor re-enables it
    ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
    return ai;
}

// Down-casts a tactical engine to AIPerplex so tests can reach tuning_ via
// the public tuning() accessor (e.g. to toggle null_move_enabled).
inline AIPerplex& as_perplex(std::unique_ptr<PlayerBase>& ai)
{
    return *static_cast<AIPerplex*>(ai.get());
}
