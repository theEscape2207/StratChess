#pragma once
#include "defines.h"
#include "Move.h"
#include "PlayerAI.h"
#include "TranspositionTable.h"
#include "PVTable.h"
#include <map>
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
	Move GetMove(_Inout_ GameInfo& info) override;
	const char* GetType() const noexcept override
	{
		return "Perplexity Transpositional AlphaBeta";
	}

	// Note: NOT to be called directly - only through Factory method (needed to be public due to usage of make_unique)
	explicit AIPerplex(_In_ unsigned md);
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
	SearchResult iterative_deepening(int max_depth, TranspositionTable& tt, PVTable& pv_table);
	int search_with_aspiration(int depth, int seed_score, TranspositionTable& tt, PVTable& pv_table);
	int pvs(int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt, PVTable& pv_table);
	int adjustScoreForGameState(bool moveFound, int ply, int best_value);
	int quiescence(int alpha, int beta, int depth_q, int ply, TranspositionTable& tt);

	// HELPER METHODS
	// --------------
	// Move ordering heuristics
	void clear_killers() noexcept;
	void store_killer(int ply, const Move& move) noexcept;

	void clear_history() noexcept;
	void age_history() noexcept;
	void update_history(eColor side, const Move& move, int depth) noexcept;

	// Quality assessment
	RejectionReason assess_iteration_quality(const IterationMetrics& metrics, const SearchState& state) const;
	bool should_stop_early(int depth, int score, int pv_length) const;			// Early termination checks
	bool handle_empty_move_emergency(SearchState& state, PVTable& pv_table);	// Emergency handling
	
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

	// Killer move heuristic: two quiet moves per ply that caused a beta cutoff
	static constexpr int MAX_KILLERS = 2;
	Move killers_[MAX_PLY][MAX_KILLERS];

	// History heuristic: accumulated score for quiet moves that caused beta cutoffs,
	// indexed by [side-to-move][from-square][to-square].
	// int32 gives plenty of headroom before the depth^2 increments overflow.
	static constexpr int HISTORY_MAX = 16'384;
	int32_t history_[2][64][64];

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
	// Debug helpers
	void debug_tt_cache_misses(unsigned int key, int ply);
	void assert_tt_store(const TranspositionTable& tt, std::uint64_t key, int16_t ply,
		[[maybe_unused]] int16_t value, [[maybe_unused]] int16_t depth, Move best_move,
		[[maybe_unused]] BoundType bound, NodeType node_type, [[maybe_unused]] SearchPhase phase);
	std::multimap<std::uint64_t, int> tt_misses;
};