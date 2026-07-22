#pragma once
#include "defines.h"
#include "Move.h"
#include "PlayerAI.h"
#include "TranspositionTable.h"
#include "PVTable.h"
#include "ThreadData.h"
#include <memory>
#include <cstdint>

// Search result structure - returned from iterative_deepening
struct SearchResult {
	Move best_move = Move::EmptyMove();
	int best_score = 0;
	int depth_completed = 0;
	int64_t nodes_searched = 0;
	bool search_was_stable = true;
};

class AIPerplex final : public PlayerAiBase
{
public:
	Move GetMove(_Inout_ GameInfo& info, const SearchLimits& limits) override;
	const char* GetType() const noexcept override
	{
		return "Perplexity Transpositional AlphaBeta";
	}

	// Note: NOT to be called directly - only through Factory method (needed to be public due to usage of make_unique)
	explicit AIPerplex(Board& board, _In_ unsigned md);
	~AIPerplex() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	AIPerplex(const AIPerplex&) = delete;
	AIPerplex& operator=(const AIPerplex&) = delete;
	AIPerplex(AIPerplex&&) = delete;
	AIPerplex& operator=(AIPerplex&&) = delete;

private:
	// TUNABLE SEARCH PARAMETERS
	struct SearchTuning {
		int64_t min_nodes_threshold = 1000;        // Minimum nodes for valid search
		double min_completion_ratio = 0.10;        // Must search 10% of previous depth
		double min_pv_ratio = 0.33;                // PV must be at least 1/3 of depth
		int score_draw_threshold = 20;             // Score delta to detect suspicious 0

		int delta_pruning_margin = 200;  // centipawns — added to stand_pat + capture_value before comparing to alpha in quiescence

		int aspiration_initial_delta = 50;  // centipawns; initial half-width on each side
		int aspiration_max_retries   = 4;   // widen iterations before opening full window
		bool aspiration_enabled      = true; // runtime kill-switch for regression testing

		int  lmr_min_depth      = 3;    // don't reduce at depth < 3
		int  lmr_min_move_index = 3;    // don't reduce the first 3 moves (si 0, 1, 2)
		bool lmr_enabled        = true; // kill-switch: set false to measure LMR impact via SimplePerfStats.txt

		// Null-move pruning tuning
		bool null_move_enabled  = true;  // enabled by default (validated via tests + self-play; see .claude/plans/null-move-pruning.md)
		int  null_move_reduction = 3;    // reduction R used in null-move search (depth -> depth-1-R)
		int  null_move_min_depth = 3;    // minimum depth to attempt null-move pruning
	} tuning_;

	// INTERNAL STRUCTURES
	struct IterationMetrics {
		int depth;
		Move current_move;
		int current_score;
		int64_t nodes_searched;
		int pv_length;
		bool interrupted;
		bool move_changed;

		// Computed values
		int score_delta;
		double completion_ratio;
	};

	struct SearchState {
		Move best_move;
		int best_score;
		int depth_completed;
		int64_t nodes_at_completed_depth;
		Move last_iteration_move;
		bool search_was_stable;

		SearchState()
			: best_move(Move::EmptyMove())
			, best_score(0)
			, depth_completed(0)
			, nodes_at_completed_depth(0)
			, last_iteration_move(Move::EmptyMove())
			, search_was_stable(true)
		{}
	};

	enum class IterationDecision {
		ACCEPT_AND_CONTINUE,    // Use this depth, keep going
		ACCEPT_AND_STOP,        // Use this depth, stop iteration
		REJECT_AND_STOP         // Reject this depth, use previous
	};

	enum class RejectionReason {
		NONE,
		INCOMPLETE,
		TOO_FEW_NODES,
		SHORT_PV,
		SCORE_DROP,
		MOVE_CHANGED
	};

	// SEARCH METHODS
	// --------------
	// ThreadData is always the first parameter: the search runs entirely on the
	// per-thread state it carries, while the TranspositionTable stays a separate
	// explicit parameter because it is shared across threads under Lazy SMP.
	void init_search(const GameInfo& info);
	SearchResult iterative_deepening(ThreadData& td, int max_depth, TranspositionTable& tt);
	int search_with_aspiration(ThreadData& td, int depth, int seed_score, TranspositionTable& tt);
	int pvs(ThreadData& td, int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt);
	int adjustScoreForGameState(ThreadData& td, bool moveFound, int ply, int best_value);
	int quiescence(ThreadData& td, int alpha, int beta, int depth_q, int ply, TranspositionTable& tt);

	// HELPER METHODS
	// --------------
	// Quality assessment
	RejectionReason assess_iteration_quality(const IterationMetrics& metrics, const SearchState& state) const;
	bool should_stop_early(int depth, int score, int pv_length) const;			// Early termination checks
	bool handle_empty_move_emergency(ThreadData& td, SearchState& state);		// Emergency handling
	bool should_try_null_move(const ThreadData& td, int depth, int beta, int ply, bool is_pv_node, bool in_check) const;
	
	// Logging helpers
	void log_iteration_eval(const IterationMetrics& metrics, const PVTable& pv_table) const;
	void log_rejection(int depth, RejectionReason reason, const IterationMetrics& metrics, const SearchState& state) const;
	void log_acceptance(const IterationMetrics& metrics) const;
	void log_search_complete(const AIPerplex::SearchState& state, const PVTable& pv_table) const;
	void log_completed_iteration(const AIPerplex::IterationMetrics& metrics, const PVTable& pv_table) const;
	void log_aspiration_retry(int depth, int retry, int score, int alpha, int beta, bool fail_low) const;
	void log_aspiration_full_window(int depth, int max_retries) const;
	
	// MEMBER VARIABLES
	std::unique_ptr<TranspositionTable> _tt;	// persistent transposition table

	// Per-thread search state (board copy, node counter, PV, killers, history, ...).
	// Persistent member — history is aged between moves, never cleared — and the
	// single instance used by the (currently single-threaded) search. Lazy SMP
	// helper threads will each get their own. See ThreadData.h.
	ThreadData td_;

	// logging control: enable detailed logging when needed (default: false)
	static inline bool s_verbose_logging = false;

#ifdef STRAT_ENABLE_TEST_ACCESS
	// Enable fine-grained unit tests for private search helpers.
	// Activated by defining STRAT_ENABLE_TEST_ACCESS in the test project
	// preprocessor settings (StratChessTests.vcxproj) — never in production.
	// See Docs/TestDesign.md §"AIPerplex Test Access" and §Phase 1 [search] tests.
	friend class AIPerlexTestFixture;
#endif

public:
	// Configure logger verbosity at runtime (call before heavy runs if needed).
	// When enabled, ensures the AIPerplex logger is initialized and sets its level to debug.
	// When disabled, silences the logger via spdlog::level::off (log helpers need only a null-check).
	static void SetVerboseLogging(bool enabled) noexcept;
	static bool IsVerboseLoggingEnabled() noexcept { return s_verbose_logging; }

	// Access to tuning parameters
	SearchTuning& tuning() { return tuning_; }
	const SearchTuning& tuning() const { return tuning_; }

private:
	SearchResult last_result_{};

public:
	/// Returns the result of the most recent search (valid after GetMove() returns).
	[[nodiscard]] SearchResult GetLastResult() const noexcept { return last_result_; }
};