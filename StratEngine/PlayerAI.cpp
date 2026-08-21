// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "PlayerAI.h"
#include "Sort.h" // Different Move sorting heuristics
#include "MoveGenerator.h"
#include <spdlog/spdlog.h>
#include <iomanip> // setw() osv.

// ************************************
// Method:      Quiescent
// Description: Soegning udover horisonten - p.t. kun slagudvekslinger
// FullName:    protected PlayerAiBase::Quiescent
// Returns:     int -
// Parameter:   unsigned ply - depth
// Parameter:   int alpha -
// Parameter:   int beta -
// Remark:
// ************************************
int PlayerAiBase::Quiescent(size_t ply, int alpha, int beta)
{
	// Increments counter
	m_SearchCount++;

	// 50 moves rules will never get hit as long we only have Captures in Quiescent moves
	// first entry is tested in Search algorithms

	// Evaluerer paa stillingen
	int value = Eval->Evaluate(m_Board);

	// Begraenser quiescent til en maksimal dybde
	if (ply == MAX_PLY - 10)
		return value;

	if (value > alpha) // Er det en bedre vaerdi?
	{
		if (value >= beta) // Cutoff !
			return beta;

		alpha = value;
	}

	MoveList moveList;
	// Only work on the captures in Quiescent
	MoveGenerator::ComputeCaptures(m_Board, moveList);
	// Sort the found captures
	MoveSorter::SortMovesByValue(moveList, moveList.size(), m_Board);

	// Tjek om der er lovlige brugbare traek her
	bool moveFound = false;

	// vi loeber dem allesammen igennem
	for (const auto& curMove : moveList) {
		// Foretag traekket
		if (!m_Board.DoMove(curMove))
			continue;

		// rekursivt kald til Quiescent. Dette sker _rigtigt_ mange gange
		value = -Quiescent(ply + 1, -beta, -alpha);

		// Vi er tilbage igen. Undo traekket igen
		m_Board.UndoMove(curMove);

		moveFound = true;

		if (value > alpha) // Er det en bedre vaerdi?
		{
			if (value >= beta) // For hoej. Cutoff !
				return beta;

			alpha = value; // Ny bedste vaerdi
		}
	}
	// Found no legal moves here
	// We don't check for mate / stalemate here, because without generating all
	// of the moves leading up to it, we don't know if the position could have
	// been avoided by one side or not.  So simply return our evaluation score
	if (!moveFound)
		return value;

	// return den bedste alpha-vaerdi
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
	// Returner det bedste traek - hvis der er noget
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
