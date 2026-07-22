#pragma once
#include "PlayerBase.h"
#include "Board.h"		// includes Move
#include "Eval.h"
#include "SearchLimits.h"
#include "Utils\TimeManager.h"
#include "Utils/TimeUtils.h"
#include <vector>
#include <sstream>
#include <chrono>

class PlayerAiBase : public PlayerBase
{
public:
	~PlayerAiBase() = default;

	std::string getDescription() const override {
		std::stringstream str;
		str << "\n\tEngine type:\t" << GetType() <<
			"\n\tDepth:\t\t" << max_depth_ <<
			"\n\tEvaluation:\t" << Eval->GetType() << '\n';
		return str.str();
	}
	const char* GetType() const noexcept override { return "AI"; }

	void SetMaxDepth(unsigned depth) noexcept { max_depth_ = depth; }
	void SetTimeLimit(std::chrono::milliseconds ms) noexcept { time_limit_ = ms; }

	/// Configure the number of search threads (Lazy SMP). Base no-op — legacy
	/// AIs (AIBasic/AIAgent/ABIterative) ignore this; AIPerplex overrides and
	/// clamps. No threading is actually spawned yet (config plumbing only).
	virtual void SetThreads(unsigned) noexcept {}

	/// Signal the search to stop immediately (e.g. from UCI 'stop').
	/// Thread-safe: may be called from any thread.
	void StopSearch() noexcept;

	PlayerAiBase(const PlayerAiBase&) = delete;
	PlayerAiBase& operator=(const PlayerAiBase&) = delete;
	PlayerAiBase(PlayerAiBase&&) = delete;
	PlayerAiBase& operator=(PlayerAiBase&&) = delete;

protected:
	// Force use of factory by
	// Preventing constructor, copy-construction & operator=
	explicit PlayerAiBase(Board& board, unsigned md) :
		m_Board(board),
		max_depth_(md)
	{
		// Create the Evaluation strategy - Right now only possible to select two: SIMPLE and COMPLEX ;-)
	}

	//virtual Move GetMove(_Inout_ GameInfo& info) = 0;

	/* AI helper methods */

	// Quiescent soegning modvirker horisont-effekten
	int Quiescent(size_t, int, int);

	// Registers the amount of used time and prints out if PRINT_STATS is set.
	// node_count: nodes searched this move — legacy AIs pass m_SearchCount,
	// AIPerplex passes its thread-local ThreadData::nodes_searched.
	std::chrono::milliseconds StopTimerAndAdjustVars(size_t node_count) const;

	// Tilfoejer dette traek til nuvaerende traekfoelge
	// Sletter eksisterende traek fra listen fra denne ply og ned
	// Benyttes til Last Move sorting - Saetter parent node
	void AddMoveToSeq(const Move& move, size_t ply);

	// Null-move counterpart to AddMoveToSeq: no Move to derive info from, so
	// snapshot the board's current GameInfo directly. Caller must call this
	// AFTER m_Board.DoNullMove() has already been applied, using the pre-
	// recursion ply (same convention as AddMoveToSeq(move, ply)).
	void AddNullMoveToSeq(size_t ply);

	// Shared m_infoSeq size-bookkeeping used by both AddMoveToSeq and
	// AddNullMoveToSeq.
	void StoreInfoAtPly(size_t ply, const GameInfo& info);

	// Returns the best first move currently found
	virtual Move GetBestMove(_In_ GameInfo& info) noexcept;

	/// Resolves per-call SearchLimits against the configured defaults
	/// (time_limit_, max_depth_), arms time_manager_ with the resulting
	/// budget, and resets per-move search state (_startingTime,
	/// stop_search_) — replaces the old StartTimer(). (nodes_since_check_ now
	/// lives on ThreadData; see ThreadData.h.)
	/// Also stores the result in effective_depth_ for legacy AIs whose
	/// recursive Search()/Quiescent() methods read the depth bound as a
	/// member rather than a parameter; AIPerplex uses the return value
	/// directly. Returns the effective search depth for this call.
	unsigned ApplyLimits(const SearchLimits& limits);

	bool ShouldStopSearch() const noexcept
	{
		return time_manager_.should_stop_search();
	}

	/// Cheap per-node guard: only reads the latched atomic, no clock call.
	/// Use at the top of pvs()/quiescence() so the call stack collapses in O(depth)
	/// steps after the first ShouldStopSearch() fires and latches the flag.
	bool IsAborted() const noexcept
	{
		return time_manager_.is_aborted();
	}

	void SetEvalEngine(EvalManager::EvalTypes type) override
	{
		Eval = EvalManager::Create(type);	// create new eval
	}

	// ************************************
	// Method:      InitMoveVariables
	// Description: 
	// FullName:    protected PlayerAiBase::InitMoveVariables
	// Returns:     void - 
	// Parameter:   const GameInfo& info - 
	// Remark:      TODO: Burde flyttes ned som en template metode DoInit efter m_MoveSeq
	//	  		    Non Iter edition
	// ************************************
	virtual void InitMoveVariables(_In_ const GameInfo& info)
	{
		m_SearchCount = 0;

		// nulstiller parent boardInfo-sekvensen
		// Store boardInfo from after last move
		m_infoSeq.clear();
		m_infoSeq.emplace_back(info);

		// Nulstiller best move
		m_BestMove.Clear();
	}

	// Returns the Current Move's predecessor in the current move sequence tree
	const Move& GetParentMove(size_t currentPly) const
	{
		return GetLastBoardInfo(currentPly).lastMove;
	}

	const GameInfo& GetLastBoardInfo(size_t currentPly) const
	{
		// This must always contain the last move, hence the one extra info 
		return m_infoSeq.at(currentPly);
	}

	// ************************************
	// Method:      UpdateGameState
	// Description: Updates the current game state at the top level
	// FullName:    protected PlayerAiBase::UpdateGameState
	// Returns:     void
	// Parameter:   unsigned int currentPly - the current depth
	// Parameter:   const GameStates& newState - The new state
	// Remark:      
	// ************************************
	void UpdateGameState(size_t currentPly, GameStates newState)
	{
		if (currentPly == 0)
		{
			GameInfo& info = m_infoSeq.at(currentPly);
			if (newState != info.gameState)
			{
				m_Board.SetGameState(newState);	// AIPerplex uses board version
				info.gameState = newState;
			}
		}
	}

	// TODO: rename to FireStateChanged. Also, investigate if refreshInfo is needed for old AI structure
	void CheckGameOver(GameInfo& info, bool fromBoard = true)
	{
		if (fromBoard)
			info = m_Board.GetGameInfo();
		else
			info = GetLastBoardInfo(0);
		if (info.gameState != GameStates::STILL_PLAYING)
		{
			EGameStateChanged.fire(this, info.gameState);
		}
	}

	// ************************************
	// Method:      IsFiftyMoves
	// Description: Test for 50 moves rules
	// FullName:    protected PlayerAiBase::IsFiftyMoves 
	// Returns:     bool - true if 
	// Parameter:   const BoardInfo& info - 
	// Remark:		
	// ************************************
	bool checkDraws(const GameInfo& info, int ply) const noexcept
	{
		if (ply > 0 && m_Board.is_repetition(ply))
		{
			return true;
		}
		if (info.fiftyCount >= 50)
		{
			assert(info.gameState == GameStates::DRAW_50_MOVES);
			return true;
		}
		return false;
	}

	/*
	*	Protected Variables	- used by nested classes
	*/
	// Debug: taeller hvor mange gange Alpha-Beta koeres igennem
	size_t m_SearchCount{ 0 };
	// Lokal reference til Board
	Board& m_Board;

	// The embedded Eval object for per-player evaluation
	std::unique_ptr<EvalManager> Eval;

	// Store GameInfo sequence for Do/Undo TODO: Board is now handling those for AIPerplex - other algos needs to be moved
	std::vector< GameInfo > m_infoSeq;

	// Det bedste traek indtil nu
	Move m_BestMove;

	std::chrono::time_point<std::chrono::high_resolution_clock> _startingTime;

	// Time control
	std::atomic<bool> stop_search_{ false };
	chess::TimeManager time_manager_;

	// Search configuration — set from game_settings.json via SetMaxDepth / SetTimeLimit
	unsigned max_depth_{ 15 };
	std::chrono::milliseconds time_limit_{ std::chrono::seconds(15) };

	// Set by ApplyLimits() every call: the resolved depth bound for the
	// current GetMove() call (== max_depth_ unless SearchLimits overrides
	// it). Legacy AIs (AIBasic/AIAgent/ABIterative) whose recursive
	// Search()/Quiescent() methods read the depth bound as a member use
	// this instead of max_depth_, which stays the unmodified configured
	// default. AIPerplex uses ApplyLimits()'s return value directly instead.
	unsigned effective_depth_{ 0 };

	//#ifdef PRINT_STATS

		// Samlet tid og antal nodes for begge computerspillere - TODO: Separer evt til per spiller. Human burde ogsaa have en klokke
		// Lazy SMP audit (Task 1, .claude/plans/lazy-smp.md): these statics are
		// written only from StopTimerAndAdjustVars(), called once per GetMove()
		// on the calling (single) thread, strictly after that call's search has
		// returned (i.e. after any Lazy SMP helper threads for that move have
		// already joined — the plan's design never has StopTimerAndAdjustVars
		// run concurrently with an in-flight search). No synchronization is
		// added here; this comment exists so Task 3's implementer re-verifies
		// the invariant still holds once helper-thread join actually exists.
	static std::chrono::milliseconds m_TotalTime;
	static size_t m_TotalCount;
	//#endif	// PRINT_STATS
};
