// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "PlayerAI.h"
#include "Sort.h" // Different Move sorting heuristics
#include "MoveGenerator.h"
#include <spdlog/spdlog.h>
#include <iomanip> // setw() etc.

// ************************************
// Method:      Quiescent
// Description: Search beyond the horizon - captures and promotions only
// FullName:    protected PlayerAiBase::Quiescent
// Returns:     int -
// Parameter:   size_t ply - absolute search ply
// Parameter:   int alpha -
// Parameter:   int beta -
// Parameter:   int qsearch_budget - capture plies still to come
// Remark:
// ************************************
int PlayerAiBase::Quiescent(size_t ply, int alpha, int beta, int qsearch_budget)
{
	m_SearchCount++;

	// The fifty-move rule cannot trigger here: every move generated below is a capture or a
	// promotion, both of which reset the halfmove clock. Draws are tested by the callers.

	int value = eval_.Evaluate(m_Board);

	// Out of capture budget - settle for the static evaluation.
	if (qsearch_budget <= 0)
		return value;

	// Backstop: ply indexes Board's fixed-size undo history, so it must never reach MAX_PLY.
	if (ply == MAX_PLY - 10)
		return value;

	if (value > alpha) {
		if (value >= beta) // Cutoff
			return beta;

		alpha = value;
	}

	MoveList moveList;
	MoveGenerator::ComputeCaptures(m_Board, moveList);
	MoveSorter::SortMovesByValue(moveList, moveList.size(), m_Board);

	bool moveFound = false;

	for (const auto& curMove : moveList) {
		if (!m_Board.DoMove(curMove))
			continue;

		value = -Quiescent(ply + 1, -beta, -alpha, qsearch_budget - 1);

		m_Board.UndoMove(curMove);

		moveFound = true;

		if (value > alpha) {
			if (value >= beta) // Cutoff
				return beta;

			alpha = value;
		}
	}
	// Found no legal moves here
	// We don't check for mate / stalemate here, because without generating all
	// of the moves leading up to it, we don't know if the position could have
	// been avoided by one side or not.  So simply return our evaluation score
	if (!moveFound)
		return value;

	return alpha;
}

// ************************************
// Method:     GetBestMove
// Description:
// FullName:   protected PlayerAiBase::GetBestMove
// Returns:    const Move - The best move or EmptyMove if game is over
// Remark:
// ************************************
Move PlayerAiBase::GetBestMove() noexcept
{
	if (!m_BestMove.is_null())
		return m_BestMove;

	// No moves found — the search must have adjudicated the root.
	assert(root_game_state_ != GameStates::STILL_PLAYING);
	return Move::EmptyMove();
}

void PlayerAiBase::StopSearch() noexcept { search_control_.Stop(); }

unsigned PlayerAiBase::ApplyLimits(const SearchLimits& limits)
{
	search_control_.ApplyLimits(limits);
	root_game_state_ = GameStates::STILL_PLAYING;
	return search_control_.EffectiveDepth();
}
