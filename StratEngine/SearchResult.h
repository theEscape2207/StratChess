#pragma once

#include "Move.h"
#include "GameState.h"
#include <chrono>
#include <cstdint>

// What one GetMove() call produced: the move to play, and everything the caller needs to know
// about how it was reached. It lives in its own header because IPlayer returns it and cannot
// include a concrete engine.
//
// game_state is the outcome the player adjudicated at its own root — a mate, a stalemate or a
// resignation. It is never DRAW_50_MOVES: the fifty-move rule is a fact about the position
// after the move is committed, which only the game controller can see.
//
// Legacy agents report their unsplit combined work in nodes_searched and leave
// qnodes_searched at its default; the human player leaves both counters at their defaults.
struct SearchResult {
	Move best_move = Move::EmptyMove();
	int best_score = 0;
	int depth_completed = 0;
	GameStates game_state = GameStates::STILL_PLAYING;
	// AIPerplex keeps the two trees apart here and callers sum them only where a
	// total is reported. Legacy agents place their already-combined counter in
	// nodes_searched and leave qnodes_searched at zero.
	// Construct with DESIGNATED initializers: a member inserted mid-struct shifts every
	// positional initializer after it, and bool -> int64_t promotes rather than narrows,
	// so /W4 /WX does not catch the shift.
	int64_t nodes_searched = 0;
	int64_t qnodes_searched = 0;
	bool search_was_stable = true;
	std::chrono::milliseconds elapsed{0};

	// Singular-extension trigger counts for this search, summed over every thread. All zero
	// unless the feature is enabled, which is what lets the reporting side stay silent in the
	// shipped configuration. They measure how often the heuristic fires, not whether it helped:
	// only a match can answer that.
	int64_t singular_eligible = 0;
	int64_t singular_verifications = 0;
	int64_t singular_extensions = 0;
	// Node edges spent inside verification searches, measured rather than inferred.
	int64_t singular_verification_nodes = 0;
};
