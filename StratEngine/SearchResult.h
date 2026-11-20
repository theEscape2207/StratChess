#pragma once

#include "Move.h"
#include "GameState.h"
#include "SearchTelemetry.h"
#include <chrono>
#include <cstdint>
#include <vector>

// One ranked root move of a multi-PV search (AIPerplex::SetMultiPV): its exact score at the result's
// depth_completed and its own line, which starts with move.
struct RootLine {
	Move move = Move::EmptyMove();
	int score = 0;
	std::vector<Move> pv{};
};

// What one GetMove() call produced: the move to play, and everything the caller needs to know
// about how it was reached. It lives in its own header because IPlayer returns it and cannot
// include a concrete engine.
//
// game_state is the outcome the player adjudicated at its own root — a mate, a stalemate or a
// resignation. It is never DRAW_50_MOVES: the fifty-move rule is a fact about the position
// after the move is committed, which only the game controller can see.
//
// The human player leaves both node counters at their defaults.
struct SearchResult {
	Move best_move = Move::EmptyMove();
	int best_score = 0;
	int depth_completed = 0;
	GameStates game_state = GameStates::STILL_PLAYING;
	// AIPerplex keeps the two trees apart here and callers sum them only where a
	// total is reported.
	// Construct with DESIGNATED initializers: a member inserted mid-struct shifts every
	// positional initializer after it, and bool -> int64_t promotes rather than narrows,
	// so /W4 /WX does not catch the shift.
	int64_t nodes_searched = 0;
	int64_t qnodes_searched = 0;
	int hashfull = 0;
	bool search_was_stable = true;
	std::chrono::milliseconds elapsed{0};

	// Trigger counters for this search, summed over every thread; see SearchTelemetry.h. They
	// measure how often a heuristic fires, not whether it helped: only a match can answer that.
	SearchTelemetry telemetry{};

	// AIPerplex: the accepted iteration's line, starting with best_move, and empty when best_move is
	// null. The human player leaves it empty.
	std::vector<Move> pv{};

	// AIPerplex: the main thread's deepest ply in the accepted iteration, or for an emergency move
	// in the interrupted one. May be below depth_completed when every line ends early (a fifty-move
	// draw at ply 1); the UCI line clamps it.
	int seldepth = 0;

	// AIPerplex at MultiPV N > 1: ranks 2..N, best first, from the same completed depth as best_move,
	// which stays rank 1. Scores never exceed best_score and every move is distinct. Its size is
	// min(N, legal root moves) - 1 whenever a depth completed, with one exception: when a limit lands
	// inside depth 1's set after rank 1 finished, the result is that rank 1 alone (depth_completed 1)
	// and this is empty. A consumer that needs a ranked set checks the size. Empty at N = 1, for an
	// emergency move and at a terminal root.
	std::vector<RootLine> alternatives{};
};
