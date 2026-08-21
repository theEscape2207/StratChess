// SearchTests.cpp — Catch2 [search] tests for AIPerplex private helper methods.
//
// Tests for:
//   assess_iteration_quality() — 6 cases, one per RejectionReason branch
//   should_stop_early()        — 2 cases (mate score, forced-line short-circuit)
//   handle_empty_move_emergency() — 2 cases (mate path, true-emergency path)
//   should_try_null_move()     — 10 cases, one per guard branch (disabled, PV,
//                                 in-check, depth, mate-score, zugzwang,
//                                 single-piece zugzwang, two-piece eligible,
//                                 consecutive-null, otherwise-eligible)
//
// Requires STRAT_ENABLE_TEST_ACCESS in the test project preprocessor definitions.
// See Docs/TestDesign.md §"AIPerplex Test Access" for the mechanism.

#include <catch_amalgamated.hpp>
#include "AIAgent.h"
#include "AIPerplex.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include "PlayerBase.h"
#include "PVIntegrity.h"
#include "PVTable.h"
#include "MoveHelper.h"
#include "TranspositionTable.h"
#include "defines.h"
#include <chrono>
#include <atomic>
#include <initializer_list>
#include <optional>
#include <thread>

TEST_CASE("AIPerplex Search uses the board supplied for each call and does not retain observers", "[search][service_api]")
{
	Board first_board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	Board second_board("6k1/5ppp/8/8/8/4R3/5PPP/6K1 w - - 0 1");
	AIPerplex ai(AIPerplexConfig{.default_depth = 2, .verbose_logging = false});

	int first_observations = 0;
	const auto first = ai.Search(first_board, SearchLimits::fixed_depth(2),
	                             [&](const IterationInfo&) { ++first_observations; });
	const int first_observations_after_first_search = first_observations;
	int second_observations = 0;
	const auto second = ai.Search(second_board, SearchLimits::fixed_depth(2),
	                              [&](const IterationInfo&) { ++second_observations; });
	const int second_observations_after_second_search = second_observations;
	CHECK(first_observations == first_observations_after_first_search);
	const auto third = ai.Search(first_board, SearchLimits::fixed_depth(2));

	CHECK_FALSE(first.best_move.is_null());
	CHECK_FALSE(second.best_move.is_null());
	CHECK(first_board.IsLegalMove(first.best_move));
	CHECK(second_board.IsLegalMove(second.best_move));
	CHECK(second.best_move.from() == e3);
	CHECK(second.best_move.to() == e8);
	CHECK(first_observations > 0);
	CHECK(second_observations > 0);
	CHECK(first_observations == first_observations_after_first_search);
	CHECK(second_observations == second_observations_after_second_search);
	CHECK_FALSE(third.best_move.is_null());
}

TEST_CASE("AIPerplexConfig selects the evaluator used by Search", "[search][service_api]")
{
	Board board("4k3/pp6/8/8/8/P7/P7/4K3 w - - 0 1");
	AIPerplex simple(AIPerplexConfig{.evaluator = EvalManager::EvalTypes::SIMPLE, .default_depth = 1});
	AIPerplex complex(AIPerplexConfig{.evaluator = EvalManager::EvalTypes::COMPLEX, .default_depth = 1});

	const SearchResult simple_result = simple.Search(board, SearchLimits::fixed_depth(1));
	const SearchResult complex_result = complex.Search(board, SearchLimits::fixed_depth(1));

	CHECK_FALSE(simple_result.best_move.is_null());
	CHECK_FALSE(complex_result.best_move.is_null());
	CHECK(simple_result.best_score != complex_result.best_score);
}

// ============================================================================
// Helper
// ============================================================================
// Returns any legal move from the starting position.
// Used to produce a guaranteed non-null Move for assess tests, and (below)
// by the fixture's own per-game-state pokes.
static Move AnyLegalMove()
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	MoveList ml;
	MoveGenerator::ComputeLegalMoves(board, ml);
	REQUIRE(!ml.empty());
	return ml[0];
}

// ============================================================================
// Test fixture
// ============================================================================
// Must be defined here (not in a header) — the name must match the friend
// declaration inside AIPerplex.h: friend class AIPerlexTestFixture;
//
// Public type aliases re-export the private AIPerplex nested types so that
// TEST_CASE functions outside the class can write e.g.
//   AIPerlexTestFixture::RejectionReason::INCOMPLETE
class AIPerlexTestFixture {
  public:
	static constexpr uint64_t TT_MARKER_KEY = 0x7fff'ffff'ffff'ffffULL;

	// The budget pvs() hands a fresh quiescence node. Re-exported because AIPerplex keeps it
	// private and the TEST_CASE functions below are not friends.
	static constexpr int QSEARCH_BUDGET = AIPerplex::QSEARCH_BUDGET;

	// Re-export private types for test use
	using RejectionReason = AIPerplex::RejectionReason;
	using Metrics = AIPerplex::IterationMetrics;
	using State = AIPerplex::SearchState;

	// Must be declared (and thus constructed/destroyed) before ai_owner —
	// ai_owner holds a Board& reference into it that must outlive it.
	Board board_;
	std::unique_ptr<PlayerBase> ai_owner;
	AIPerplex* ai = nullptr;

	explicit AIPerlexTestFixture(const std::string& fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	                             unsigned max_depth = 4)
	    : board_(fen)
	{
		// max_depth sets max_depth_ (the IDS hard cap used if GetMove() is ever called).
		// Defaults to 4, a don't-care for the many [search] tests that never call
		// GetMove(); the node-limit tests below raise it so the node poll — not the
		// depth cap — is what stops the search.
		ai_owner = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, max_depth, board_);
		ai = static_cast<AIPerplex*>(ai_owner.get());
		AIPerplex::SetVerboseLogging(false);
		// Note: SetEvalEngine() is NOT called — the helper methods under test
		// do not invoke Eval->Evaluate(), so this is safe.
	}

	RejectionReason assess(const Metrics& m, const State& s) const { return ai->assess_iteration_quality(m, s); }

	bool stop_early(int depth, int score, int pv_len) const { return ai->should_stop_early(depth, score, pv_len); }

	// The PV written by the emergency path now lives in ai->td_.pv_table.
	bool emergency(State& s) const { return ai->handle_empty_move_emergency(ai->td_, s); }

	bool try_null_move(int depth, int beta, int ply, bool is_pv_node, bool in_check) const
	{
		return ai->should_try_null_move(ai->td_, depth, beta, ply, is_pv_node, in_check);
	}

	// Pokes the consecutive-null-move guard array inside the private td_
	// member. Needed because td_ is private on AIPerplex — only
	// AIPerlexTestFixture (the declared friend) can reach it, not the free
	// TEST_CASE functions.
	void set_last_move_was_null(int ply, bool value) const { ai->td_.last_move_was_null[ply] = value; }

	// Reads the private threads_ member — set (clamped) via the public
	// SetThreads() override; needs friend access because threads_ itself
	// is private. Used by the [smp] clamp tests below.
	unsigned threads() const { return ai->threads_; }

	void store_tt_marker() const
	{
		ai->_tt->store(TT_MARKER_KEY, 123, 1, 0, Move::EmptyMove(), BoundType::EXACT, NodeType::PV_NODE,
		               SearchPhase::MAIN);
	}

	bool has_tt_marker() const { return ai->_tt->probe(TT_MARKER_KEY, 0).has_value(); }

	void start_new_game() const { ai->StartNewGame(); }

	// --- root_game_state pokes, for proving init_search() resets the per-call carrier ---

	void set_root_game_state(GameStates s) const { ai->td_.root_game_state = s; }
	GameStates root_game_state() const { return ai->td_.root_game_state; }
	void call_init_search() const { ai->init_search(); }

	// --- Per-game state pokes, for proving StartNewGame() resets them ---

	void poke_history() const { ai->td_.update_history(WHITE, AnyLegalMove(), 4); }
	bool history_is_clear() const
	{
		for (const auto& side : ai->td_.history)
			for (const auto& from : side)
				for (int32_t score : from)
					if (score != 0)
						return false;
		return true;
	}

	void poke_killer(int ply) const { ai->td_.store_killer(ply, AnyLegalMove()); }
	bool has_killer(int ply) const { return !ai->td_.killers[ply][0].is_null(); }

	// --- PV table pokes ---
	// A row seeded here stands in for what a real search leaves behind: row 0 for a completed
	// aspiration retry, row 1 for whatever subtree last reached ply 1. Both are the input the
	// abort and emergency paths have to refuse to publish.
	void seed_pv_row(int ply, const Move& move) const { ai->td_.pv_table.update(ply, move); }
	int pv_length(int ply) const { return ai->td_.pv_table.get_length(ply); }
	Move pv_move(int ply) const { return ai->td_.pv_table.get_pv_move(ply); }
	std::span<const Move> pv_line(int ply) const
	{
		return {ai->td_.pv_table.get_line(ply).data(), static_cast<size_t>(ai->td_.pv_table.get_length(ply))};
	}

	void add_fake_helper() const { ai->helper_tds_.push_back(std::make_unique<ThreadData>()); }
	size_t helper_count() const { return ai->helper_tds_.size(); }

	// The helper threads' own counters, for checking GetMove()'s post-join aggregation exactly
	// rather than by inequality — a helper that never got scheduled contributes a legitimate 0.
	int64_t helper_nodes() const
	{
		int64_t total = 0;
		for (const auto& htd : ai->helper_tds_)
			total += htd->nodes_searched;
		return total;
	}

	int64_t helper_qnodes() const
	{
		int64_t total = 0;
		for (const auto& htd : ai->helper_tds_)
			total += htd->qnodes_searched;
		return total;
	}

	void set_last_result_depth(int depth) const { ai->last_result_.depth_completed = depth; }

	void search_depth_one()
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		REQUIRE(board_.fullmove_count() == 1);
		const Move move = ai->GetMove(SearchLimits::fixed_depth(1)).best_move;
		REQUIRE_FALSE(move.is_null());
	}

	// One complete GetMove() at a chosen thread count — the only way to observe the aggregation
	// GetMove() performs after joining its helpers.
	SearchResult get_move_at_threads(unsigned threads, int depth) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		ai->SetThreads(threads);
		return ai->GetMove(SearchLimits::fixed_depth(depth));
	}

	// --- Terminal-node helpers ---

	// Counts the moves that survive DoMove(). ComputeLegalMoves() is pseudo-legal at
	// the edges and pvs() relies on DoMove() to reject the rest, so this is the same
	// notion of "has a move" the search uses. Zero makes the position terminal.
	int count_legal_moves() const
	{
		Board copy = board_;
		MoveList ml;
		MoveGenerator::ComputeLegalMoves(copy, ml);

		int legal = 0;
		for (const auto& move : ml) {
			if (copy.DoMove(move)) {
				++legal;
				copy.UndoMove(move);
			}
		}
		return legal;
	}

	// Runs one pvs() node on the fixture's board at an arbitrary ply, with the
	// thread-local state a real search would have set up. Lets the terminal-node
	// tests place a mate/stalemate node at a chosen ply without building a tree.
	int search_node(int depth, int ply, int alpha = -GameValues::Search_Init, int beta = GameValues::Search_Init,
	                bool is_pv_node = true) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		ai->td_.board = board_;
		return ai->pvs(ai->td_, depth, alpha, beta, ply, is_pv_node, *ai->_tt);
	}

	std::optional<TTEntry> probe_tt(int ply) const { return ai->_tt->probe(board_.get_zobrist_hash(), ply); }

	std::optional<TTEntry> probe_tt(uint64_t key, int ply) const { return ai->_tt->probe(key, ply); }

	// Runs one quiescence() node on the fixture's board. The timer is armed because
	// quiescence polls the wall clock every 1024 nodes and a default-constructed
	// TimeManager has its start_time_ at the epoch, which would latch an abort.
	int quiesce_node(int alpha, int beta, int qsearch_budget, int ply) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		ai->ApplyLimits(SearchLimits::fixed_time(std::chrono::milliseconds(60'000)));
		ai->td_.board = board_;
		ai->td_.nodes_since_check_ = 0;
		return ai->quiescence(ai->td_, alpha, beta, qsearch_budget, ply, *ai->_tt);
	}

	// Enters quiescence the way the search does — through pvs() with no depth left — so the
	// budget under test is the one pvs() hands out, not one the test chose. Passing the budget
	// in directly would assert nothing about the unit it is expressed in.
	int quiesce_via_pvs(int alpha, int beta, int ply) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		ai->ApplyLimits(SearchLimits::fixed_time(std::chrono::milliseconds(60'000)));
		ai->td_.board = board_;
		ai->td_.nodes_since_check_ = 0;
		return ai->pvs(ai->td_, /*depth=*/0, alpha, beta, ply, /*is_pv_node=*/true, *ai->_tt);
	}

	// Plants a quiescence entry for the fixture's board with a chosen remaining budget, so a
	// test can prove which budgets a later probe is willing to reuse.
	void store_qsearch_entry(int16_t value, int16_t qsearch_budget, int ply) const
	{
		ai->_tt->store(board_.get_zobrist_hash(), value, qsearch_budget, static_cast<int16_t>(ply), Move::EmptyMove(),
		               BoundType::EXACT, NodeType::PV_NODE, SearchPhase::QUIESCENCE);
	}

	// Replays a UCI move list onto td_.board, then runs one quiescence() node at the
	// resulting ply. Lets a test place the node inside a line, with real repetition history
	// behind it, rather than at a synthetic root.
	int quiesce_after(std::initializer_list<const char*> moves, int alpha, int beta) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		ai->ApplyLimits(SearchLimits::fixed_time(std::chrono::milliseconds(60'000)));
		ai->td_.board = board_;
		ai->td_.nodes_since_check_ = 0;

		int ply = 0;
		for (const char* uci : moves) {
			const Move move = MoveFormatter::FromUCI(uci, ai->td_.board);
			REQUIRE(ai->td_.board.DoMove(move));
			++ply;
		}
		return ai->quiescence(ai->td_, alpha, beta, AIPerplex::QSEARCH_BUDGET, ply, *ai->_tt);
	}

	SearchResult last_result() const { return ai->GetLastResult(); }

	int64_t qnodes() const { return ai->td_.qnodes_searched; }

	int64_t mainnodes() const { return ai->td_.nodes_searched; }

	// Plants a value the search itself could never produce, so a test can prove
	// init_search() replaced the counters rather than added to them.
	void poison_node_counters(int64_t value) const
	{
		ai->td_.nodes_searched = value;
		ai->td_.qnodes_searched = value;
	}

	// Static evaluation of the fixture's board — the value stand-pat would have used.
	int evaluate() const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		return ai->Eval->Evaluate(board_);
	}

	// Full fixed-depth search from the fixture's board. SetEvalEngine() is protected
	// on PlayerAiBase, so only the fixture (a declared friend) can arm the evaluator.
	Move search_to_depth(int depth) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		return ai->GetMove(SearchLimits::fixed_depth(depth)).best_move;
	}

	// Full search bounded by a node budget instead of a fixed depth. Requires the
	// fixture to be constructed with a max_depth high enough that the node poll,
	// not the IDS depth cap, is what stops the search.
	Move search_with_nodes(int64_t nodes) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		return ai->GetMove(SearchLimits::fixed_nodes(nodes)).best_move;
	}

	bool search_is_aborted() const { return ai->control_.IsAborted(); }

	// The same search, but handing back the whole result. The abort tests need game_state and
	// best_move together, and Threads=1 so the node poll is the only thing that stops it.
	SearchResult result_with_nodes(int64_t nodes) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		ai->SetThreads(1);
		return ai->GetMove(SearchLimits::fixed_nodes(nodes));
	}

	// Entries the poll gate counted for the last search: pvs() and quiescence() entries together,
	// which is not nodes_searched + qnodes_searched (those increment past several early returns).
	// iterative_deepening() zeroes it, so this is a per-search figure.
	int64_t poll_ticks() const { return ai->td_.nodes_since_check_; }

	static bool verbose_logging(const AIPerplex& ai) { return ai.verbose_logging_; }
};

TEST_CASE("AIPerplex compatibility stop does not abort the next direct Search", "[search][service_api]")
{
	Board board("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2");
	AIPerplex ai(AIPerplexConfig{.default_depth = 50, .verbose_logging = false});
	std::atomic<bool> accepted_iteration{false};
	SearchResult stopped_result;

	std::jthread search_thread([&] {
		stopped_result = ai.Search(board, SearchLimits::fixed_depth(50), [&](const IterationInfo&) {
			accepted_iteration.store(true, std::memory_order_release);
		});
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!accepted_iteration.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();

	PlayerAiBase& compatibility_surface = ai;
	compatibility_surface.StopSearch();
	search_thread.join();
	REQUIRE(accepted_iteration.load(std::memory_order_acquire));
	CHECK_FALSE(stopped_result.best_move.is_null());

	const SearchResult next = ai.Search(board, SearchLimits::fixed_depth(2));
	CHECK_FALSE(next.best_move.is_null());
	CHECK(next.depth_completed == 2);
}

TEST_CASE("AIPerplex verbosity configuration is isolated per engine", "[search][service_api]")
{
	AIPerplex quiet(AIPerplexConfig{.verbose_logging = false});
	REQUIRE_FALSE(AIPerlexTestFixture::verbose_logging(quiet));

	AIPerplex verbose(AIPerplexConfig{.verbose_logging = true});
	CHECK_FALSE(AIPerlexTestFixture::verbose_logging(quiet));
	CHECK(AIPerlexTestFixture::verbose_logging(verbose));
}

// ============================================================================
// Legacy-agent test fixture
// ============================================================================
// A minimal counterpart to AIPerlexTestFixture for the legacy (non-Lazy-SMP) agents, which
// have no ThreadData and carry root_game_state_ directly on PlayerAiBase. Must be defined
// here (not in a header) — the name must match the friend declaration inside PlayerAI.h:
// friend class LegacyAiTestFixture;
class LegacyAiTestFixture {
  public:
	// Must be declared (and thus constructed/destroyed) before ai_owner —
	// ai_owner holds a Board& reference into it that must outlive it.
	Board board_;
	std::unique_ptr<PlayerBase> ai_owner;
	AIAgent* ai = nullptr;

	explicit LegacyAiTestFixture(const std::string& fen, unsigned max_depth = 4) : board_(fen)
	{
		ai_owner = PlayerBase::Create(PlayerBase::ePlayerTypes::AIAGENT, max_depth, board_);
		ai = static_cast<AIAgent*>(ai_owner.get());
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
	}

	void set_root_game_state(GameStates s) const { ai->root_game_state_ = s; }
	GameStates root_game_state() const { return ai->root_game_state_; }

	// Calls the same reset point every legacy GetMove() calls before it does anything else.
	void call_apply_limits() const { ai->ApplyLimits(SearchLimits::fixed_depth(1)); }
	void stop_search() const { ai->StopSearch(); }
	bool search_is_aborted() const { return ai->IsAborted(); }

	SearchResult get_move(int depth) const { return ai->GetMove(SearchLimits::fixed_depth(depth)); }
	int64_t search_count() const { return static_cast<int64_t>(ai->m_SearchCount); }

	// A GetMove() whose budget is spent the moment it starts. The depth loop's StopRequested()
	// gate sits in front of Search() with no node counter in the way, so the loop breaks before
	// any root frame adjudicates -- the abort this agent can reach without racing a clock.
	SearchResult get_move_with_spent_clock() const
	{
		return ai->GetMove(SearchLimits::fixed_time(std::chrono::milliseconds(0)));
	}
};

// ============================================================================
// New-game lifecycle tests
// ============================================================================

TEST_CASE("Search - StartNewGame clears a populated AIPerplex TT", "[search][tt]")
{
	AIPerlexTestFixture fix;
	fix.store_tt_marker();
	REQUIRE(fix.has_tt_marker());

	fix.start_new_game();

	REQUIRE_FALSE(fix.has_tt_marker());
}

TEST_CASE("Search - fullmove-one position does not define TT lifetime", "[search][tt]")
{
	AIPerlexTestFixture fix;
	fix.store_tt_marker();

	fix.search_depth_one();

	REQUIRE(fix.has_tt_marker());
}

TEST_CASE("Search - StartNewGame resets td_ history and killers", "[search]")
{
	AIPerlexTestFixture fix;
	fix.poke_history();
	fix.poke_killer(0);
	REQUIRE_FALSE(fix.history_is_clear());
	REQUIRE(fix.has_killer(0));

	fix.start_new_game();

	REQUIRE(fix.history_is_clear());
	REQUIRE_FALSE(fix.has_killer(0));
}

TEST_CASE("Search - StartNewGame clears helper_tds_", "[search][smp]")
{
	// Lazy SMP helpers are reused across searches within a game (GetMove()
	// only grows helper_tds_, never shrinks it) — StartNewGame() must clear
	// the vector so the next search reconstructs them fresh instead of
	// carrying killers/history over from the previous game.
	AIPerlexTestFixture fix;
	fix.add_fake_helper();
	REQUIRE(fix.helper_count() == 1);

	fix.start_new_game();

	REQUIRE(fix.helper_count() == 0);
}

TEST_CASE("Search - StartNewGame does not reset tuning_", "[search]")
{
	// Regression: Game::SetPlayerParams() applies game_settings.json's
	// search_tuning overrides and then unconditionally calls StartNewGame()
	// on the same object -- if StartNewGame() reset tuning_, every configured
	// override would be silently discarded before the first move is searched.
	AIPerlexTestFixture fix;
	fix.ai->tuning().null_move_enabled = false;

	fix.start_new_game();

	REQUIRE(fix.ai->tuning().null_move_enabled == false);
}

TEST_CASE("Search - StartNewGame resets last_result_", "[search]")
{
	AIPerlexTestFixture fix;
	fix.set_last_result_depth(5);
	REQUIRE(fix.ai->GetLastResult().depth_completed == 5);

	fix.start_new_game();

	REQUIRE(fix.ai->GetLastResult().depth_completed == 0);
}

// ============================================================================
// assess_iteration_quality tests
// ============================================================================

TEST_CASE("Search - assess: null current_move yields INCOMPLETE", "[search]")
{
	AIPerlexTestFixture fix;

	AIPerlexTestFixture::Metrics m{};
	m.depth = 4;
	m.current_move = Move{}; // null — triggers CASE 1
	m.current_score = 100;
	m.nodes_searched = 5000;
	m.pv_length = 2;
	m.interrupted = true;
	m.move_changed = false;
	m.score_delta = 10;
	m.completion_ratio = 0.5;

	AIPerlexTestFixture::State s{};
	s.depth_completed = 0;
	s.best_score = 100;
	s.nodes_at_completed_depth = 0;

	REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::INCOMPLETE);
}

TEST_CASE("Search - assess: too few nodes yields INCOMPLETE", "[search]")
{
	AIPerlexTestFixture fix;
	const Move any = AnyLegalMove();

	AIPerlexTestFixture::Metrics m{};
	m.depth = 4;
	m.current_move = any;
	m.current_score = 100;
	m.nodes_searched = 10; // below min_nodes_threshold (default 1000) — CASE 1
	m.pv_length = 2;
	m.interrupted = true;
	m.move_changed = false;
	m.score_delta = 10;
	m.completion_ratio = 0.5;

	AIPerlexTestFixture::State s{};
	s.depth_completed = 0;
	s.best_score = 100;
	s.nodes_at_completed_depth = 0;

	REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::INCOMPLETE);
}

TEST_CASE("Search - assess: low completion ratio yields TOO_FEW_NODES", "[search]")
{
	AIPerlexTestFixture fix;
	const Move any = AnyLegalMove();

	// Pass CASE 1 (move ok, nodes ok) but fail CASE 2 (completion ratio)
	AIPerlexTestFixture::Metrics m{};
	m.depth = 4;
	m.current_move = any;
	m.current_score = 100;
	m.nodes_searched = 5000;
	m.pv_length = 2;
	m.interrupted = true;
	m.move_changed = false;
	m.score_delta = 10;
	m.completion_ratio = 0.01; // below min_completion_ratio (default 0.10)

	AIPerlexTestFixture::State s{};
	s.depth_completed = 3; // > 0: previous depth exists
	s.best_score = 100;
	s.nodes_at_completed_depth = 5000; // > 0: denominator present

	REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::TOO_FEW_NODES);
}

TEST_CASE("Search - assess: pv too short yields SHORT_PV", "[search]")
{
	AIPerlexTestFixture fix;
	const Move any = AnyLegalMove();

	// depth=9, min_pv_ratio=0.33 → min required pv = max(1, int(9*0.33)) = max(1, 2) = 2
	// pv_length=1 < 2 → SHORT_PV
	AIPerlexTestFixture::Metrics m{};
	m.depth = 9;
	m.current_move = any;
	m.current_score = 100;
	m.nodes_searched = 5000;
	m.pv_length = 1; // too short (< 2)
	m.interrupted = true;
	m.move_changed = false;
	m.score_delta = 10;
	m.completion_ratio = 0.5; // passes CASE 2

	AIPerlexTestFixture::State s{};
	s.depth_completed = 8;
	s.best_score = 100;
	s.nodes_at_completed_depth = 5000;

	REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::SHORT_PV);
}

TEST_CASE("Search - assess: score drops to 0 from large value yields SCORE_DROP", "[search]")
{
	AIPerlexTestFixture fix;
	const Move any = AnyLegalMove();

	// current_score == 0, previous was 300 (abs > score_draw_threshold=20) → SCORE_DROP
	AIPerlexTestFixture::Metrics m{};
	m.depth = 4;
	m.current_move = any;
	m.current_score = 0; // suspicious zero
	m.nodes_searched = 5000;
	m.pv_length = 3;
	m.interrupted = true;
	m.move_changed = false;
	m.score_delta = -300;
	m.completion_ratio = 0.5;

	AIPerlexTestFixture::State s{};
	s.depth_completed = 3;
	s.best_score = 300; // abs > score_draw_threshold (20)
	s.nodes_at_completed_depth = 5000;

	REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::SCORE_DROP);
}

TEST_CASE("Search - assess: move changed on interrupt yields MOVE_CHANGED", "[search]")
{
	AIPerlexTestFixture fix;
	const Move any = AnyLegalMove();

	AIPerlexTestFixture::Metrics m{};
	m.depth = 4;
	m.current_move = any;
	m.current_score = 100;
	m.nodes_searched = 5000;
	m.pv_length = 3;
	m.interrupted = true;
	m.move_changed = true; // different from last iteration
	m.score_delta = 10;
	m.completion_ratio = 0.5;

	AIPerlexTestFixture::State s{};
	s.depth_completed = 3;
	s.best_score = 90;
	s.nodes_at_completed_depth = 5000;
	s.last_iteration_move = Move{}; // not read by assess_iteration_quality;
	                                // CASE 5 fires on metrics.move_changed == true
	                                // && state.depth_completed > 0

	REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::MOVE_CHANGED);
}

// ============================================================================
// should_stop_early tests
// ============================================================================

TEST_CASE("Search - should_stop_early: mate score returns true", "[search]")
{
	AIPerlexTestFixture fix;
	// GameValues::Mate_Threshold == 29900; mate score is >= this
	REQUIRE(fix.stop_early(5, GameValues::Mate_Threshold, 4) == true);
	REQUIRE(fix.stop_early(5, GameValues::Mate_Threshold + 100, 4) == true);
	REQUIRE(fix.stop_early(5, -(GameValues::Mate_Threshold), 4) == true);
}

TEST_CASE("Search - should_stop_early: short PV relative to depth returns true", "[search]")
{
	AIPerlexTestFixture fix;
	// Condition: depth > 1 && pv_length > 0 && pv_length < (depth - depth/2)
	// depth=6, pv_length=2 → 2 < (6-3)=3 → true
	REQUIRE(fix.stop_early(6, 100, 2) == true);
	// depth=4, pv_length=1 → 1 < (4-2)=2 → true
	REQUIRE(fix.stop_early(4, 100, 1) == true);
	// depth=4, pv_length=2 → 2 == (4-2)=2, not < → false
	REQUIRE(fix.stop_early(4, 100, 2) == false);
	// depth=1: condition requires depth > 1 → false
	REQUIRE(fix.stop_early(1, 100, 0) == false);
}

// ============================================================================
// handle_empty_move_emergency tests
// ============================================================================

TEST_CASE("Search - handle_empty_move_emergency: mate score returns false (no move needed)", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position

	AIPerlexTestFixture::State s{};
	s.best_move = Move{};                           // null — no move found
	s.best_score = GameValues::Mate_Threshold + 50; // mate detected

	REQUIRE(fix.emergency(s) == false); // game is over, no move needed
	// best_move remains null — caller must not play
	REQUIRE(s.best_move.is_null());
}

TEST_CASE("Search - handle_empty_move_emergency: non-mate emergency sets a legal move", "[search]")
{
	// default ctor sets up a real, playable starting position so the
	// emergency path finds legal moves
	AIPerlexTestFixture fix;

	AIPerlexTestFixture::State s{};
	s.best_move = Move{}; // null — emergency condition
	s.best_score = 0;     // not a mate score

	const bool result = fix.emergency(s);

	REQUIRE(result == true);         // emergency move was found
	REQUIRE(!s.best_move.is_null()); // a move was set
}

TEST_CASE("Search - handle_empty_move_emergency: a stale row 1 is not spliced onto the emergency move", "[search][pv]")
{
	// PVTable::update copies row ply + 1 onto the end of row ply, and row 1 at this point holds
	// whatever subtree last reached ply 1 — a different position. Without clearing it first the
	// emergency move is published with a tail that describes nothing, which is #310's defect
	// arriving by another route.
	AIPerlexTestFixture fix;
	fix.seed_pv_row(1, AnyLegalMove());
	REQUIRE(fix.pv_length(1) == 1); // the stale row the emergency path must not read

	AIPerlexTestFixture::State s{};
	s.best_move = Move{};
	s.best_score = 0;

	REQUIRE(fix.emergency(s));

	REQUIRE(fix.pv_length(0) == 1); // exactly the emergency move, nothing spliced on
	REQUIRE(fix.pv_move(0) == s.best_move);
	REQUIRE(pv_replays_legally(fix.board_, fix.pv_line(0)));
}

// ============================================================================
// should_try_null_move tests
// ============================================================================

TEST_CASE("Search - should_try_null_move: disabled returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.ai->tuning().null_move_enabled = false;

	REQUIRE(fix.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: PV node returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.ai->tuning().null_move_enabled = true;

	REQUIRE(fix.try_null_move(4, 0, 1, /*is_pv_node=*/true, false) == false);
}

TEST_CASE("Search - should_try_null_move: in check returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.ai->tuning().null_move_enabled = true;

	REQUIRE(fix.try_null_move(4, 0, 1, false, /*in_check=*/true) == false);
}

TEST_CASE("Search - should_try_null_move: depth below minimum returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.ai->tuning().null_move_enabled = true;
	fix.ai->tuning().null_move_min_depth = 3;

	REQUIRE(fix.try_null_move(/*depth=*/2, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: mate-score beta returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.ai->tuning().null_move_enabled = true;

	REQUIRE(fix.try_null_move(4, GameValues::Mate_Threshold, 1, false, false) == false);
	REQUIRE(fix.try_null_move(4, -GameValues::Mate_Threshold, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: zugzwang (no non-pawn material) returns false", "[search]")
{
	// White: king + pawn only. Black: king only. No non-pawn material for
	// the side to move (white) -> zugzwang guard must refuse NMP.
	AIPerlexTestFixture fix("8/8/8/3k4/8/3K4/3P4/8 w - - 0 1");
	fix.ai->tuning().null_move_enabled = true;

	REQUIRE(fix.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: single non-pawn piece returns false (issue #66)", "[search]")
{
	// QFORK-001 (issue #66): KQ vs KR is won via domination/zugzwang — Black
	// loses only because he must move. Letting the side with a lone rook
	// "pass" makes the null search report that Black holds, hiding the win.
	// The zugzwang guard must refuse NMP whenever the side to move has fewer
	// than two non-pawn pieces.
	AIPerlexTestFixture black_to_move("8/8/8/3r4/4k3/8/8/3QK3 b - - 0 1");
	black_to_move.ai->tuning().null_move_enabled = true;
	REQUIRE(black_to_move.try_null_move(4, 0, 1, false, false) == false);

	// Same position, White to move: a lone queen is refused too.
	AIPerlexTestFixture white_to_move("8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1");
	white_to_move.ai->tuning().null_move_enabled = true;
	REQUIRE(white_to_move.try_null_move(4, 0, 1, false, false) == false);

	// One knight + six pawns is still refused: the guard counts non-pawn
	// pieces, deliberately ignoring pawns (material-count-based, not
	// phase-based).
	AIPerlexTestFixture knight_and_pawns("4k3/8/8/8/8/8/PPPPPPN1/4K3 w - - 0 1");
	knight_and_pawns.ai->tuning().null_move_enabled = true;
	REQUIRE(knight_and_pawns.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: two non-pawn pieces returns true", "[search]")
{
	// Queen + knight for the side to move: above the single-piece zugzwang
	// guard threshold, so NMP stays available.
	AIPerlexTestFixture fix("8/8/8/3r4/4k3/8/8/2NQK3 w - - 0 1");
	fix.ai->tuning().null_move_enabled = true;

	REQUIRE(fix.try_null_move(4, 0, 1, false, false) == true);
}

TEST_CASE("Search - should_try_null_move: consecutive null move returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.ai->tuning().null_move_enabled = true;
	fix.set_last_move_was_null(2, true); // ply 2 was reached via a null move

	REQUIRE(fix.try_null_move(4, 0, /*ply=*/2, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: otherwise-eligible position returns true", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.ai->tuning().null_move_enabled = true;
	fix.ai->tuning().null_move_min_depth = 3;

	REQUIRE(fix.try_null_move(4, 0, 1, false, false) == true);
}

// ============================================================================
// SetThreads clamp tests
// ============================================================================
// Verifies the [1, 32] clamp on threads_ in isolation.

TEST_CASE("SMP - SetThreads(0) clamps to 1", "[smp]")
{
	AIPerlexTestFixture fix;
	fix.ai->SetThreads(0);
	REQUIRE(fix.threads() == 1u);
}

TEST_CASE("SMP - SetThreads(64) clamps to 32", "[smp]")
{
	AIPerlexTestFixture fix;
	fix.ai->SetThreads(64);
	REQUIRE(fix.threads() == 32u);
}

TEST_CASE("SMP - SetThreads(4) passes through unchanged", "[smp]")
{
	AIPerlexTestFixture fix;
	fix.ai->SetThreads(4);
	REQUIRE(fix.threads() == 4u);
}

TEST_CASE("SMP - SetThreads default is 1", "[smp]")
{
	AIPerlexTestFixture fix;
	REQUIRE(fix.threads() == 1u);
}

// GetMove() returns the result AFTER the helper threads are joined and their node counts folded
// in, which is the same object GetLastResult() hands out. The two are only distinguishable at
// Threads > 1: the pre-join result carries the main thread's counts alone, so returning it would
// pass at Threads = 1 and silently under-report everywhere else.
TEST_CASE("SMP - GetMove's return value matches GetLastResult at Threads > 1", "[smp]")
{
	AIPerlexTestFixture fix("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 6);

	const SearchResult returned = fix.get_move_at_threads(4, 6);
	const SearchResult last = fix.ai->GetLastResult();

	REQUIRE_FALSE(returned.best_move.is_null());
	CHECK(returned.best_move == last.best_move);
	CHECK(returned.best_score == last.best_score);
	CHECK(returned.depth_completed == last.depth_completed);
	CHECK(returned.game_state == last.game_state);
	CHECK(returned.nodes_searched == last.nodes_searched);
	CHECK(returned.qnodes_searched == last.qnodes_searched);
	CHECK(returned.search_was_stable == last.search_was_stable);

	// The aggregate really is an aggregate. Stated as the exact sum rather than an inequality:
	// a helper whose thread starts after the main search has already finished contributes a
	// legitimate zero, so "greater than the main thread's count" is not guaranteed on a loaded
	// or single-core machine. Without this, the equality checks above would pass on a pre-join
	// result.
	CHECK(returned.nodes_searched == fix.mainnodes() + fix.helper_nodes());
	CHECK(returned.qnodes_searched == fix.qnodes() + fix.helper_qnodes());
}

TEST_CASE("AIPerplex - a completed result reports non-negative elapsed time", "[search]")
{
	// Catches GetMove() dropping the elapsed value from its completed SearchControl session.
	AIPerlexTestFixture fix;

	const SearchResult result = fix.get_move_at_threads(1, 1);

	CHECK(result.elapsed >= std::chrono::milliseconds::zero());
}

TEST_CASE("AIAgent - a completed result reports its unsplit legacy node count", "[search]")
{
	// Catches MakeResult() leaving the legacy count at the SearchResult default, or incorrectly
	// claiming that legacy nodes belong to the separately reported quiescence tree.
	LegacyAiTestFixture fix("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

	const SearchResult result = fix.get_move(3);

	REQUIRE(fix.search_count() > 0);
	CHECK(result.nodes_searched == fix.search_count());
	CHECK(result.qnodes_searched == 0);
}

// ============================================================================
// Terminal results at the root
// ============================================================================
// The producer half of the contract GameLoopTests.cpp exercises from the consumer side: asked
// for a move in a position that has none, a real player returns a null move and names the
// outcome in game_state. That is the only channel left — the state-changed event is gone — so
// nothing else reports the end of a game, and scripted players cannot prove a real one supplies
// it.

TEST_CASE("AIPerplex - a mated root returns no move and names the winner", "[search]")
{
	// Black to move and mated: Ra8 covers the back rank, f7/g7/h7 block every escape.
	AIPerlexTestFixture fix("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");

	const SearchResult result = fix.get_move_at_threads(1, 3);

	CHECK(result.best_move.is_null());
	CHECK(result.game_state == GameStates::WHITE_WON);
}

TEST_CASE("AIPerplex - a stalemated root returns no move and DRAW_PAT", "[search]")
{
	// Black to move, not in check, and every king move is covered: Qf7 takes g8, g7 and h7,
	// with Kg6 covering the last two a second time.
	AIPerlexTestFixture fix("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
	REQUIRE(fix.count_legal_moves() == 0);

	const SearchResult result = fix.get_move_at_threads(1, 3);

	CHECK(result.best_move.is_null());
	CHECK(result.game_state == GameStates::DRAW_PAT);
}

TEST_CASE("AIPerplex - a position with a move reports STILL_PLAYING", "[search]")
{
	// The control for both cases above: without it, a GetMove() that reported a terminal state
	// unconditionally would pass them.
	AIPerlexTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");

	const SearchResult result = fix.get_move_at_threads(1, 3);

	CHECK_FALSE(result.best_move.is_null());
	CHECK(result.game_state == GameStates::STILL_PLAYING);
}

// The root verdict carrier is reset per GetMove() call, and an aborted search never writes it.
// Both halves matter: any search that completes a root frame overwrites the carrier anyway, so an
// abort is the only way a previous call's terminal verdict reaches the caller -- and a terminal
// verdict makes handle_empty_move_emergency() return no move at all.

// What one complete iteration at `depth` costs the poll gate on `fen`.
static int64_t poll_ticks_at_depth(const std::string& fen, int depth)
{
	AIPerlexTestFixture probe(fen, static_cast<unsigned>(depth));
	probe.search_to_depth(depth);
	return probe.poll_ticks();
}

// Kiwipete: 48 root moves, and enough hanging material that quiescence keeps going under most of
// them. Chosen because its depth-1 iteration costs more than one poll interval -- the only shape of
// position where a node limit can abort before any root frame has adjudicated.
static constexpr const char* BUSY_FEN = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
static constexpr int64_t POLL_INTERVAL = 1024; // poll_search_limits(): (++nodes_since_check_ & 1023)

TEST_CASE("AIPerplex - init_search resets the root verdict before any node runs", "[search]")
{
	AIPerlexTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");

	fix.set_root_game_state(GameStates::BLACK_WON);
	fix.call_init_search();

	CHECK(fix.root_game_state() == GameStates::STILL_PLAYING);
}

TEST_CASE("AIPerplex - a search aborted inside its first root frame drops the previous verdict", "[search]")
{
	// Black to move and mated: this call leaves WHITE_WON in the carrier.
	AIPerlexTestFixture fix("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1", /*max_depth=*/8);
	REQUIRE(fix.get_move_at_threads(1, 3).game_state == GameStates::WHITE_WON);

	// Asserted, not assumed: if depth 1 ever gets cheap enough to finish inside one poll interval,
	// the abort below would land after a root frame adjudicated and this test would pass for the
	// wrong reason. It goes red here instead.
	REQUIRE(poll_ticks_at_depth(BUSY_FEN, 1) > POLL_INTERVAL);

	REQUIRE(fix.board_.SetupFromFEN(BUSY_FEN));
	const SearchResult aborted = fix.result_with_nodes(1);

	CHECK(aborted.game_state == GameStates::STILL_PLAYING);
	// The behaviour that depends on it: handle_empty_move_emergency() refuses to supply a move
	// when the carrier names a finished game, so a stale WHITE_WON here costs the engine its move.
	CHECK_FALSE(aborted.best_move.is_null());
}

TEST_CASE("AIAgent - ApplyLimits resets the root verdict before any node runs", "[search]")
{
	LegacyAiTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");

	fix.set_root_game_state(GameStates::BLACK_WON);
	fix.call_apply_limits();

	CHECK(fix.root_game_state() == GameStates::STILL_PLAYING);
}

TEST_CASE("AIAgent - StopSearch latches the composed control for the legacy search guard", "[search_control]")
{
	LegacyAiTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");

	fix.call_apply_limits();
	fix.stop_search();

	CHECK(fix.search_is_aborted());
}

TEST_CASE("AIAgent - a search aborted before its first root frame drops the previous verdict", "[search]")
{
	// One real search first, so m_Line is populated: GetBestMove() asserts on an empty line unless
	// the root was adjudicated, and the aborted call below never reaches Search().
	LegacyAiTestFixture fix("4k3/8/8/8/8/8/1R6/4K3 w - - 5 60");
	REQUIRE(fix.get_move(4).game_state == GameStates::STILL_PLAYING);

	// What a terminal call would have left behind.
	fix.set_root_game_state(GameStates::WHITE_WON);

	// Only game_state is checked: the leftover line makes the returned move two ply stale, which is
	// a legacy quirk of seeding each search from the last one, not what is under test.
	CHECK(fix.get_move_with_spent_clock().game_state == GameStates::STILL_PLAYING);
}

// ============================================================================
// Terminal-node TT storage
// ============================================================================
// A node with no legal move leaves best_value at the -Search_Init sentinel.
// Narrowing that to the TT's int16_t value field wraps it to +15536, so pvs()
// must resolve the checkmate/stalemate score before it classifies and stores
// the entry. These tests drive a single pvs() node so the terminal position
// sits at a chosen ply, then read back what reached the table.

TEST_CASE("Search - checkmate node is stored as an exact ply-adjusted mate score", "[search][tt]")
{
	// Black to move and mated: Ra8 covers the back rank, f7/g7/h7 block every escape.
	AIPerlexTestFixture fix("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 0);

	const int ply = GENERATE(0, 1, 5);
	INFO("ply = " << ply);

	REQUIRE(fix.search_node(/*depth=*/3, ply) == -GameValues::Mate + ply);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	CHECK(entry->value == -GameValues::Mate + ply);
	// The wrapped sentinel this storage path used to write instead.
	CHECK(entry->value != static_cast<int16_t>(-GameValues::Search_Init));
	CHECK(entry->bound == BoundType::EXACT);
	CHECK(entry->best_move.is_null());
	CHECK(entry->phase == SearchPhase::MAIN);
}

TEST_CASE("Search - stalemate node is stored as an exact draw score", "[search][tt]")
{
	// White to move and stalemated: Qf2 covers g1/g2/h2 without giving check.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/5q2/7K w - - 0 1");
	REQUIRE_FALSE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 0);

	const int ply = GENERATE(0, 2, 7);
	INFO("ply = " << ply);

	REQUIRE(fix.search_node(/*depth=*/3, ply) == GameValues::Draw);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	CHECK(entry->value == GameValues::Draw);
	CHECK(entry->value != static_cast<int16_t>(-GameValues::Search_Init));
	CHECK(entry->bound == BoundType::EXACT);
	CHECK(entry->best_move.is_null());
	CHECK(entry->phase == SearchPhase::MAIN);
}

TEST_CASE("Search - terminal score is stored exact even when it falls outside the window", "[search][tt]")
{
	// The terminal value is the position's true minimax value, not a window-relative
	// bound, so it is stored EXACT regardless of the window it was searched under.
	AIPerlexTestFixture fix("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1");
	REQUIRE(fix.count_legal_moves() == 0);

	REQUIRE(fix.search_node(/*depth=*/3, /*ply=*/0, /*alpha=*/100, /*beta=*/200) == -GameValues::Mate);

	const auto entry = fix.probe_tt(0);
	REQUIRE(entry.has_value());
	CHECK(entry->value == -GameValues::Mate);
	CHECK(entry->bound == BoundType::EXACT);
}

TEST_CASE("Search - a probe of a terminal entry cannot return the wrapped sentinel", "[search][tt]")
{
	// The stored value only misleads a prober whose alpha already exceeds +15536,
	// which is the regime an aspiration window enters after a mate score is seeded.
	// Stored as an UPPER bound of +15536, the entry collapsed such a window and
	// handed the caller +155 pawns for a drawn position; stored EXACT it cannot.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/5q2/7K w - - 0 1");
	REQUIRE(fix.count_legal_moves() == 0);

	// Seed the entry at a depth deeper than the probing call will ask for.
	REQUIRE(fix.search_node(/*depth=*/5, /*ply=*/0) == GameValues::Draw);

	const int score = fix.search_node(/*depth=*/1, /*ply=*/0, /*alpha=*/GameValues::Mate - 60,
	                                  /*beta=*/GameValues::Mate, /*is_pv_node=*/false);
	CHECK(score == GameValues::Draw);
	CHECK(score != static_cast<int16_t>(-GameValues::Search_Init));
}

TEST_CASE("Search - a full search stores its mated child node correctly", "[search][tt]")
{
	// Ra8 is mate in one. Depth 2 is the shallowest search that reaches the mated
	// position through pvs(): at depth 1 the child node is entered with depth 0 and
	// handed to quiescence, which never generates moves or detects mate.
	const std::string fen = "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1";
	AIPerlexTestFixture fix(fen);

	Board mated(fen);
	const Move mating_move = MoveFormatter::FromUCI("a1a8", mated);
	REQUIRE(mated.DoMove(mating_move));
	const uint64_t mated_key = mated.get_zobrist_hash();

	REQUIRE(MoveFormatter::ToUCI(fix.search_to_depth(2)) == "a1a8");

	// The mated position was searched at ply 1, so probing at ply 1 must give back
	// the same mate-in-one the root saw.
	const auto entry = fix.probe_tt(mated_key, 1);
	REQUIRE(entry.has_value());
	CHECK(entry->value == -GameValues::Mate + 1);
	CHECK(entry->bound == BoundType::EXACT);
	CHECK(entry->best_move.is_null());
}

// ============================================================================
// Delta pruning bound
// ============================================================================
// Delta pruning needs an OPTIMISTIC bound on what a move can win. MoveHelper::Value
// is MVV-LVA — an ordering heuristic that subtracts a sixteenth of the moving piece —
// so it understates the gain and is not usable as a bound. For a king (10 000) that
// subtraction is 625, which turns a won pawn into -525 and discards the capture.
// Officer and king captures only became reachable in quiescence with #306, which is
// what exposed this.

TEST_CASE("Qsearch - delta pruning keeps a king capture that wins a pawn", "[search][qsearch]")
{
	// Kxd2 wins an undefended pawn. It is the only capture available, and the rooks
	// keep the position clear of insufficient-material handling so the gain shows up.
	AIPerlexTestFixture fix("7k/8/8/8/r7/8/3p4/3KR3 w - - 0 1");
	REQUIRE_FALSE(fix.board_.InCheck());

	const int stand_pat = fix.evaluate();
	const int score = fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init,
	                                   AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0);

	// Standing pat is always available, so the node can never score below it. Scoring
	// exactly it means the only capture was pruned before it was ever searched.
	INFO("stand_pat = " << stand_pat << ", qsearch = " << score);
	CHECK(score > stand_pat);
}

TEST_CASE("MoveHelper - DeltaGain bounds the material a move can win", "[search][qsearch]")
{
	Board board("7k/8/8/8/r7/8/3p4/3KR3 w - - 0 1");
	const Move kingTakesPawn = MoveFormatter::FromUCI("d1d2", board);
	// The bound is the pawn itself, not the pawn minus a sixteenth of the king.
	CHECK(MoveHelper::DeltaGain(kingTakesPawn, board.GetEffectiveMovPiece(kingTakesPawn),
	                            board.GetCapturedPiece(kingTakesPawn)) == 100);

	// A promotion is worth the piece it becomes less the pawn it consumes, and a
	// capture-promotion adds the captured piece on top.
	Board promo("r3k3/1P6/8/8/8/8/8/4K3 w - - 0 1");
	const Move queenPromo = MoveFormatter::FromUCI("b7b8q", promo);
	CHECK(MoveHelper::DeltaGain(queenPromo, promo.GetEffectiveMovPiece(queenPromo),
	                            promo.GetCapturedPiece(queenPromo)) == 800);

	// 200, not the ">= 800" the old delta-pruning comment claimed for all promotions.
	const Move knightPromo = MoveFormatter::FromUCI("b7b8n", promo);
	CHECK(MoveHelper::DeltaGain(knightPromo, promo.GetEffectiveMovPiece(knightPromo),
	                            promo.GetCapturedPiece(knightPromo)) == 200);

	const Move queenPromoCapture = MoveFormatter::FromUCI("b7a8q", promo);
	CHECK(MoveHelper::DeltaGain(queenPromoCapture, promo.GetEffectiveMovPiece(queenPromoCapture),
	                            promo.GetCapturedPiece(queenPromoCapture)) == 800 + 500);

	// A quiet move wins nothing.
	const Move quiet = MoveFormatter::FromUCI("e1e2", promo);
	CHECK(MoveHelper::DeltaGain(quiet, promo.GetEffectiveMovPiece(quiet), promo.GetCapturedPiece(quiet)) == 0);
}

// ============================================================================
// Legal quiescence while in check
// ============================================================================
// A side to move in check may not decline to move, so quiescence cannot settle
// such a node by standing pat, and a capture-only move list cannot answer it:
// blocks and king walks are evasions too. Each case below is a position where
// the capture-only, stand-pat-first version returns a different answer.

TEST_CASE("Qsearch - in check, stand-pat cannot cut off and mate is seen", "[search][qsearch]")
{
	// White is two knights up and in check from Ra1; f2/g2/h2 are its own pawns and
	// f1/h1 are covered along the rank, so it is mate. Neither knight can reach the
	// first rank. Standing pat here returns a winning score for a lost position.
	AIPerlexTestFixture fix("7k/8/8/NN6/8/8/5PPP/r5K1 w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 0);

	constexpr int beta = 100;
	// The precondition that makes this test meaningful: stand-pat would have cut off.
	REQUIRE(fix.evaluate() >= beta);

	const int ply = 3;
	CHECK(fix.quiesce_node(-GameValues::Search_Init, beta, AIPerlexTestFixture::QSEARCH_BUDGET, ply) ==
	      -GameValues::Mate + ply);
}

// The assertion that carries these three is the TT's stored best move: it names the
// evasion the node actually searched. "Not mate" alone is not enough — the capture-only
// version returns stand-pat here, which is also not mate, so such a test passes on the
// very behaviour it is meant to reject.

TEST_CASE("Qsearch - in check, a quiet king evasion is found", "[search][qsearch]")
{
	// Ra1 checks along the rank; every escape is a quiet king step off it, and no
	// capture exists. A capture-only generator sees an empty list here.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/r5K1 w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() > 0);

	const int ply = 2;
	const int score =
	    fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, AIPerlexTestFixture::QSEARCH_BUDGET, ply);
	CHECK(score > -GameValues::Mate_Threshold);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	REQUIRE_FALSE(entry->best_move.is_null());
	// Only the king can move, so whichever escape was chosen must start on g1.
	CHECK(entry->best_move.from() == g1);
}

TEST_CASE("Qsearch - in check, a quiet blocking evasion is found and refuted", "[search][qsearch]")
{
	// Ra1 checks along the rank, the king is walled in by its own pawns, and the only
	// legal reply is the quiet interposition Rd7-d1 — a move no capture-only generator
	// produces. The block does not save the game: Rxd1 renews the check with nothing left
	// to interpose. Both halves of that line are needed to see it, so this case fails
	// under either change alone — a capture-only evasion list never finds Rd1, and a
	// pawn-only capture list never finds the officer recapture that mates.
	AIPerlexTestFixture fix("7k/3R4/8/8/8/8/5PPP/r5K1 w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 1);

	const int ply = 2;
	const int score =
	    fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, AIPerlexTestFixture::QSEARCH_BUDGET, ply);
	CHECK(score == -GameValues::Mate + ply + 2);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	REQUIRE_FALSE(entry->best_move.is_null());
	CHECK(MoveFormatter::ToUCI(entry->best_move) == "d7d1");
}

TEST_CASE("Qsearch - in check, a capturing evasion is still found", "[search][qsearch]")
{
	// Ne2 checks the boxed-in king and a knight check cannot be blocked, so Re7xe2 is
	// the only legal reply. This is the one evasion shape the old generator could see;
	// it must survive the switch to a full evasion list.
	AIPerlexTestFixture fix("7k/4R3/8/8/8/8/4nPPP/5RKR w - - 0 1");
	REQUIRE(fix.board_.InCheck());
	REQUIRE(fix.count_legal_moves() == 1);

	const int ply = 2;
	const int score =
	    fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, AIPerlexTestFixture::QSEARCH_BUDGET, ply);
	CHECK(score > -GameValues::Mate_Threshold);

	const auto entry = fix.probe_tt(ply);
	REQUIRE(entry.has_value());
	REQUIRE_FALSE(entry->best_move.is_null());
	CHECK(MoveFormatter::ToUCI(entry->best_move) == "e7e2");
}

TEST_CASE("Qsearch - an in-check line that repeats the root scores as a draw", "[search][qsearch][repetition]")
{
	// The termination case the in-check path introduced. White's king is checked along the
	// rank, steps off it, and the rook re-checks on the next rank — a quiet evasion answered
	// by a quiet check, with no capture anywhere. Four plies later the position is the root
	// again, and nothing about the material has changed, so this can go on forever.
	//
	// Removing either the FEN-setup seeding of the repetition history or quiescence's
	// check_draws call makes this return -572 — the static evaluation of a position that is
	// still in check, which is exactly the failure mode being guarded against.
	//
	// It does NOT pin the third part, widening the in-search bound to admit the root: with
	// that reverted the same draw is found one cycle deeper, between two in-search
	// positions, and the score is still 0. That half is covered by the tactical suite
	// instead, where without it the engine shuffles a knight rather than winning a queen
	// (WAC-008, 35/36).
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/r6K w - - 0 1");
	REQUIRE(fix.board_.InCheck());

	const int score =
	    fix.quiesce_after({"h1h2", "a1a2", "h2h1", "a2a1"}, -GameValues::Search_Init, GameValues::Search_Init);
	CHECK(score == GameValues::Draw);
}

TEST_CASE("Qsearch - out of check, the stand-pat cutoff is unchanged", "[search][qsearch]")
{
	// The untouched path: quiet position, evaluation above beta, so the node stands pat
	// and returns beta without generating anything. The queen sits on b1, not a1: from a1
	// it would check the black king on h8, and a FEN whose side-not-to-move is in check is
	// rejected outright by Board::setup_from_fen_impl, leaving an empty board behind.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/1Q4K1 w - - 0 1");
	REQUIRE_FALSE(fix.board_.InCheck());

	constexpr int beta = 100;
	REQUIRE(fix.evaluate() >= beta);

	CHECK(fix.quiesce_node(-GameValues::Search_Init, beta, AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0) == beta);
}

// ============================================================================
// Quiescence TT depth: remaining budget, not plies consumed
// ============================================================================
// The transposition table's depth field means "search still to come" on the main
// path, and quiescence now writes the same unit. Both tests enter through pvs() so
// that the budget is the one the search hands out: with plies counted as consumed
// instead, the first would see 0 stored at a fresh node, and the second would reuse
// an entry produced with almost no search left.

TEST_CASE("Qsearch - a fresh node stores its remaining budget as the entry depth", "[search][qsearch][tt]")
{
	// A quiet position, so the node stands pat and stores without recursing: the entry's
	// depth is then exactly the budget pvs() handed the node.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/1Q4K1 w - - 0 1");

	fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

	const auto entry = fix.probe_tt(/*ply=*/0);
	REQUIRE(entry.has_value());
	CHECK(entry->phase == SearchPhase::QUIESCENCE);
	CHECK(entry->depth == AIPerlexTestFixture::QSEARCH_BUDGET);
}

TEST_CASE("Qsearch - an entry with less remaining budget does not satisfy a fresh node", "[search][qsearch][tt]")
{
	// A value no evaluation of this position could produce, so returning it proves the entry
	// was reused rather than the node re-searched.
	constexpr int16_t planted = 12'345;
	const std::string quiet_position = "7k/8/8/8/8/8/8/1Q4K1 w - - 0 1";

	SECTION("insufficient entry is ignored")
	{
		AIPerlexTestFixture fix(quiet_position);
		fix.store_qsearch_entry(planted, /*qsearch_budget=*/1, /*ply=*/0);

		const int score = fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

		CHECK(score != planted);
	}

	SECTION("sufficient entry is reused")
	{
		AIPerlexTestFixture fix(quiet_position);
		fix.store_qsearch_entry(planted, AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/0);

		const int score = fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

		CHECK(score == planted);
	}

	SECTION("an entry from an exhausted in-check chain is ignored")
	{
		// In check the budget is bypassed, so a chain of evasions drives it negative and the
		// entries it stores carry negative depths. This pins the contract for that range
		// rather than reproducing a past defect: under plies-consumed those entries carried
		// the *largest* depths in the table, so the situation this rules out could not arise
		// in the same shape, and reverting the unit does not fail this section.
		AIPerlexTestFixture fix(quiet_position);
		fix.store_qsearch_entry(planted, /*qsearch_budget=*/-5, /*ply=*/0);

		const int score = fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

		CHECK(score != planted);
	}
}

TEST_CASE("Qsearch - the root of a check chain records its full budget", "[search][qsearch][tt]")
{
	// A lone rook checking a cornered king: every evasion is quiet, so the chain runs on the
	// in-check path that ignores the budget. The chain's own entries carry negative budgets;
	// this pins the root, which is the only one of them reachable by key from here.
	AIPerlexTestFixture fix("7k/8/8/8/8/8/8/r6K w - - 0 1");
	REQUIRE(fix.board_.InCheck());

	fix.quiesce_via_pvs(-GameValues::Search_Init, GameValues::Search_Init, /*ply=*/0);

	const auto entry = fix.probe_tt(/*ply=*/0);
	REQUIRE(entry.has_value());
	CHECK(entry->phase == SearchPhase::QUIESCENCE);
	// The root of the chain still holds its full budget; the entries below it are the negative
	// ones, and they are unreachable from here without walking the tree. What this pins is that
	// the root is not itself recorded as exhausted.
	CHECK(entry->depth == AIPerlexTestFixture::QSEARCH_BUDGET);
}

// ============================================================================
// Quiescence node accounting
// ============================================================================
// nodes_searched counts pvs() edges only, so quiescence work reached the nps
// denominator's time but never its numerator's count. These pin the split.

TEST_CASE("Search - quiescence nodes are counted separately from main-tree nodes", "[search][nodes]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	REQUIRE_FALSE(fix.search_to_depth(6).is_null());

	// A zero on either side means a counter stopped being incremented — silent
	// in every other test.
	CHECK(fix.mainnodes() > 0);
	CHECK(fix.qnodes() > 0);

	// At Threads=1 there are no helper counts to add, so the result's fields are exactly
	// the main thread's, and the two are reported separately rather than pre-summed.
	const SearchResult result = fix.last_result();
	CHECK(result.nodes_searched == fix.mainnodes());
	CHECK(result.qnodes_searched == fix.qnodes());
}

TEST_CASE("Search - node counters reset between searches", "[search][nodes]")
{
	AIPerlexTestFixture fix("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	REQUIRE_FALSE(fix.search_to_depth(5).is_null());
	const int64_t first_main = fix.mainnodes();
	const int64_t first_q = fix.qnodes();
	REQUIRE(first_main > 0);
	REQUIRE(first_q > 0);

	// A second search on the SAME fixture reuses a warm TT, so its count is not comparable
	// with the first. The sentinel makes the reset provable anyway: a counter that
	// accumulated instead of resetting can only report back at least this much.
	constexpr int64_t sentinel = 1'000'000'000;
	fix.poison_node_counters(sentinel);

	REQUIRE_FALSE(fix.search_to_depth(5).is_null());
	CHECK(fix.mainnodes() < sentinel);
	CHECK(fix.qnodes() < sentinel);
	CHECK(fix.mainnodes() > 0);
	CHECK(fix.qnodes() > 0);

	// The exact-equality form of the same property, with the table taken out of it:
	// an independent fixture is a fresh AI and a fresh TT, so a correctly reset
	// counter must reproduce the first search's count exactly.
	AIPerlexTestFixture fresh("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
	REQUIRE_FALSE(fresh.search_to_depth(5).is_null());
	CHECK(fresh.mainnodes() == first_main);
	CHECK(fresh.qnodes() == first_q);
}

// ============================================================================
// go nodes — node-limit poll (SearchLimits::fixed_nodes)
// ============================================================================
// The poll in pvs()/quiescence() fires only every 1024 nodes on thread 0, so
// the search cannot stop exactly at the budget — these pin the observable
// contract instead: it stops within one poll interval, and it does so the
// same way every time.

TEST_CASE("Search - fixed_nodes stops within one poll interval past the budget", "[search][nodes][search_control]")
{
	// After 1.e4 e5 2.Nf3, black to move. max_depth=8 is chosen so that this test
	// fails rather than hangs if the poll ever stops firing: a full depth-8 search
	// here costs far more than the budget, so a broken limit overshoots the upper
	// bound below and goes red in about a second — while with a working limit the
	// search still stops on nodes, mid-iteration, well before depth 8.
	AIPerlexTestFixture fix("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2", /*max_depth=*/8);

	constexpr int64_t node_budget = 20000;
	const Move move = fix.search_with_nodes(node_budget);
	REQUIRE_FALSE(move.is_null());

	const int64_t total_nodes = fix.mainnodes() + fix.qnodes();
	CHECK(total_nodes >= node_budget);
	CHECK(fix.search_is_aborted());
	// Loose on purpose: the limit is observed only at multiples of 1024, and during
	// the abort collapse the parent pvs()/quiescence() frames each add their own
	// nodes before unwinding. Do not tighten without re-deriving the bound.
	CHECK(total_nodes <= node_budget + 8192);
}

TEST_CASE("Search - fixed_nodes is deterministic across repeated searches", "[search][nodes]")
{
	// This is the property the abort-collapse strategy depends on: a fresh search
	// hitting the same node budget from the same position must behave identically
	// every time, not just land in a range.
	const std::string fen = "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2";
	constexpr int64_t node_budget = 20000;

	AIPerlexTestFixture first(fen, /*max_depth=*/8);
	const Move first_move = first.search_with_nodes(node_budget);
	REQUIRE_FALSE(first_move.is_null());
	const int64_t first_total = first.mainnodes() + first.qnodes();

	AIPerlexTestFixture second(fen, /*max_depth=*/8);
	const Move second_move = second.search_with_nodes(node_budget);
	REQUIRE_FALSE(second_move.is_null());
	const int64_t second_total = second.mainnodes() + second.qnodes();

	CHECK(MoveFormatter::ToUCI(first_move) == MoveFormatter::ToUCI(second_move));
	CHECK(first_total == second_total);
}

TEST_CASE("Search - a pvs frame that aborts at entry leaves an empty pv row", "[search][pv]")
{
	// The one abort the unwind guard cannot cover, because it happens before the frame searches
	// anything: the entry exit returns a fabricated GameValues::Draw. A parent frame discards
	// that at its own guard, but the root's caller is search_with_aspiration(), which hands it to
	// iterative_deepening() as this iteration's score. Row 0 is what decides whether that score
	// is believed — a populated row (here, a completed aspiration retry's line) makes
	// metrics.current_move plausible and lets `score cp 0` through CASE 4 in a balanced position.
	// Clearing the row before the exit is what turns it into the INCOMPLETE rejection instead.
	AIPerlexTestFixture fix;
	fix.seed_pv_row(0, AnyLegalMove());
	REQUIRE(fix.pv_length(0) == 1); // the earlier retry's line, still standing

	fix.ai->StopSearch(); // latch the abort flag, as UCI 'stop' or an expired clock would

	const int score = fix.search_node(/*depth=*/4, /*ply=*/0);

	REQUIRE(score == GameValues::Draw); // fabricated: nothing was searched
	REQUIRE(fix.pv_length(0) == 0);     // ... and nothing is published alongside it
	REQUIRE(fix.pv_move(0).is_null());
}
