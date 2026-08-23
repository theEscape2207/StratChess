#pragma once
#include "Board.h"
#include "PVTable.h"
#include "MoveHelper.h"
#include <cassert>
#include <cstdint>
#include <cstring>

// Per-thread search state for AIPerplex, extracted so that a future Lazy SMP
// helper thread is just "construct another ThreadData, call the same search
// functions".
//
// Deliberately NOT in here:
//   - TranspositionTable  — shared across threads by design; passed explicitly
//   - SearchTuning        — read-only configuration
//   - time control        — SearchControl owned by AIPerplex
struct ThreadData {
	static constexpr int MAX_KILLERS = 2;
	static constexpr int32_t HISTORY_MAX = 16'384;

	// Thread-local board copy — the search runs on this, not on the game board.
	// Copy-assigned from the Search() root at the start of every search.
	Board board;

	// Game outcome adjudicated at this thread's root (ply 0). Thread-local rather than a
	// single AIPerplex-level member because adjustScoreForGameState() runs on every Lazy
	// SMP helper thread, each writing its own root at ply 0 concurrently — a shared member
	// would be a data race.
	GameStates root_game_state = GameStates::STILL_PLAYING;

	// Thread-local main-tree node counter owned by AIPerplex. Counts pvs() move edges only.
	// Keeping quiescence out is deliberate:
	// assess_iteration_quality() and completion_ratio are calibrated against main-tree size,
	// so folding it in here would change search behaviour, not just reporting.
	int64_t nodes_searched = 0;

	// Thread-local quiescence counter, same unit as nodes_searched: one per move edge
	// searched. The two sum without overlap — a quiescence root's incoming edge belongs to
	// the main tree — and are reported both together (UCI 'nodes') and apart (Run-Bench.ps1).
	int64_t qnodes_searched = 0;

	// Node-based SearchControl polling counter. Reset alongside nodes_searched. Under Lazy SMP each helper thread
	// increments its own copy — no cross-thread contention — but only thread 0 ever
	// calls the wall-clock check (see pvs()/quiescence(): gated on thread_id == 0).
	// Helper threads rely solely on the cheap atomic IsAborted() read instead.
	int64_t nodes_since_check_ = 0;

	// Thread-local principal variation.
	PVTable pv_table;

	// For debugging/logging once helper threads exist; main thread is 0.
	int thread_id = 0;

	// Killer move heuristic: two quiet moves per ply that caused a beta cutoff.
	Move killers[MAX_PLY][MAX_KILLERS];

	// Null-move consecutive-pass guard: last_move_was_null[ply] is true when
	// the move that led to this ply was itself a null move. Indexed the same
	// way as killers; cleared at search start and reset immediately after
	// each null-move attempt completes (see AIPerplex::pvs()).
	bool last_move_was_null[MAX_PLY]{};

	// History heuristic: accumulated score for quiet moves that caused beta cutoffs,
	// indexed by [side-to-move][from-square][to-square].
	// int32 gives plenty of headroom before the depth^2 increments overflow.
	int32_t history[2][64][64];

	ThreadData()
	{
		clear_killers();
		clear_history();
	}

	void clear_killers() noexcept
	{
		for (auto& ply_killers : killers)
			for (auto& k : ply_killers)
				k = Move::EmptyMove();
	}

	void clear_null_move_flags() noexcept { std::memset(last_move_was_null, 0, sizeof(last_move_was_null)); }

	// Resets everything that must not leak into a new game. History is
	// deliberately aged, never cleared, WITHIN a game (see the class comment
	// above) -- this is what draws that line at the game boundary instead.
	// Killers and null-move flags are already cleared at the start of every
	// move by iterative_deepening(), so clearing them again here is only for
	// the (harmless) case of something reading them before the new game's
	// first search runs. `board` is reset too even though every Search()
	// copy-assigns it fresh from the supplied root before searching: it
	// costs nothing and avoids a stale position sitting in thread-local
	// state between games.
	void reset_for_new_game()
	{
		board = Board();
		nodes_searched = 0;
		qnodes_searched = 0;
		nodes_since_check_ = 0;
		pv_table = PVTable();
		clear_killers();
		clear_null_move_flags();
		clear_history();
	}

	void store_killer(int ply, const Move& move) noexcept
	{
		// Only quiet moves are stored as killers
		if (MoveHelper::IsCapture(move))
			return;
		// Avoid storing the same move twice in slot 0
		if (killers[ply][0] == move)
			return;
		// Shift slot 0 to slot 1, then store new killer in slot 0
		killers[ply][1] = killers[ply][0];
		killers[ply][0] = move;
	}

	void clear_history() noexcept { std::memset(history, 0, sizeof(history)); }

	void age_history() noexcept
	{
		// Halve all scores between iterative-deepening depths so that older
		// cutoff information fades rather than being discarded entirely.
		// Scores from deeper searches stay proportionally larger.
		for (auto& side : history)
			for (auto& from : side)
				for (auto& score : from)
					score >>= 1;
	}

	void update_history(eColor side, const Move& move, int depth) noexcept
	{
		// Only quiet moves contribute to the history table
		if (MoveHelper::IsCapture(move))
			return;
		// Bonus scales with depth^2 so deep cutoffs outweigh shallow ones
		int32_t& entry = history[side][move.from()][move.to()];
		entry += depth * depth;
		// Cap to avoid int32 overflow after many iterations
		if (entry > HISTORY_MAX)
			entry = HISTORY_MAX;
	}

	// Threefold repetition and the fifty-move rule (thread-local board). Neither applies at
	// the root: the caller asked for a move, not an adjudication, and a draw returned there
	// leaves the search with nothing to report but the emergency move.
	bool check_draws(int ply) const noexcept
	{
		if (ply == 0)
			return false;
		return board.is_repetition(ply) || board.halfmove_clock() >= HALFMOVE_CLOCK_LIMIT;
	}

	// Updates the game state adjudicated at the root of the search tree.
	void update_game_state(size_t ply, GameStates newState)
	{
		if (ply == 0)
			root_game_state = newState;
	}
};
