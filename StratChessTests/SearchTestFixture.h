// SearchTestFixture.h — shared test infrastructure for the [search] test files
// (SearchServiceTests.cpp, SearchIterationTests.cpp, SearchTelemetryTests.cpp,
// SearchTTContractTests.cpp, QuiescenceTests.cpp): the STRAT_ENABLE_TEST_ACCESS fixtures and
// the legal-move helper they share.
//
// Requires STRAT_ENABLE_TEST_ACCESS in the test project preprocessor definitions.
// See Docs/TestDesign.md §"AIPerplex Test Access" for the mechanism.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include "AIAgent.h"
#include "AIPerplex.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include "PlayerFactory.h"
#include "PlayerBase.h"
#include "SearchPlayer.h"
#include "TranspositionTable.h"
#include "defines.h"
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// Player-owned search accessor
// ============================================================================
// Must be defined here — the name must match the friend declaration inside
// SearchPlayer.h: friend class SearchPlayerTestFixture;
class SearchPlayerTestFixture {
  public:
	static AIPerplex& search(IPlayer& player) { return dynamic_cast<SearchPlayer&>(player).search_; }
};

// ============================================================================
// Helper
// ============================================================================
// Returns any legal move from the starting position.
// Used to produce a guaranteed non-null Move for the assess tests, and by the
// fixture's own per-game-state pokes below.
inline Move AnyLegalMove()
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	MoveList ml;
	MoveGenerator::ComputeLegalMoves(board, ml);
	REQUIRE(!ml.empty());
	return ml[0];
}

// ============================================================================
// AIPerplex test fixture
// ============================================================================
// Must be defined here — the name must match the friend declaration inside
// AIPerplex.h: friend class AIPerlexTestFixture;
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

	Board board_;
	std::unique_ptr<AIPerplex> ai_owner;
	AIPerplex* ai = nullptr;

	explicit AIPerlexTestFixture(const std::string& fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	                             unsigned max_depth = 4)
	    : board_(fen)
	{
		// max_depth sets the IDS hard cap used if Search() receives empty limits.
		// Defaults to 4, a don't-care for the many [search] tests that never call
		// Search(); the node-limit tests below raise it so the node poll — not the
		// depth cap — is what stops the search.
		ai_owner = std::make_unique<AIPerplex>(AIPerplexConfig{
		    .evaluator = EvalManager::EvalTypes::COMPLEX, .default_depth = max_depth, .verbose_logging = false});
		ai = ai_owner.get();
		ai->td_.board = board_;
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

	// Reaches the private tuning_ member. Used by the poll-gate tests, which need a search whose
	// cost does not move every time pruning improves.
	void set_see_pruning(bool enabled) const { ai->tuning_.see_pruning_enabled = enabled; }

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
	void call_init_search() const { ai->init_search(board_); }

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

	// --- Quiescence ordering helpers (#320) ---

	// The order quiescence would search this position's moves in, as UCI strings. Calls the
	// same private helper the node itself calls, so reverting that helper's in-check branch
	// makes the ordering tests below fail.
	std::vector<std::string> quiescence_order() const
	{
		MoveList list;
		const bool in_check = ai->td_.board.InCheck();
		if (in_check)
			MoveGenerator::ComputeLegalMoves(ai->td_.board, list);
		else
			MoveGenerator::ComputeCaptures(ai->td_.board, list);

		ai->order_quiescence_moves(ai->td_, list, in_check, 0);

		std::vector<std::string> order;
		order.reserve(list.size());
		for (const auto& move : list)
			order.push_back(MoveFormatter::ToUCI(move));
		return order;
	}

	// Gives one quiet move the history score a real cutoff would have left behind.
	void seed_history(std::string_view uci, int depth) const
	{
		const Move move = MoveFormatter::FromUCI(uci, ai->td_.board);
		REQUIRE_FALSE(move.is_null());
		ai->td_.update_history(ai->td_.board.GetCurrentColor(), move, depth);
	}

	void search_depth_one()
	{
		REQUIRE(board_.fullmove_count() == 1);
		const Move move = ai->Search(board_, SearchLimits::fixed_depth(1)).best_move;
		REQUIRE_FALSE(move.is_null());
	}

	// One complete GetMove() at a chosen thread count — the only way to observe the aggregation
	// GetMove() performs after joining its helpers.
	SearchResult get_move_at_threads(unsigned threads, int depth) const
	{
		ai->SetThreads(threads);
		return ai->Search(board_, SearchLimits::fixed_depth(depth));
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
		ai->td_.board = board_;
		return ai->pvs(ai->td_, depth, alpha, beta, ply, is_pv_node, *ai->_tt);
	}

	// Starts the clock the per-node poll reads, for a search_node() call deep enough to reach
	// it: a default-constructed TimeManager sits at the epoch, so the 1024-node poll would
	// latch an abort. Deliberately not folded into search_node(), which is also used to drive
	// a frame whose abort flag the test latched on purpose.
	void arm_clock() const
	{
		ai->control_.ApplyLimits(SearchLimits::fixed_time(std::chrono::milliseconds(60'000)));
		ai->td_.nodes_since_check_ = 0;
	}

	std::optional<TTEntry> probe_tt(int ply) const { return ai->_tt->probe(board_.get_zobrist_hash(), ply); }

	std::optional<TTEntry> probe_tt(uint64_t key, int ply) const { return ai->_tt->probe(key, ply); }

	// Runs one quiescence() node on the fixture's board. The timer is armed because
	// quiescence polls the wall clock every 1024 nodes and a default-constructed
	// TimeManager has its start_time_ at the epoch, which would latch an abort.
	int quiesce_node(int alpha, int beta, int qsearch_budget, int ply) const
	{
		ai->control_.ApplyLimits(SearchLimits::fixed_time(std::chrono::milliseconds(60'000)));
		ai->td_.board = board_;
		ai->td_.nodes_since_check_ = 0;
		return ai->quiescence(ai->td_, alpha, beta, qsearch_budget, ply, *ai->_tt);
	}

	// Enters quiescence the way the search does — through pvs() with no depth left — so the
	// budget under test is the one pvs() hands out, not one the test chose. Passing the budget
	// in directly would assert nothing about the unit it is expressed in.
	int quiesce_via_pvs(int alpha, int beta, int ply) const
	{
		ai->control_.ApplyLimits(SearchLimits::fixed_time(std::chrono::milliseconds(60'000)));
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

	// Plants a MAIN entry for the fixture's board, so a test can prove what quiescence() will
	// and will not do with a main-search bound for the position in front of it.
	void store_main_entry(int16_t value, int16_t depth, int ply, BoundType bound) const
	{
		ai->_tt->store(board_.get_zobrist_hash(), value, depth, static_cast<int16_t>(ply), Move::EmptyMove(), bound,
		               NodeType::CUT_NODE, SearchPhase::MAIN);
	}

	// Replays a UCI move list onto td_.board, then runs one quiescence() node at the
	// resulting ply. Lets a test place the node inside a line, with real repetition history
	// behind it, rather than at a synthetic root.
	int quiesce_after(std::initializer_list<const char*> moves, int alpha, int beta) const
	{
		ai->control_.ApplyLimits(SearchLimits::fixed_time(std::chrono::milliseconds(60'000)));
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
	int evaluate() const { return ai->evaluator_->Evaluate(board_); }

	// The margin delta pruning adds to its bound. Read rather than hardcoded so a
	// test can place alpha exactly at the pruning threshold without pinning a
	// tuning value it does not care about.
	int delta_pruning_margin() const { return ai->tuning_.delta_pruning_margin; }

	// Full fixed-depth search from the fixture's board.
	Move search_to_depth(int depth) const { return ai->Search(board_, SearchLimits::fixed_depth(depth)).best_move; }
	SearchResult result_to_depth(int depth) const { return ai->Search(board_, SearchLimits::fixed_depth(depth)); }

	// Full search bounded by a node budget instead of a fixed depth. Requires the
	// fixture to be constructed with a max_depth high enough that the node poll,
	// not the IDS depth cap, is what stops the search.
	Move search_with_nodes(int64_t nodes) const
	{
		return ai->Search(board_, SearchLimits::fixed_nodes(nodes)).best_move;
	}
	void set_null_move_enabled(bool enabled) const { ai->tuning_.null_move_enabled = enabled; }
	void set_null_move_min_depth(int depth) const { ai->tuning_.null_move_min_depth = depth; }
	bool null_move_enabled() const { return ai->tuning_.null_move_enabled; }

	bool search_is_aborted() const { return ai->control_.IsAborted(); }

	// The same search, but handing back the whole result. The abort tests need game_state and
	// best_move together, and Threads=1 so the node poll is the only thing that stops it.
	SearchResult result_with_nodes(int64_t nodes) const
	{
		ai->SetThreads(1);
		return ai->Search(board_, SearchLimits::fixed_nodes(nodes));
	}

	// Entries the poll gate counted for the last search: pvs() and quiescence() entries together,
	// which is not nodes_searched + qnodes_searched (those increment past several early returns).
	// iterative_deepening() zeroes it, so this is a per-search figure.
	int64_t poll_ticks() const { return ai->td_.nodes_since_check_; }

	static bool verbose_logging(const AIPerplex& ai) { return ai.verbose_logging_; }
	static std::string evaluator_type(const AIPerplex& ai) { return ai.evaluator_->GetType(); }
	static const SearchTuning& tuning(const AIPerplex& ai) { return ai.tuning_; }
	static unsigned configured_threads(const AIPerplex& ai) { return ai.threads_; }
	static uint64_t game_generation(const AIPerplex& ai) { return ai.game_generation_; }
};

// ============================================================================
// Legacy-agent test fixture
// ============================================================================
// A minimal counterpart to AIPerlexTestFixture for the legacy (non-Lazy-SMP) agents, which
// have no ThreadData and carry root_game_state_ directly on PlayerAiBase. Must be defined
// here — the name must match the friend declaration inside PlayerAI.h:
// friend class LegacyAiTestFixture;
class LegacyAiTestFixture {
  public:
	// Must be declared (and thus constructed/destroyed) before ai_owner —
	// ai_owner holds a Board& reference into it that must outlive it.
	Board board_;
	std::unique_ptr<IPlayer> ai_owner;
	AIAgent* ai = nullptr;

	explicit LegacyAiTestFixture(const std::string& fen, unsigned max_depth = 4) : board_(fen)
	{
		Config::PlayerConfig config;
		config.type = static_cast<unsigned>(PlayerBase::ePlayerTypes::AIAGENT);
		config.depth = max_depth;
		config.eval = static_cast<unsigned>(EvalManager::EvalTypes::COMPLEX);
		ai_owner = CreatePlayer(config, board_);
		ai = static_cast<AIAgent*>(ai_owner.get());
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
