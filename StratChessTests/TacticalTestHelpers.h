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
// deliberately supplied to Search() rather than retained by the engine. A 1 MiB table
// (TestDesign.md → Test Isolation Rules).
inline std::unique_ptr<AIPerplex> make_tactical_engine(unsigned depth, bool null_move_enabled = true)
{
	SearchTuning tuning;
	tuning.null_move_enabled = null_move_enabled;
	auto ai = std::make_unique<AIPerplex>(
	    AIPerplexConfig{.default_depth = depth, .hash_mb = 1, .tuning = tuning, .verbose_logging = false});
	ai->StartNewGame();
	return ai;
}
