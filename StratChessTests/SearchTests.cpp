// SearchTests.cpp — Catch2 [search] tests for AIPerplex private helper methods.
//
// Tests for:
//   assess_iteration_quality() — 6 cases, one per RejectionReason branch
//   should_stop_early()        — 2 cases (mate score, forced-line short-circuit)
//   handle_empty_move_emergency() — 2 cases (mate path, true-emergency path)
//   should_try_null_move()     — 8 cases, one per guard branch (disabled, PV,
//                                 in-check, depth, mate-score, zugzwang,
//                                 consecutive-null, otherwise-eligible)
//
// Requires STRAT_ENABLE_TEST_ACCESS in the test project preprocessor definitions.
// See Docs/TestDesign.md §"AIPerplex Test Access" for the mechanism.

#include <catch_amalgamated.hpp>
#include "AIPerplex.h"
#include "Board.h"
#include "MoveGenerator.h"
#include "PlayerBase.h"
#include "PVTable.h"
#include "defines.h"

// ============================================================================
// Test fixture
// ============================================================================
// Must be defined here (not in a header) — the name must match the friend
// declaration inside AIPerplex.h: friend class AIPerlexTestFixture;
//
// Public type aliases re-export the private AIPerplex nested types so that
// TEST_CASE functions outside the class can write e.g.
//   AIPerlexTestFixture::RejectionReason::INCOMPLETE
class AIPerlexTestFixture
{
public:
    // Re-export private types for test use
    using RejectionReason = AIPerplex::RejectionReason;
    using Metrics         = AIPerplex::IterationMetrics;
    using State           = AIPerplex::SearchState;

    std::unique_ptr<PlayerBase> ai_owner;
    AIPerplex* ai = nullptr;

    AIPerlexTestFixture()
    {
        // depth=4 sets max_depth_ (the IDS hard cap used if GetMove() is ever called).
        // None of the current [search] tests call GetMove(), so this is a don't-care.
        ai_owner = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, 4, Board::Instance());
        ai = static_cast<AIPerplex*>(ai_owner.get());
        AIPerplex::SetVerboseLogging(false);
        // Note: SetEvalEngine() is NOT called — the helper methods under test
        // do not invoke Eval->Evaluate(), so this is safe.
    }

    RejectionReason assess(const Metrics& m, const State& s) const
        { return ai->assess_iteration_quality(m, s); }

    bool stop_early(int depth, int score, int pv_len) const
        { return ai->should_stop_early(depth, score, pv_len); }

    bool emergency(State& s, PVTable& pv) const
        { return ai->handle_empty_move_emergency(s, pv); }

    bool try_null_move(int depth, int beta, int ply, bool is_pv_node, bool in_check) const
        { return ai->should_try_null_move(depth, beta, ply, is_pv_node, in_check); }

    // Pokes the private consecutive-null-move guard array. Needed because
    // last_move_was_null_ is private on AIPerplex — only AIPerlexTestFixture
    // (the declared friend) can reach it, not the free TEST_CASE functions.
    void set_last_move_was_null(int ply, bool value) const
        { ai->last_move_was_null_[ply] = value; }
};

// ============================================================================
// Helper
// ============================================================================
// Returns any legal move from the starting position.
// Used to produce a guaranteed non-null Move for assess tests.
static Move AnyLegalMove()
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    GameInfo info = Board::Instance().GetGameInfo();
    MoveList ml;
    MoveGenerator::ComputeLegalMoves(Board::Instance(), info, ml);
    REQUIRE(!ml.empty());
    return ml[0];
}

// ============================================================================
// assess_iteration_quality tests
// ============================================================================

TEST_CASE("Search - assess: null current_move yields INCOMPLETE", "[search]")
{
    AIPerlexTestFixture fix;

    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = Move{};    // null — triggers CASE 1
    m.current_score    = 100;
    m.nodes_searched   = 5000;
    m.pv_length        = 2;
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = 10;
    m.completion_ratio = 0.5;

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 0;
    s.best_score               = 100;
    s.nodes_at_completed_depth = 0;

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::INCOMPLETE);
}

TEST_CASE("Search - assess: too few nodes yields INCOMPLETE", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = any;
    m.current_score    = 100;
    m.nodes_searched   = 10;    // below min_nodes_threshold (default 1000) — CASE 1
    m.pv_length        = 2;
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = 10;
    m.completion_ratio = 0.5;

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 0;
    s.best_score               = 100;
    s.nodes_at_completed_depth = 0;

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::INCOMPLETE);
}

TEST_CASE("Search - assess: low completion ratio yields TOO_FEW_NODES", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    // Pass CASE 1 (move ok, nodes ok) but fail CASE 2 (completion ratio)
    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = any;
    m.current_score    = 100;
    m.nodes_searched   = 5000;
    m.pv_length        = 2;
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = 10;
    m.completion_ratio = 0.01;  // below min_completion_ratio (default 0.10)

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 3;     // > 0: previous depth exists
    s.best_score               = 100;
    s.nodes_at_completed_depth = 5000;  // > 0: denominator present

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::TOO_FEW_NODES);
}

TEST_CASE("Search - assess: pv too short yields SHORT_PV", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    // depth=9, min_pv_ratio=0.33 → min required pv = max(1, int(9*0.33)) = max(1, 2) = 2
    // pv_length=1 < 2 → SHORT_PV
    AIPerlexTestFixture::Metrics m{};
    m.depth            = 9;
    m.current_move     = any;
    m.current_score    = 100;
    m.nodes_searched   = 5000;
    m.pv_length        = 1;     // too short (< 2)
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = 10;
    m.completion_ratio = 0.5;   // passes CASE 2

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 8;
    s.best_score               = 100;
    s.nodes_at_completed_depth = 5000;

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::SHORT_PV);
}

TEST_CASE("Search - assess: score drops to 0 from large value yields SCORE_DROP", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    // current_score == 0, previous was 300 (abs > score_draw_threshold=20) → SCORE_DROP
    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = any;
    m.current_score    = 0;     // suspicious zero
    m.nodes_searched   = 5000;
    m.pv_length        = 3;
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = -300;
    m.completion_ratio = 0.5;

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 3;
    s.best_score               = 300;   // abs > score_draw_threshold (20)
    s.nodes_at_completed_depth = 5000;

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::SCORE_DROP);
}

TEST_CASE("Search - assess: move changed on interrupt yields MOVE_CHANGED", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = any;
    m.current_score    = 100;
    m.nodes_searched   = 5000;
    m.pv_length        = 3;
    m.interrupted      = true;
    m.move_changed     = true;  // different from last iteration
    m.score_delta      = 10;
    m.completion_ratio = 0.5;

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 3;
    s.best_score               = 90;
    s.nodes_at_completed_depth = 5000;
    s.last_iteration_move      = Move{};  // not read by assess_iteration_quality;
                                          // CASE 5 fires on metrics.move_changed == true
                                          // && state.depth_completed > 0

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::MOVE_CHANGED);
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
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;

    AIPerlexTestFixture::State s{};
    s.best_move  = Move{};  // null — no move found
    s.best_score = GameValues::Mate_Threshold + 50;  // mate detected

    PVTable pv;
    REQUIRE(fix.emergency(s, pv) == false);  // game is over, no move needed
    // best_move remains null — caller must not play
    REQUIRE(s.best_move.is_null());
}

TEST_CASE("Search - handle_empty_move_emergency: non-mate emergency sets a legal move", "[search]")
{
    AIPerlexTestFixture fix;

    // Set up a real, playable position so the emergency path finds legal moves
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    AIPerlexTestFixture::State s{};
    s.best_move  = Move{};  // null — emergency condition
    s.best_score = 0;       // not a mate score

    PVTable pv;
    const bool result = fix.emergency(s, pv);

    REQUIRE(result == true);                // emergency move was found
    REQUIRE(!s.best_move.is_null());        // a move was set
}

// ============================================================================
// should_try_null_move tests
// ============================================================================

TEST_CASE("Search - should_try_null_move: disabled returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = false;

    REQUIRE(fix.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: PV node returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;

    REQUIRE(fix.try_null_move(4, 0, 1, /*is_pv_node=*/true, false) == false);
}

TEST_CASE("Search - should_try_null_move: in check returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;

    REQUIRE(fix.try_null_move(4, 0, 1, false, /*in_check=*/true) == false);
}

TEST_CASE("Search - should_try_null_move: depth below minimum returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled  = true;
    fix.ai->tuning().null_move_min_depth = 3;

    REQUIRE(fix.try_null_move(/*depth=*/2, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: mate-score beta returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;

    REQUIRE(fix.try_null_move(4, GameValues::Mate_Threshold, 1, false, false) == false);
    REQUIRE(fix.try_null_move(4, -GameValues::Mate_Threshold, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: zugzwang (no non-pawn material) returns false", "[search]")
{
    // White: king + pawn only. Black: king only. No non-pawn material for
    // the side to move (white) -> zugzwang guard must refuse NMP.
    Board::Instance().SetupFromFEN("8/8/8/3k4/8/3K4/3P4/8 w - - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;

    REQUIRE(fix.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: consecutive null move returns false", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled = true;
    fix.set_last_move_was_null(2, true);   // ply 2 was reached via a null move

    REQUIRE(fix.try_null_move(4, 0, /*ply=*/2, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: otherwise-eligible position returns true", "[search]")
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    AIPerlexTestFixture fix;
    fix.ai->tuning().null_move_enabled  = true;
    fix.ai->tuning().null_move_min_depth = 3;

    REQUIRE(fix.try_null_move(4, 0, 1, false, false) == true);
}
