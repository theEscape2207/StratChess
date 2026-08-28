#pragma once
#include "defines.h"
#include "Eval.h"
#include "Move.h"
#include "TranspositionTable.h"
#include "PVTable.h"
#include "ThreadData.h"
#include "SearchResult.h"
#include "SearchControl.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// Snapshot of one accepted iterative-deepening iteration, handed to the
// per-call iteration observer supplied to AIPerplex::Search(). `nodes` is the
// CUMULATIVE main-search-thread node count at the end of this accepted
// iteration, both trees summed (td.nodes_searched + td.qnodes_searched) — the
// standard UCI convention for a per-iteration "nodes so far" figure, not the
// per-iteration delta IterationMetrics tracks. It is NOT guaranteed to equal the final
// info/bestmove line's node count: on a clocked search the loop typically
// starts one more iteration, gets interrupted, and has that iteration
// rejected by assess_iteration_quality() (REJECT_AND_STOP emits nothing —
// see iterative_deepening()), but the rejected iteration's nodes are already
// in both counters by the time Search() reports the final total, so
// that total is typically strictly greater than this field at Threads=1.
// Under Lazy SMP the two also diverge because the final total sums helper
// threads' nodes, which are never visible here. `pv` is a copy of the PV
// table's root line, taken at emit time before the next iteration
// overwrites it. `elapsed` shares SearchResult's SearchControl origin, so a
// caller can report iteration and final times on one monotonic timeline.
struct IterationInfo {
	int depth = 0;
	int score = 0;
	int64_t nodes = 0;
	std::chrono::milliseconds elapsed{0};
	std::vector<Move> pv;
};

using IterationObserver = std::function<void(const IterationInfo&)>;

inline constexpr unsigned DEFAULT_AIPERPLEX_HASH_MB = 192;

// Hand-aligned: this is the one tuning surface shared by the concrete
// service configuration and the search implementation.
struct SearchTuning {
	int64_t min_nodes_threshold = 1000;
	double min_completion_ratio = 0.10;
	double min_pv_ratio = 0.33;
	int score_draw_threshold = 20;

	int delta_pruning_margin = 200;

	int aspiration_initial_delta = 50;
	int aspiration_max_retries = 4;
	bool aspiration_enabled = true;

	int lmr_min_depth = 3;
	int lmr_min_move_index = 3;
	bool lmr_enabled = true;

	bool null_move_enabled = true;
	int null_move_reduction = 3;
	int null_move_min_depth = 3;

	// Gates SEE pruning in quiescence only, never the SEE capture tiers in ScoreMoves, and turning
	// it off must leave the search node-identical. The !in_check guard at the pruning site is
	// correctness, not tuning, and is deliberately outside this flag.
	bool see_pruning_enabled = true;
};

struct AIPerplexConfig {
	EvalManager::EvalTypes evaluator{EvalManager::EvalTypes::COMPLEX};
	unsigned default_depth{4};
	std::chrono::milliseconds default_time{15000};
	unsigned hash_mb{DEFAULT_AIPERPLEX_HASH_MB};
	unsigned threads{1};
	SearchTuning tuning{};
	bool verbose_logging{false};
};

class AIPerplex final {
  public:
	struct HashConfigurationResult {
		bool success{false};
		unsigned requested_mb{0};
		size_t entry_mb{0};
		size_t bucket_count{0};
	};

	// Concrete search-service constructor. Search roots are supplied per call.
	explicit AIPerplex(AIPerplexConfig config = {});
	SearchResult Search(const Board& root, const SearchLimits& limits, IterationObserver observer = {});

	// Configuration/lifecycle methods SetThreads(), SetHash(), and
	// StartNewGame() must not overlap Search(). Stop() is the only method that
	// may be called concurrently with Search().
	// Configure the number of Lazy SMP search threads; clamps to [1, 32].
	// Search() spawns threads_ - 1 helper std::jthreads sharing the
	// transposition table with the main search.
	void SetThreads(unsigned n) noexcept { threads_ = std::clamp(n, 1u, 32u); }
	// MAX_HASH_MB = 1536 is a deliberate policy cap, not the largest exact fit.
	// Steady-state total is about 1664 MiB on Windows or 2432 MiB on Linux including locks;
	// construct-before-replace briefly holds old and new tables, roughly doubling the peak.
	static constexpr unsigned DEFAULT_HASH_MB = DEFAULT_AIPERPLEX_HASH_MB;
	static constexpr unsigned MIN_HASH_MB = 1;
	static constexpr unsigned MAX_HASH_MB = 1536;

	HashConfigurationResult SetHash(unsigned mb) noexcept;
	void StartNewGame();
	void Stop() noexcept;
	~AIPerplex() = default;

	// Force use of factory by preventing constructor, copy-construction & operator=
	AIPerplex(const AIPerplex&) = delete;
	AIPerplex& operator=(const AIPerplex&) = delete;
	AIPerplex(AIPerplex&&) = delete;
	AIPerplex& operator=(AIPerplex&&) = delete;

  private:
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
	void init_search(const Board& root);
	SearchResult iterative_deepening(ThreadData& td, int max_depth, TranspositionTable& tt,
	                                 const IterationObserver& observer = {});
	int search_with_aspiration(ThreadData& td, int depth, int seed_score, TranspositionTable& tt);
	int pvs(ThreadData& td, int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt);
	int adjustScoreForGameState(ThreadData& td, bool moveFound, int ply, int best_value);
	// Budget a node entering quiescence from pvs() starts with. quiescence() spends it
	// downwards and stops when it goes negative, so 16 ply levels run out of check; the value
	// it carries is always search still to come — the same unit pvs() uses for depth and both
	// phases write to the transposition table.
	static constexpr int QSEARCH_BUDGET = 15;

	// qsearch_budget is the quiescence plies still to come, counted down towards zero, so it
	// carries the same "remaining search" unit as pvs()'s depth and the TT entries both store.
	int quiescence(ThreadData& td, int alpha, int beta, int qsearch_budget, int ply, TranspositionTable& tt);

	// Orders a quiescence node's moves in place. The two phases order on different criteria and
	// keep their scratch buffers off the caller's frame — see the definition.
	void order_quiescence_moves(ThreadData& td, MoveList& moveList, bool in_check, int ply) const;

	// The per-node limit poll shared by pvs() and quiescence(): true means this search must
	// stop now. Only thread 0 polls, and only every 1024 node entries, so the chrono::now()
	// behind the clock check is amortised; a helper thread returns false without even
	// touching the counter. Whichever limit fires latches the abort flag, after which the
	// IsAborted() fast path at the top of both functions answers for free.
	//
	// The two counters are in different units, which matters when reasoning about how far
	// past the budget a node-limited search can run: nodes_since_check_ counts node
	// *entries*, while the budget is compared against nodes_searched (one per move edge
	// considered, including edges DoMove rejects) plus qnodes_searched (one per legal
	// quiescence edge). So the stop lands at the first poll at or past the budget, not at
	// the first multiple of 1024 of the budget's own counter.
	bool poll_search_limits(ThreadData& td);
	// Lazy SMP helper thread entry point: plain iterative-deepening loop with
	// no quality gates (no assess_iteration_quality, no emergency handling,
	// no game-state/root propagation, no logging). Result is discarded —
	// the helper's only contribution is the TT entries it writes along the
	// way and its node count (aggregated by Search() after join). Exits on
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
	// iteration mutates it) and forwards it to the current call's observer. No-op
	// when no observer was supplied. Called from both accept branches of
	// iterative_deepening(), after `state` is updated for that iteration.
	void emit_iteration_info(const ThreadData& td, int depth, int score, const IterationObserver& observer) const;
	// UCI-only half of the immediate go/stop launch handshake. Kept private so
	// ordinary Search callers have one synchronous operation and no pre-call
	// ordering contract.
	void arm_uci_search_launch() noexcept;
	void finish_search_launch() noexcept;

	// MEMBER VARIABLES
	std::unique_ptr<TranspositionTable> _tt; // persistent transposition table
	std::unique_ptr<EvalManager> evaluator_; // owned concrete-search evaluator
	SearchControl control_;                  // owned limits, timer and abort latch
	SearchTuning tuning_;
	std::mutex stop_mutex_;
	bool search_launch_active_{false};
	bool stop_pending_{false};
	uint64_t game_generation_{0};

	// Per-thread search state (board copy, node counter, PV, killers, history, ...).
	// Persistent member — history is aged between moves, never cleared — and the
	// single instance used by the (currently single-threaded) search. Lazy SMP
	// helper threads will each get their own. See ThreadData.h.
	ThreadData td_;

	// Lazy SMP helper threads' per-thread state, one per helper (threads_ - 1
	// entries). Sized lazily on first use in Search() and never shrunk, so
	// history/killers age across moves per helper the same way td_'s does.
	// Empty and untouched whenever threads_ == 1.
	std::vector<std::unique_ptr<ThreadData>> helper_tds_;

	// Configured number of search threads (Lazy SMP). Clamped to [1, 32] by
	// SetThreads(). threads_ == 1 (the default) takes the exact pre-SMP code
	// path in Search() — no helper_tds_ construction, no thread spawn.
	unsigned threads_{1};

	// Per-service logging policy. The shared logger is only a sink; every
	// AIPerplex instance decides independently whether to emit diagnostics.
	bool verbose_logging_{false};

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
	friend class UciHandler;
};
