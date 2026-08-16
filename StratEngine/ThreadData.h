#pragma once
#include "Board.h"
#include "PVTable.h"
#include "MoveHelper.h"
#include <cassert>
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

	// Thread-local main-tree node counter (replaces PlayerAiBase::m_SearchCount for
	// AIPerplex). Counts pvs() move edges only. Keeping quiescence out is deliberate:
	// assess_iteration_quality() and completion_ratio are calibrated against main-tree size,
	// so folding it in here would change search behaviour, not just reporting.
	int64_t nodes_searched = 0;

	// Thread-local quiescence counter, same unit as nodes_searched: one per move edge
	// searched. The two sum without overlap — a quiescence root's incoming edge belongs to
	// the main tree — and are reported both together (UCI 'nodes') and apart (Run-Bench.ps1).
	int64_t qnodes_searched = 0;

	// Node-based time-check counter (replaces PlayerAiBase::nodes_since_check_ for
	// AIPerplex). Reset alongside nodes_searched. Under Lazy SMP each helper thread
	// increments its own copy — no cross-thread contention — but only thread 0 ever
	// calls the wall-clock check (see pvs()/quiescence(): gated on thread_id == 0).
	// Helper threads rely solely on the cheap atomic IsAborted() read instead.
	int64_t nodes_since_check_ = 0;

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

	void clear_null_move_flags() noexcept { std::memset(last_move_was_null, 0, sizeof(last_move_was_null)); }

	// Resets everything that must not leak into a new game. History is
	// deliberately aged, never cleared, WITHIN a game (see the class comment
	// above) -- this is what draws that line at the game boundary instead.
	// Killers and null-move flags are already cleared at the start of every
	// move by iterative_deepening(), so clearing them again here is only for
	// the (harmless) case of something reading them before the new game's
	// first search runs. `board` is reset too even though every GetMove()
	// copy-assigns it fresh from the real game board before searching: it
	// costs nothing and avoids a stale position sitting in thread-local
	// state between games.
	void reset_for_new_game()
	{
		board = Board();
		nodes_searched = 0;
		qnodes_searched = 0;
		nodes_since_check_ = 0;
		pv_table = PVTable();
		info_seq.clear();
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

	// --- GameInfo sequence bookkeeping ---
	// Mirrors PlayerAiBase::StoreInfoAtPly/AddMoveToSeq/AddNullMoveToSeq/
	// GetLastBoardInfo/checkDraws, operating on the thread-local board and
	// info_seq. The PlayerAiBase originals remain for the legacy AI agents.

	const GameInfo& get_last_info(size_t ply) const
	{
		// This must always contain the last move, hence the one extra info
		return info_seq.at(ply);
	}

	// Faelles bookkeeping for info_seq - holder vektoren i lockstep med
	// search-traeets ply, uanset om GameInfo stammer fra et rigtigt traek
	// (add_move_to_seq) eller et null-move (add_null_move_to_seq)
	void store_info_at_ply(size_t ply, const GameInfo& info)
	{
		// where to add it? size er 2 efter foerste traek ved ply 0
		const size_t infoSize = info_seq.size();

		if (ply + 1 == infoSize) // foerste traek ved hver dybde
			info_seq.emplace_back(info);
		else if (infoSize == ply + 2) // 2. traek ved hver dybde og resten
			info_seq[ply + 1] = info;
		else if (infoSize > ply + 2) // Skal der slettes nogen? Dont delete the first!
		{
			info_seq.erase(info_seq.begin() + static_cast<int>(ply + 1), info_seq.end());
			info_seq.emplace_back(info);
		} else
			assert(!"BoardInfo update - Somebody hasn't handled all cases");
	}

	void add_move_to_seq(const Move& move, size_t ply)
	{
		GameInfo info = get_last_info(ply);

		// After DoMove the piece sits on move.to(); read it to obtain the moving piece.
		info.UpdateBoardInfo(move, board.GetPiece(move.to()));

		store_info_at_ply(ply, info);
	}

	// Null-move counterpart: no Move to derive info from, so snapshot the
	// board's current GameInfo directly (the board has already had
	// DoNullMove() applied by the caller before this is called).
	// Note: unlike add_move_to_seq, this does not call UpdateBoardInfo, so the
	// stored GameInfo::lastMove is whatever the parent ply's real move was,
	// not a sentinel for "no move". Callers must not treat lastMove at a
	// null-move ply as "the move that produced this ply".
	void add_null_move_to_seq(size_t ply) { store_info_at_ply(ply, board.GetGameInfo()); }

	// Test for 50 moves rule and threefold repetition (thread-local board)
	bool check_draws(const GameInfo& info, int ply) const noexcept
	{
		if (ply > 0 && board.is_repetition(ply)) {
			return true;
		}
		if (info.fiftyCount >= 50) {
			assert(info.gameState == GameStates::DRAW_50_MOVES);
			return true;
		}
		return false;
	}

	// Updates the game state at the root of the search tree (thread-local
	// board + info_seq[0]). AIPerplex::GetMove() propagates the result back
	// to the real game board after the search returns.
	void update_game_state(size_t ply, GameStates newState)
	{
		if (ply == 0) {
			GameInfo& info = info_seq.at(ply);
			if (newState != info.gameState) {
				board.SetGameState(newState);
				info.gameState = newState;
			}
		}
	}
};
