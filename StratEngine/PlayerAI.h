#pragma once
#include "PlayerBase.h"
#include "Board.h" // includes Move
#include "Eval.h"
#include "SearchLimits.h"
#include "SearchControl.h"
#include <sstream>
#include <chrono>

class PlayerAiBase : public PlayerBase {
  public:
	~PlayerAiBase() = default;

	std::string getDescription() const override
	{
		std::stringstream str;
		str << "\n\tEngine type:\t" << GetType() << "\n\tDepth:\t\t" << max_depth_ << "\n\tEvaluation:\t"
		    << Eval->GetType() << '\n';
		return str.str();
	}
	const char* GetType() const noexcept override { return "AI"; }

	void SetMaxDepth(unsigned depth) noexcept
	{
		max_depth_ = depth;
		search_control_.SetDefaults(max_depth_, time_limit_);
	}
	void SetTimeLimit(std::chrono::milliseconds ms) noexcept
	{
		time_limit_ = ms;
		search_control_.SetDefaults(max_depth_, time_limit_);
	}

	struct HashConfigurationResult {
		bool success{false};
		unsigned requested_mb{0};
		size_t entry_mb{0};
		size_t bucket_count{0};
	};

	/// Replace the transposition table for a client-supplied entry-memory budget.
	/// Unsupported legacy AIs return failure; AIPerplex returns the actual allocation.
	virtual HashConfigurationResult SetHash(unsigned) noexcept { return {}; }

	/// Configure the number of search threads (Lazy SMP). Base no-op — legacy
	/// AIs (AIBasic/AIAgent/ABIterative) ignore this; AIPerplex overrides and
	/// clamps. No threading is actually spawned yet (config plumbing only).
	virtual void SetThreads(unsigned) noexcept {}

	/// Reset per-game search state before the first move of a new game.
	/// Legacy AIs have no persistent state that needs an explicit reset.
	virtual void StartNewGame() {}

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
	explicit PlayerAiBase(Board& board, unsigned md)
	    : m_Board(board), max_depth_(md), search_control_(md, std::chrono::seconds(15))
	{
		// Create the Evaluation strategy - Right now only possible to select two: SIMPLE and COMPLEX ;-)
	}

	/* AI helper methods */

	// Quiescent soegning modvirker horisont-effekten
	int Quiescent(size_t, int, int);

	// Returns the best first move currently found
	virtual Move GetBestMove() noexcept;

	// Legacy searches report all work as unsplit nodes because their shared counter includes
	// both main and quiescence work.
	SearchResult MakeResult() noexcept
	{
		return {.best_move = GetBestMove(),
		        .best_score = GetBestScore(),
		        .game_state = root_game_state_,
		        .nodes_searched = static_cast<int64_t>(m_SearchCount),
		        .elapsed = search_control_.Elapsed()};
	}

	/// Resolves per-call SearchLimits against the configured defaults, arms the
	/// composed search control, and resets the per-move root verdict.
	unsigned ApplyLimits(const SearchLimits& limits);

	/// Has anything asked this search to stop? True when the abort flag is
	/// already latched — by a node limit, by UCI 'stop' via StopSearch(), or by
	/// the end of a previous search — and otherwise when the hard clock limit has
	/// just expired, which latches it. Deliberately not named for the clock: the
	/// flag has carried more than one reason since UCI 'stop' existed, and a name
	/// claiming otherwise is what a reader of the abort path would trust.
	bool StopRequested() const noexcept { return search_control_.StopRequested(); }

	/// Node budget: has this search used its allowance? Latches the same abort
	/// flag as the clock, so the stack collapse and every IsAborted() consumer
	/// need not know which limit stopped them. Unlimited unless the caller asked
	/// for a node budget, in which case a false answer costs one optional test.
	/// Only AIPerplex polls this; the legacy agents accept a node limit through
	/// ApplyLimits() and then ignore it, so for them only the depth cap bounds
	/// a nodes-only search.
	/// @param nodes  Nodes searched so far by the polling thread — under Lazy
	///               SMP that is thread 0's count, matching the clock check.
	bool NodeLimitReached(int64_t nodes) noexcept { return search_control_.NodeLimitReached(nodes); }

	/// Cheap per-node guard: only reads the latched atomic, no clock call.
	/// Use at the top of pvs()/quiescence() so the call stack collapses in O(depth)
	/// steps after the first StopRequested() or NodeLimitReached() fires and
	/// latches the flag.
	bool IsAborted() const noexcept { return search_control_.IsAborted(); }

	bool ShouldStopIteration() const noexcept { return search_control_.ShouldStopIteration(); }

	unsigned EffectiveDepth() const noexcept { return search_control_.EffectiveDepth(); }

	void SetEvalEngine(EvalManager::EvalTypes type) override
	{
		Eval = EvalManager::Create(type); // create new eval
	}

	// ************************************
	// Method:      InitMoveVariables
	// Description:
	// FullName:    protected PlayerAiBase::InitMoveVariables
	// Returns:     void -
	// Remark:      TODO: Burde flyttes ned som en template metode DoInit
	//	  		    Non Iter edition
	// ************************************
	virtual void InitMoveVariables()
	{
		m_SearchCount = 0;

		// Nulstiller best move
		m_BestMove.Clear();
	}

	// The move that led to the node being searched — the board is the only object that
	// tracks it, and move ordering is its only consumer.
	Move GetParentMove() const { return m_Board.last_move(); }

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
			root_game_state_ = newState;
	}

	// Threefold repetition and the fifty-move rule. Both are draws by position history, so
	// neither applies at the root: the caller asked for a move, not for an adjudication.
	bool checkDraws(int ply) const noexcept
	{
		if (ply == 0)
			return false;
		return m_Board.is_repetition(ply) || m_Board.halfmove_clock() >= HALFMOVE_CLOCK_LIMIT;
	}

	/*
	*	Protected Variables	- used by nested classes
	*/
	// Debug: taeller hvor mange gange Alpha-Beta koeres igennem
	size_t m_SearchCount{0};
	// Lokal reference til Board
	Board& m_Board;

	// Game outcome adjudicated at the root of the current GetMove() call. Legacy agents are
	// single-threaded, so unlike ThreadData::root_game_state a single member here is safe.
	GameStates root_game_state_ = GameStates::STILL_PLAYING;

	// The embedded Eval object for per-player evaluation
	std::unique_ptr<EvalManager> Eval;

	// Det bedste traek indtil nu
	Move m_BestMove;

	// Search configuration — set from game_settings.json via SetMaxDepth / SetTimeLimit
	unsigned max_depth_{15};
	std::chrono::milliseconds time_limit_{std::chrono::seconds(15)};

	SearchControl search_control_;

#ifdef STRAT_ENABLE_TEST_ACCESS
	// Grants a legacy-agent test fixture (StratChessTests/SearchTests.cpp) access to
	// root_game_state_ so a test can prove it is reset per GetMove() call, mirroring
	// AIPerlexTestFixture's access to AIPerplex::td_.root_game_state.
	friend class LegacyAiTestFixture;
#endif
};
