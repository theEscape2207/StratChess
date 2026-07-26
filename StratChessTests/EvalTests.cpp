// EvalTests.cpp — Catch2 tests for EvalSimple and EvalComplex
//
// Validates the direction and relative magnitude of evaluation scores.
// Exact centipawn values are intentionally NOT tested — positional tables
// and future evaluation changes would make exact-value tests fragile.
//
// Pattern: Board board(fen); then EvalManager::Create(type)->Evaluate(board)
//
// See Docs/TestDesign.md §Phase 0 for rationale.

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "Eval.h"

// ── FEN constants ─────────────────────────────────────────────────────────────

// Symmetric starting position — should evaluate to ~0 for both evaluators.
static constexpr const char* FEN_START =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// White has all pawns + queen + king; black has all pawns + king only (no queen).
// White to move — white has a massive material advantage.
static constexpr const char* FEN_WHITE_EXTRA_QUEEN =
    "4k3/pppppppp/8/8/8/8/PPPPPPPP/3QK3 w - - 0 1";

// Black has all pawns + queen + king; white has all pawns + king only (no queen).
// Black to move — black has a massive material advantage.
static constexpr const char* FEN_BLACK_EXTRA_QUEEN =
    "3qk3/pppppppp/8/8/8/8/PPPPPPPP/4K3 b - - 0 1";

// White doubled pawns on the a-file (Pa2 + Pa3); black has Pa7 + Pb7 (normal structure).
// White to move.
static constexpr const char* FEN_WHITE_DOUBLED =
    "4k3/pp6/8/8/8/P7/P7/4K3 w - - 0 1";

// White normal pawn structure (Pa2 + Pb3); black has Pa7 + Pb7.
// White to move. Same material as FEN_WHITE_DOUBLED but no doubled pawn.
static constexpr const char* FEN_WHITE_NORMAL =
    "4k3/pp6/8/8/8/1P6/P7/4K3 w - - 0 1";

// Endgame: White Ke1 + Re7 (rook on 7th rank). Black Ke8.
// Reduced material triggers ENDGAME stage; rook-on-7th bonus should apply.
// White to move.
static constexpr const char* FEN_ROOK_ON_7TH =
    "4k3/4R3/8/8/8/8/8/4K3 w - - 0 1";

// Mop-up evaluation (issue #70 / epic #110): White King+Queen vs Black King+Rook,
// pawnless, decisive material lead (900 - 500 = 400 cp). Black king cornered (a8)
// vs centered (c6) — everything else identical. White to move.
static constexpr const char* FEN_MOPUP_LOSER_KING_CORNER =
    "k6r/8/8/8/3Q4/8/8/4K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_LOSER_KING_CENTER =
    "7r/8/2k5/8/3Q4/8/8/4K3 w - - 0 1";

// Same as above, but with one pawn each (Pa2/pa7) — mop-up must be gated off
// once pawns are on the board.
static constexpr const char* FEN_MOPUP_LOSER_KING_CORNER_WITH_PAWNS =
    "k6r/p7/8/8/3Q4/8/P7/4K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_LOSER_KING_CENTER_WITH_PAWNS =
    "7r/p7/2k5/8/3Q4/8/P7/4K3 w - - 0 1";

// White King+Knight vs Black King+Bishop, pawnless, materially EQUAL (300 - 300 = 0).
// Same corner/center king placement idea — mop-up must be gated off below the
// decisive-material threshold.
static constexpr const char* FEN_MOPUP_MARGINAL_CORNER =
    "7k/8/8/8/5N2/8/8/b3K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_MARGINAL_CENTER =
    "8/8/2k5/8/5N2/8/8/b3K3 w - - 0 1";

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("Eval - EvalSimple: starting position is near-symmetric (within 200 cp)", "[eval]")
{
    Board board(FEN_START);

    int score = EvalManager::Create(EvalManager::EvalTypes::SIMPLE)->Evaluate(board);

    REQUIRE(score >= -200);
    REQUIRE(score <=  200);
}

TEST_CASE("Eval - EvalComplex: starting position is near-symmetric (within 200 cp)", "[eval]")
{
    Board board(FEN_START);

    int score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

    REQUIRE(score >= -200);
    REQUIRE(score <=  200);
}

TEST_CASE("Eval - EvalSimple: side with extra queen scores > 500 cp", "[eval]")
{
    Board board(FEN_WHITE_EXTRA_QUEEN);

    REQUIRE(EvalManager::Create(EvalManager::EvalTypes::SIMPLE)->Evaluate(board) > 500);
}

TEST_CASE("Eval - EvalSimple: black extra queen scores > 500 cp from black's perspective", "[eval]")
{
    Board board(FEN_BLACK_EXTRA_QUEEN);  // black to move

    REQUIRE(EvalManager::Create(EvalManager::EvalTypes::SIMPLE)->Evaluate(board) > 500);
}

TEST_CASE("Eval - EvalComplex: side with extra queen scores > 500 cp", "[eval]")
{
    Board board(FEN_WHITE_EXTRA_QUEEN);

    REQUIRE(EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board) > 500);
}

TEST_CASE("Eval - both evaluators agree on material advantage direction", "[eval]")
{
    Board board(FEN_WHITE_EXTRA_QUEEN);

    int simple_score  = EvalManager::Create(EvalManager::EvalTypes::SIMPLE)->Evaluate(board);
    int complex_score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

    REQUIRE(simple_score  > 0);
    REQUIRE(complex_score > 0);
}

TEST_CASE("Eval - EvalComplex penalises doubled pawns relative to normal structure", "[eval]")
{
    Board board;

    board.SetupFromFEN(FEN_WHITE_DOUBLED);
    int doubled_score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

    board.SetupFromFEN(FEN_WHITE_NORMAL);
    int normal_score  = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

    // Normal structure must score strictly higher than the doubled-pawn position.
    REQUIRE(normal_score > doubled_score);
}

TEST_CASE("Eval - EvalComplex awards rook-on-7th bonus: position scores positively for white", "[eval]")
{
    // White has a rook on the 7th rank in an endgame. Black has only a king.
    // EvalComplex should award a rook-on-7th bonus and the material edge,
    // so white's evaluation from white's perspective must be positive.
    Board board(FEN_ROOK_ON_7TH);

    int score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

    REQUIRE(score > 0);
}


TEST_CASE("Eval - EvalComplex mop-up: decisively-won pawnless ending scores higher with the losing king cornered", "[eval]")
{
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board cornerBoard(FEN_MOPUP_LOSER_KING_CORNER);
    Board centerBoard(FEN_MOPUP_LOSER_KING_CENTER);

    int cornerScore = eval->Evaluate(cornerBoard);
    int centerScore = eval->Evaluate(centerBoard);

    // Same material both sides (Q vs R, 400 cp lead) — the only difference is
    // how cornered the losing (black) king is. Mop-up must prefer the corner.
    REQUIRE(cornerScore > centerScore);
}

TEST_CASE("Eval - EvalComplex mop-up: gated off once pawns are on the board", "[eval]")
{
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board pawnlessCorner(FEN_MOPUP_LOSER_KING_CORNER);
    Board pawnlessCenter(FEN_MOPUP_LOSER_KING_CENTER);
    int pawnlessDelta = eval->Evaluate(pawnlessCorner) - eval->Evaluate(pawnlessCenter);

    Board pawnsCorner(FEN_MOPUP_LOSER_KING_CORNER_WITH_PAWNS);
    Board pawnsCenter(FEN_MOPUP_LOSER_KING_CENTER_WITH_PAWNS);
    int withPawnsDelta = eval->Evaluate(pawnsCorner) - eval->Evaluate(pawnsCenter);

    // Both variants have the identical king-placement swing available to them;
    // only the pawnless one should get the (larger) mop-up contribution on top.
    REQUIRE(pawnlessDelta > withPawnsDelta);
}

TEST_CASE("Eval - EvalComplex mop-up: gated off below the decisive material threshold", "[eval]")
{
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board decisiveCorner(FEN_MOPUP_LOSER_KING_CORNER);
    Board decisiveCenter(FEN_MOPUP_LOSER_KING_CENTER);
    int decisiveDelta = eval->Evaluate(decisiveCorner) - eval->Evaluate(decisiveCenter);

    Board marginalCorner(FEN_MOPUP_MARGINAL_CORNER);
    Board marginalCenter(FEN_MOPUP_MARGINAL_CENTER);
    int marginalDelta = eval->Evaluate(marginalCorner) - eval->Evaluate(marginalCenter);

    // The 400 cp Q-vs-R lead should swing far more from cornering than the
    // materially-equal N-vs-B case, which gets no mop-up bonus at all.
    REQUIRE(decisiveDelta > marginalDelta);
}
