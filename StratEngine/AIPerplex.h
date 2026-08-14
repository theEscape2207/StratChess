#pragma once
#include "defines.h"
#include "Move.h"
#include "PlayerAI.h"
#include "TranspositionTable.h"
#include "PVTable.h"
#include "ThreadData.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// Search result structure - returned from iterative_deepening
struct SearchResult {
	Move best_move = Move::EmptyMove();
	int best_score = 0;
	int depth_completed = 0;
	int64_t nodes_searched = 0;
	bool search_was_stable = true;
};

// Snapshot of one accepted iterative-deepening iteration, handed to the
// iteration observer (see AIPerplex::SetIterationObserver). `nodes` is the
// CUMULATIVE main-search-thread node count at the end of this accepted
// iteration (td.nodes_searched) — the standard UCI convention for a
// per-iteration "nodes so far" figure, not the per-iteration delta
// IterationMetrics tracks. It is NOT guaranteed to equal the final
// info/bestmove line's node count: on a clocked search the loop typically
// starts one more iteration, gets interrupted, and has that iteration
// rejected by assess_iteration_quality() (REJECT_AND_STOP emits nothing —
// see iterative_deepening()), but the rejected iteration's nodes are already
// in td.nodes_searched by the time GetMove() reports the final total, so
// that total is typically strictly greater than this field at Threads=1.
// Under Lazy SMP the two also diverge because the final total sums helper
// threads' nodes, which are never visible here. `pv` is a copy of the PV
// table's root line, taken at emit time before the next iteration
// overwrites it.
struct IterationInfo {
	int depth = 0;
	int score = 0;
	int64_t nodes = 0;
	std::vector<Move> pv;
};

class AIPerplex final : public PlayerAiBase {
  public:
	Move GetMove(GameInfo& info, const SearchLimits& limits) override;
	const char* GetType() const noexcept override { return "Perplexity Transpositional AlphaBeta"; }

	// Configure the number of Lazy SMP search threads; clamps to [1, 32].
	// GetMove() spawns threads_ - 1 helper std::jthreads sharing the
	// transposition table with the main search.
	void SetThreads(unsigned n) noexcept override { threads_ = std::clamp(n, 1u, 32u); }
	// MAX_HASH_MB = 1536 is a deliberate policy cap, not the largest exact fit.
	// Steady-state total is about 1664 MiB on Windows or 2432 MiB on Linux including locks;
	// construct-before-replace briefly holds old and new tables, roughly doubling the peak.
	static constexpr unsigned DEFAULT_HASH_MB = 192;
	static constexpr unsigned MIN_HASH_MB = 1;
	static constexpr unsigned MAX_HASH_MB = 1536;

	HashConfigurationResult SetHash(unsigned mb) noexcept override;
	void StartNewGame() override;

	// Registers a callback invoked once per accepted iterative-deepening iteration
	// (both ACCEPT_AND_CONTINUE and ACCEPT_AND_STOP, never REJECT_AND_STOP), from
	// whichever thread is running iterative_deepening() — the main search thread
	// only; Lazy SMP helper threads run helper_loop() and never call this. Empty
	// by default: no observer means no per-iteration work at all, so game mode and
	// non-UCI callers that never register one are unaffected. UCIHandler::cmd_go
	// is the only current caller — it registers before spawning the search thread
	// and clears the observer once the search returns.
	void SetIterationObserver(std::function<void(const IterationInfo&)> observer)
	{
		iteration_observer_ = std::move(observer);
	}

	// Note: NOT to be called directly - only through Factory method (needed to be public due to usage of make_unique)
	explicit AIPerplex(Board& board, unsigned md);
	~AIPerplex() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	AIPerplex(const AIPerplex&) = delete;
	AIPerplex& operator=(const AIPerplex&) = delete;
	AIPerplex(AIPerplex&&) = delete;
	AIPerplex& operator=(AIPerplex&&) = delete;

  private:
	// TUNABLE SEARCH PARAMETERS
	// clang-format off
	// Hand-aligned: the values and their explanations line up in columns so the whole
	// tuning surface can be read at once. Wrapping to a column limit tears initializers
	// off their declarations and splits the explanations.
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
	// clang-format on

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
		    : best_move(Move::EmptyMove()), best_score(0), depth_completed(0), nodes_at_completed_depth(0),
		      last_iteration_move(Move::EmptyMove()), search_was_stable(true)
		{}
	};

	enum class IterationDecision {
		ACCEPT_AND_CONTINUE, // Use this depth, keep going
		ACCEPT_AND_STOP,     // Use this depth, stop iteration
		REJECT_AND_STOP      // Reject this depth, use previous
	};

	enum class RejectionReason { NONE, INCOMPLETE, TOO_FEW_NODES, SHORT_PV, SCORE_DROP, MOVE_CHANGED };

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

	// Lazy SMP helper thread entry point: plain iterative-deepening loop with
	// no quality gates (no assess_iteration_quality, no emergency handling,
	// no game-state/root propagation, no logging). Result is discarded —
	// the helper's only contribution is the TT entries it writes along the
	// way and its node count (aggregated by GetMove() after join). Exits on
	// IsAborted() or when max_depth is reached.
	void helper_loop(ThreadData& td, int max_depth, TranspositionTable& tt);

	// HELPER METHODS
	// --------------
	// Quality assessment
	RejectionReason assess_iteration_quality(const IterationMetrics& metrics, const SearchState& state) const;
	bool should_stop_early(int depth, int score, int pv_length) const;    // Early termination checks
	bool handle_empty_move_emergency(ThreadData& td, SearchState& state); // Emergency handling
	bool should_try_null_move(const ThreadData& td, int depth, int beta, int ply, bool is_pv_node, bool in_check) const;

	// Logging helpers
	void log_iteration_eval(const IterationMetrics& metrics, const PVTable& pv_table) const;
	void log_rejection(int depth, RejectionReason reason, const IterationMetrics& metrics,
	                   const SearchState& state) const;
	void log_acceptance(const IterationMetrics& metrics) const;
	void log_search_complete(const AIPerplex::SearchState& state, const PVTable& pv_table) const;
	void log_completed_iteration(const AIPerplex::IterationMetrics& metrics, const PVTable& pv_table) const;
	void log_aspiration_retry(int depth, int retry, int score, int alpha, int beta, bool fail_low) const;
	void log_aspiration_full_window(int depth, int max_retries) const;

	// Builds an IterationInfo snapshot (copying the PV out of td before the next
	// iteration mutates it) and forwards it to iteration_observer_. No-op when no
	// observer is registered. Called from both accept branches of
	// iterative_deepening(), after `state` is updated for that iteration.
	void emit_iteration_info(const ThreadData& td, int depth, int score) const;

	// MEMBER VARIABLES
	std::unique_ptr<TranspositionTable> _tt; // persistent transposition table

	// Per-thread search state (board copy, node counter, PV, killers, history, ...).
	// Persistent member — history is aged between moves, never cleared — and the
	// single instance used by the (currently single-threaded) search. Lazy SMP
	// helper threads will each get their own. See ThreadData.h.
	ThreadData td_;

	// Lazy SMP helper threads' per-thread state, one per helper (threads_ - 1
	// entries). Sized lazily on first use in GetMove() and never shrunk, so
	// history/killers age across moves per helper the same way td_'s does.
	// Empty and untouched whenever threads_ == 1.
	std::vector<std::unique_ptr<ThreadData>> helper_tds_;

	// Configured number of search threads (Lazy SMP). Clamped to [1, 32] by
	// SetThreads(). threads_ == 1 (the default) takes the exact pre-SMP code
	// path in GetMove() — no helper_tds_ construction, no thread spawn.
	unsigned threads_{1};

	// logging control: enable detailed logging when needed (default: false)
	static inline bool s_verbose_logging = false;

	// Per-iteration UCI diagnostic hook (see SetIterationObserver). Default-constructed
	// empty: emit_iteration_info() checks this before doing any work, so an unregistered
	// observer costs a single bool check per accepted iteration.
	std::function<void(const IterationInfo&)> iteration_observer_;

#ifdef STRAT_ENABLE_TEST_ACCESS
	// Enable fine-grained unit tests for private search helpers.
	// Activated by defining STRAT_ENABLE_TEST_ACCESS in the test project
	// preprocessor settings (StratChessTests.vcxproj) — never in production.
	// See Docs/TestDesign.md §"AIPerplex Test Access" and §Phase 1 [search] tests.
	friend class AIPerlexTestFixture;
	// Grants UCIHandler's test fixture (StratChessTests/UCITests.cpp) access
	// to threads_ so the "Threads survives ucinewgame" regression test can
	// verify the fix end to end, not just via UciHandler's own private state.
	friend class UciHandlerTestFixture;
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
