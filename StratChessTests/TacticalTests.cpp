// TacticalTests.cpp — Catch2 search regression tests
//
// Each test verifies that AIPerplex (depth 4) finds the correct move in a
// forced tactical position. Tests run in < 1 second each (Release build):
// mate-in-1 positions are resolved at depth 1; depth 4 is used so that any
// future depth-reduction heuristic (LMR, futility) still reaches the answer.
//
// Pattern:
//   1. Board::Instance().SetupFromFEN(fen)
//   2. Create AIPerplex via factory; set eval engine and suppress logging
//   3. Call GetMove(info) with GameInfo from the board
//   4. Check m.from() and m.to() against the expected move
//
// When a search regression is found in production, add a new TEST_CASE here
// with a comment explaining the bug (see RepetitionTests.cpp for the pattern).
//
// See Docs/TestDesign.md §Phase 0 for rationale.

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "AIPerplex.h"
#include "PlayerBase.h"
#include "Eval.h"
#include "defines.h"

// ── Search depth used for all tactical tests ──────────────────────────────────
// Depth 4: fast on sparse positions (< 100 ms), deep enough to find forced
// results reliably even after future reductions (LMR, aspiration windows).
static constexpr unsigned TACTICAL_DEPTH = 4;

// ── Helper ────────────────────────────────────────────────────────────────────

// Create a fresh AIPerplex at the given depth, configured for test use.
// Must be called AFTER Board::Instance().SetupFromFEN() because the board
// state is read during search.
static std::unique_ptr<PlayerBase> make_engine(unsigned depth = TACTICAL_DEPTH)
{
    auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth);
    AIPerplex::SetVerboseLogging(false);  // suppress after ctor re-enables it
    ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
    return ai;
}

// ── FEN constants ─────────────────────────────────────────────────────────────

// White: Ra1, Kg1. Black: Kg8, Pf7, Pg7, Ph7.
// Ra8 is the only mating move (back-rank mate). White to move.
static constexpr const char* FEN_MATE_IN_1_ROOK =
    "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1";

// White: Qd2, Kg1. Black: Kg8, Pf7, Pg7, Ph7.
// Qd8# is the only mating move: queen slides d2→d8 along the open d-file.
// On d8, queen covers all of rank 8; king is 3 squares away and cannot capture.
// f7/g7/h7 seal every rank-7 escape square. White to move.
static constexpr const char* FEN_MATE_IN_1_QUEEN =
    "6k1/5ppp/8/8/8/8/3Q4/6K1 w - - 0 1";

// White: Qd1, Ke1. Black: Ke8, Rc1 (unprotected rook on c1).
// Qd1xc1 wins a free rook. White to move.
static constexpr const char* FEN_CAPTURE_HANGING_ROOK =
    "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("Tactical - mate in 1: rook delivers back-rank checkmate", "[tactical]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_MATE_IN_1_ROOK);

    auto ai = make_engine();
    GameInfo info = board.GetGameInfo();
    Move m = ai->GetMove(info);

    // Ra1-a8#
    REQUIRE(m.from() == a1);
    REQUIRE(m.to()   == a8);
}

TEST_CASE("Tactical - mate in 1: queen delivers back-rank checkmate", "[tactical]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_MATE_IN_1_QUEEN);

    auto ai = make_engine();
    GameInfo info = board.GetGameInfo();
    Move m = ai->GetMove(info);

    // Qd2-d8#
    REQUIRE(m.from() == d2);
    REQUIRE(m.to()   == d8);
}

TEST_CASE("Tactical - engine captures hanging rook", "[tactical]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CAPTURE_HANGING_ROOK);

    auto ai = make_engine();
    GameInfo info = board.GetGameInfo();
    Move m = ai->GetMove(info);

    // Qd1xc1 — only move that wins material
    REQUIRE(m.from() == d1);
    REQUIRE(m.to()   == c1);
}
