#pragma once
#include "Board.h"
#include "PVTable.h"
#include "MoveHelper.h"
#include <cstdint>
#include <cstring>
#include <vector>

// Per-thread search state for AIPerplex, extracted so that a future Lazy SMP
// helper thread is just "construct another ThreadData, call the same search
// functions" (see .claude/plans/extract-threaddata-structure.md).
//
// Deliberately NOT in here:
//   - TranspositionTable  — shared across threads by design; passed explicitly
//   - SearchTuning        — read-only configuration
//   - time control        — control plane owned by PlayerAiBase
struct ThreadData {
	static constexpr int MAX_KILLERS = 2;
	static constexpr int32_t HISTORY_MAX = 16'384;

	// Thread-local board copy — the search runs on this, not on the game board.
	// Copy-assigned from the game board at the start of every GetMove().
	Board board;

	// Thread-local node counter (replaces PlayerAiBase::m_SearchCount for AIPerplex).
	int64_t nodes_searched = 0;

	// Thread-local principal variation.
	PVTable pv_table;

	// Thread-local GameInfo sequence for Do/Undo bookkeeping (replaces
	// PlayerAiBase::m_infoSeq for AIPerplex).
	std::vector<GameInfo> info_seq;

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
		info_seq.reserve(32);
		clear_killers();
		clear_history();
	}

	void clear_killers() noexcept
	{
		for (auto& ply_killers : killers)
			for (auto& k : ply_killers)
				k = Move::EmptyMove();
	}

	void clear_null_move_flags() noexcept
	{
		std::memset(last_move_was_null, 0, sizeof(last_move_was_null));
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

	void clear_history() noexcept
	{
		std::memset(history, 0, sizeof(history));
	}

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
};
