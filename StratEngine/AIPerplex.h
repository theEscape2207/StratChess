#pragma once
#include "defines.h"
#include "Eval.h"
#include "Move.h"
#include "TranspositionTable.h"
#include "PVTable.h"
#include "ThreadData.h"
#include "SearchResult.h"
#include "SearchControl.h"
#include "SearchTuningSchema.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
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
	int hashfull = 0;
	std::chrono::milliseconds elapsed{0};
	std::vector<Move> pv;
};

using IterationObserver = std::function<void(const IterationInfo&)>;
// Receives the finished search on the launch thread, after IsSearching() has turned false. It must
// not call StartAsync, Wait, StopAndWait, SetHash, SetThreads, SetTuning or StartNewGame, or destroy the service
// (Debug-asserted): a join from the launch thread throws, and the rest race the controlling thread.
using CompletionHandler = std::function<void(const SearchResult&)>;

inline constexpr unsigned DEFAULT_AIPERPLEX_HASH_MB = 192;

// Late move pruning, deliberately not tunable: parent depth exactly two, and the zero-based legal
// move index at which quiet moves become skippable (the thirteenth legal move).
inline constexpr int kLateMovePruningDepth = 2;
inline constexpr int kLateMovePruningMinLegalIndex = 12;

struct AIPerplexConfig {
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

	// Configuration/lifecycle methods SetThreads(), SetHash(), SetTuning() and
	// StartNewGame() must not overlap Search(). Stop() is the only method that
	// may be called concurrently with Search().
	// Configure the number of Lazy SMP search threads; clamps to [1, 32].
	// Search() spawns threads_ - 1 helper std::jthreads sharing the
	// transposition table with the main search.
	void SetThreads(unsigned n) noexcept
	{
		assert_not_in_completion_handler();
		threads_ = std::clamp(n, 1u, 32u);
	}
	// MAX_HASH_MB = 1536 is a deliberate policy cap, not an exact fit: 64-byte buckets make the
	// exact fits powers of two, so 1536 rounds down to 2^24 buckets and allocates 1024 MiB.
	// Steady-state total is about 1152 MiB on Windows or 1920 MiB on Linux including locks;
	// construct-before-replace briefly holds old and new tables, roughly doubling the peak.
	static constexpr unsigned DEFAULT_HASH_MB = DEFAULT_AIPERPLEX_HASH_MB;
	static constexpr unsigned MIN_HASH_MB = 1;
	static constexpr unsigned MAX_HASH_MB = 1536;

	HashConfigurationResult SetHash(unsigned mb) noexcept;
	// Validates, then replaces the tuning; an invalid tuning leaves everything unchanged. A changed
	// tuning clears the transposition table, whose scores the old pruning produced. StartNewGame()
	// keeps the tuning.
	std::optional<SearchTuningSchema::TuningError> SetTuning(const SearchTuning& tuning);
	const SearchTuning& Tuning() const noexcept { return tuning_; }
	void StartNewGame();
	void Stop() noexcept;

	// StartAsync, StopAndWait and Wait come from one controlling thread; Stop() and IsSearching()
	// from any.
	// Runs Search() on a launch thread owned by this service and returns at once. Stops and joins
	// any previous launch first; the root is copied, so the caller's board may change afterwards.
	// A Stop() made after this returns stops the launched search, even before it initialises.
	void StartAsync(const Board& root, const SearchLimits& limits, IterationObserver observer,
	                CompletionHandler on_done);
	void StopAndWait(); // Stop(), then join the launch thread
	void Wait();        // join the launch thread without stopping it
	// True from StartAsync() (or a direct Search()) until Search() returns; false before on_done.
	bool IsSearching() const noexcept;
	~AIPerplex();

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
	SearchResult iterative_deepening(ThreadData& td, int max_depth, TranspositionTable& tt, uint8_t search_start_age,
	                                 const IterationObserver& observer = {});
	int search_with_aspiration(ThreadData& td, int depth, int seed_score, TranspositionTable& tt);
	int pvs(ThreadData& td, int depth, int alpha, int beta, int ply, bool is_pv_node, TranspositionTable& tt);
	int adjustScoreForGameState(ThreadData& td, bool moveFound, int ply, int best_value);

	// The score of a draw this search DETECTED — repetition, the fifty-move rule, stalemate — in
	// the negamax perspective of the node reporting it. Never the value an aborted or
	// time-limited frame unwinds with: those are fabricated, and stay at GameValues::Draw.
	//
	// The sign comes from the board's side to move against the root colour, not from ply parity.
	// A null move flips the side to move while incrementing ply, so below one, parity no longer
	// tracks who is on move and the offset would invert for the whole subtree.
	int draw_score(const ThreadData& td) const noexcept;
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
	// *entries*, while the budget is compared against nodes_searched plus qnodes_searched
	// (one per legal move edge searched, in each tree). So the stop lands at the first poll
	// at or past the budget, not at the first multiple of 1024 of the budget's own counter.
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
	// The zugzwang floor null-move pruning and reverse futility share: below two non-pawn pieces,
	// "the side to move is not obliged to worsen its position" stops being true, and both
	// heuristics rest on it. pvs() establishes it once per node and hands it to both guards.
	static bool has_two_non_pawn_pieces(const Board& board);
	bool should_try_null_move(const ThreadData& td, int depth, int beta, int ply, bool is_pv_node, bool in_check,
	                          bool zugzwang_safe) const;
	// Every reverse-futility guard except the static evaluation itself, so pvs() only pays for
	// that evaluation on a node a cutoff could actually apply to.
	bool reverse_futility_eligible(int depth, int beta, bool is_pv_node, bool in_check, bool is_exclusion_frame,
	                               bool zugzwang_safe) const;
	// The node-level frontier-futility guards. The move-level ones live in the pvs() move loop,
	// where the move, the live killers and the made move's check status are at hand.
	bool frontier_futility_eligible(int depth, int alpha, bool is_pv_node, bool in_check,
	                                bool is_exclusion_frame) const;
	// The node-level late-move-pruning guards, runtime flag included. The legal-index and move-level
	// guards live in the move loop.
	bool late_move_pruning_eligible(int depth, int alpha, int beta, bool is_pv_node, bool in_check,
	                                bool is_exclusion_frame) const;

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
	void emit_iteration_info(const ThreadData& td, int depth, int score, uint8_t search_start_age,
	                         const IterationObserver& observer) const;
	// The immediate go/stop handshake. StartAsync() arms it before the launch thread exists, so a
	// Stop() arriving before Search() initialises is kept rather than reset.
	void arm_search_launch() noexcept;
	void finish_search_launch() noexcept;

	// Set on the launch thread while on_done runs; the entry points on_done must not call assert it
	// is clear. Per thread, not per service, so it also flags a call into any other service.
	static inline thread_local bool in_completion_handler_ = false;
	static void assert_not_in_completion_handler() noexcept { assert(!in_completion_handler_); }

	// MEMBER VARIABLES
	std::unique_ptr<TranspositionTable> _tt; // persistent transposition table
	Evaluator evaluator_;                    // stateless, safe to share unsynchronized across threads
	SearchControl control_;                  // owned limits, timer and abort latch
	SearchTuning tuning_;
	mutable std::mutex stop_mutex_;
	bool search_launch_active_{false};
	bool stop_pending_{false};
	uint64_t game_generation_{0};

	// The side to move at the root of the CURRENT search. Written in Search() before any helper
	// thread starts and read-only for the rest of it, the same discipline tuning_ follows.
	// Defaulted rather than left indeterminate because pvs() is reachable without Search() in the
	// test build; a test that cares sets it explicitly.
	eColor root_color_{WHITE};

	// The (root colour, contempt) pair the transposition table's contents were produced under.
	// A contempt-derived draw score propagates into parent entries, so a stored bound depends on
	// which side the search was favouring and by how much — context the Zobrist key does not
	// carry. Search() clears the table when the incoming pair differs. Empty until the first
	// search of a game; StartNewGame() empties it again along with the table itself.
	//
	// Unreachable in normal play, where a UCI engine only ever searches its own moves and the
	// root colour is constant for a whole game. It is analysis, the tactical runner and tests —
	// one service searching both colours, or under a changed contempt — that can hit it.
	std::optional<std::pair<eColor, int>> tt_contempt_context_;

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

	// Declared after td_ and helper_tds_ so td_ stays ahead of the cold members and the launch thread
	// is destroyed before the state it searches; the destructor still joins it explicitly.
	std::jthread launch_thread_;
#ifdef STRAT_ENABLE_TEST_ACCESS
	// Called on the launch thread before Search(), so a test can deliver Stop() before the search
	// initialises instead of relying on scheduling.
	std::function<void()> launch_barrier_;
#endif

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
};
