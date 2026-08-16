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
#include "AIPerplex.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include "PlayerBase.h"
#include "PVTable.h"
#include "MoveHelper.h"
#include "TranspositionTable.h"
#include "defines.h"
#include <chrono>
#include <initializer_list>
#include <optional>

// ============================================================================
// Helper
// ============================================================================
// Returns any legal move from the starting position.
// Used to produce a guaranteed non-null Move for assess tests, and (below)
// by the fixture's own per-game-state pokes.
static Move AnyLegalMove()
{
	Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	GameInfo info = board.GetGameInfo();
	MoveList ml;
	MoveGenerator::ComputeLegalMoves(board, info, ml);
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

	// Re-export private types for test use
	using RejectionReason = AIPerplex::RejectionReason;
	using Metrics = AIPerplex::IterationMetrics;
	using State = AIPerplex::SearchState;

	// Must be declared (and thus constructed/destroyed) before ai_owner —
	// ai_owner holds a Board& reference into it that must outlive it.
	Board board_;
	std::unique_ptr<PlayerBase> ai_owner;
	AIPerplex* ai = nullptr;

	explicit AIPerlexTestFixture(const std::string& fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")
	    : board_(fen)
	{
		// depth=4 sets max_depth_ (the IDS hard cap used if GetMove() is ever called).
		// None of the current [search] tests call GetMove(), so this is a don't-care.
		ai_owner = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, 4, board_);
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

	void add_fake_helper() const { ai->helper_tds_.push_back(std::make_unique<ThreadData>()); }
	size_t helper_count() const { return ai->helper_tds_.size(); }

	void set_last_result_depth(int depth) const { ai->last_result_.depth_completed = depth; }

	void search_depth_one()
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		GameInfo info = board_.GetGameInfo();
		REQUIRE(info.fullMoveCount == 1);
		const Move move = ai->GetMove(info, SearchLimits::fixed_depth(1));
		REQUIRE_FALSE(move.is_null());
	}

	// --- Terminal-node helpers ---

	// Counts the moves that survive DoMove(). ComputeLegalMoves() is pseudo-legal at
	// the edges and pvs() relies on DoMove() to reject the rest, so this is the same
	// notion of "has a move" the search uses. Zero makes the position terminal.
	int count_legal_moves() const
	{
		Board copy = board_;
		GameInfo info = copy.GetGameInfo();
		MoveList ml;
		MoveGenerator::ComputeLegalMoves(copy, info, ml);

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
		ai->td_.info_seq.assign(static_cast<size_t>(ply) + 1, board_.GetGameInfo());
		return ai->pvs(ai->td_, depth, alpha, beta, ply, is_pv_node, *ai->_tt);
	}

	std::optional<TTEntry> probe_tt(int ply) const { return ai->_tt->probe(board_.get_zobrist_hash(), ply); }

	std::optional<TTEntry> probe_tt(uint64_t key, int ply) const { return ai->_tt->probe(key, ply); }

	// Runs one quiescence() node on the fixture's board. The timer is armed because
	// quiescence polls the wall clock every 1024 nodes and a default-constructed
	// TimeManager has its start_time_ at the epoch, which would latch an abort.
	int quiesce_node(int alpha, int beta, int qsearch_depth, int ply) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		ai->time_manager_.start(std::chrono::milliseconds(60'000));
		ai->td_.board = board_;
		ai->td_.nodes_since_check_ = 0;
		ai->td_.info_seq.assign(static_cast<size_t>(ply) + 1, board_.GetGameInfo());
		return ai->quiescence(ai->td_, alpha, beta, qsearch_depth, ply, *ai->_tt);
	}

	// Replays a UCI move list onto td_.board, advancing info_seq in lockstep the way the
	// search does, then runs one quiescence() node at the resulting ply. Lets a test place
	// the node inside a line, with real repetition history behind it, rather than at a
	// synthetic root.
	int quiesce_after(std::initializer_list<const char*> moves, int alpha, int beta) const
	{
		ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
		ai->time_manager_.start(std::chrono::milliseconds(60'000));
		ai->td_.board = board_;
		ai->td_.nodes_since_check_ = 0;
		ai->td_.info_seq.assign(1, board_.GetGameInfo());

		int ply = 0;
		for (const char* uci : moves) {
			const Move move = MoveFormatter::FromUCI(uci, ai->td_.board);
			REQUIRE(ai->td_.board.DoMove(move));
			ai->td_.add_move_to_seq(move, static_cast<size_t>(ply));
			++ply;
		}
		return ai->quiescence(ai->td_, alpha, beta, /*qsearch_depth=*/0, ply, *ai->_tt);
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
		GameInfo info = board_.GetGameInfo();
		return ai->GetMove(info, SearchLimits::fixed_depth(depth));
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
	const int score = fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, 0, /*ply=*/0);

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
	CHECK(fix.quiesce_node(-GameValues::Search_Init, beta, /*qsearch_depth=*/0, ply) == -GameValues::Mate + ply);
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
	const int score = fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, 0, ply);
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
	const int score = fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, 0, ply);
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
	const int score = fix.quiesce_node(-GameValues::Search_Init, GameValues::Search_Init, 0, ply);
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

	CHECK(fix.quiesce_node(-GameValues::Search_Init, beta, 0, /*ply=*/0) == beta);
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
