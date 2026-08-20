// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "PlayerAI.h"
#include "Utils/TimeUtils.h"
#include "Sort.h" // Different Move sorting heuristics
#include "MoveGenerator.h"
#include "Utils/Logger.h"
#include <spdlog/spdlog.h>
#include <iomanip> // setw() osv.

// static variables
std::chrono::milliseconds PlayerAiBase::m_TotalTime = std::chrono::milliseconds(0);
size_t PlayerAiBase::m_TotalCount = 0;

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

// ************************************
// Method:      StopTimer
// Description: Updates time statistics - and prints it to file if PRINT_STATS is defined
// FullName:    protected PlayerAiBase::StopTimer
// Returns:     void -
// Remark:
// ************************************
std::chrono::milliseconds PlayerAiBase::StopTimerAndAdjustVars(size_t node_count) const
{
	auto end = std::chrono::high_resolution_clock::now();
	auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - _startingTime);
	if (elapsedMs == std::chrono::milliseconds(0))
		elapsedMs = std::chrono::milliseconds(1);
	m_TotalTime += elapsedMs;
	m_TotalCount += node_count;

	//#ifdef PRINT_STATS
	// Use the perf logger only if one already exists -- creating it here would write into the
	// process CWD. Game::Init() is the sole creator, so in non-game contexts (tests, tactical
	// runner, UCI) this is a no-op.
	auto perf = Engine::Logger::GetPerfLogger();

	if (perf) {
		// preserve column layout using fmt width specifiers and spaces between columns
		// Columns: node_count | elapsedMs.count() | nodes/ms | m_TotalCount | m_TotalTime.count() | total nodes/ms
		// Use integer arithmetic for nodes per ms (same as original)
		long nodes_per_ms = 0;
		if (elapsedMs.count() != 0)
			nodes_per_ms = static_cast<long>(node_count / elapsedMs.count());
		long total_nodes_per_ms = 0;
		if (m_TotalTime.count() != 0)
			total_nodes_per_ms = static_cast<long>(m_TotalCount / m_TotalTime.count());

		// Format with right-aligned columns similar to setw in original code
		perf->info("{:>10} {:>13} {:>13} {:>19} {:>13} {:>13}", node_count, elapsedMs.count(), nodes_per_ms,
		           m_TotalCount, m_TotalTime.count(), total_nodes_per_ms);
	}
	// If perf logger is unavailable (e.g. logs/ absent in UCI mode), skip silently.
	// Writing to std::cout here would corrupt the UCI output stream.

	//#endif // PRINT_STATS
	return elapsedMs;
}

void PlayerAiBase::StopSearch() noexcept { time_manager_.stop(); }

unsigned PlayerAiBase::ApplyLimits(const SearchLimits& limits)
{
	const auto r = Engine::resolve_limits(limits, time_limit_, max_depth_);
	_startingTime = std::chrono::high_resolution_clock::now();
	time_manager_.start(r.budget.soft, r.budget.hard);
	stop_search_.store(false, std::memory_order_relaxed);
	effective_depth_ = r.effective_depth;
	node_limit_ = r.node_limit;
	root_game_state_ = GameStates::STILL_PLAYING;
	return r.effective_depth;
}
