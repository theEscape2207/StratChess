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

#include <sstream>
#include <string>
#include <vector>
#include <string_view>

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

// Color-symmetry regression cases (issue #125) — see MirrorFen below.

// White queen on c6: the case that exposes the pre-fix getEvalBoard defect.
// getEvalBoard(BLACK, sq) used (63 - sq), a 180-degree rotation that mirrors
// files as well as ranks, instead of a vertical flip (sq ^ 56). Before this fix
// the queen PST was not file-symmetric (c6 = 4, f6 = 3), so a White queen on c6
// and its color-mirror (a Black queen on c3) scored 1 cp apart. Black king
// on h8 is not attacked: c6's rank/file/diagonals reach a6-h6, c1-c8, a8, b7,
// d7, e8, d5, e4, f3, g2, h1, b5, a4 — not h8. White to move.
//
// This is the ONLY whole-position case below that discriminates: the others
// contain no queen at all, or (FEN_MOPUP_LOSER_KING_CORNER) a queen whose
// rotated and flipped images happen to hold the same value. The direct
// getEvalBoard tests are the real guard — do not delete them as "redundant
// with the position tests".
static constexpr const char* FEN_QUEEN_C6 =
    "7k/8/2Q5/8/8/8/8/4K3 w - - 0 1";

// Middlegame with rooks on open/half-open files: both sides have 11800 material
// (iMinScore > 11500 -> MIDDLEGAME), exercising the rook open/half-open-file
// terms and the middlegame king PST under color mirroring.
static constexpr const char* FEN_MIDDLEGAME_ROOKS =
    "2rr2k1/pp3ppp/5n2/8/8/2N5/PP3PPP/3RR1K1 w - - 0 1";

// Endgame with pawns: 10100 material each side trips the ENDGAME stage (and so
// the endgame king PST), while the pawns on the board keep the pawnless mop-up
// branch gated off.
static constexpr const char* FEN_ENDGAME_KING_PST =
    "8/5p2/4k3/8/8/2K5/3P4/8 w - - 0 1";

// Rook open-file definition (issue #126). White Re1 + Kf1, Black Kg8 + one
// minor/pawn on the rook's file (e) or one file off (d). The knight's PST
// value is identical on d5/e5 (10), and the pawn's PST value is identical on
// d5/e5 (14), so within each pair the ONLY eval difference is the open-file
// classification.
static constexpr const char* FEN_ROOK_OPEN_FILE_KNIGHT_ON =
    "6k1/8/8/4n3/8/8/8/4RK2 w - - 0 1";
static constexpr const char* FEN_ROOK_OPEN_FILE_KNIGHT_OFF =
    "6k1/8/8/3n4/8/8/8/4RK2 w - - 0 1";
static constexpr const char* FEN_ROOK_OPEN_FILE_PAWN_ON =
    "6k1/8/8/4p3/8/8/8/4RK2 w - - 0 1";
static constexpr const char* FEN_ROOK_OPEN_FILE_PAWN_OFF =
    "6k1/8/8/3p4/8/8/8/4RK2 w - - 0 1";

// Rook open-file, own-pawn-behind decision (D5, issue #126). Same fixed White
// pawn on e4 in both; the rook sits behind it (e6) or ahead of it (e2).
static constexpr const char* FEN_ROOK_OWN_PAWN_BEHIND =
    "6k1/8/4R3/8/4P3/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_ROOK_OWN_PAWN_AHEAD =
    "6k1/8/8/8/4P3/8/4R3/6K1 w - - 0 1";

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

// ── Rook open-file definition (issue #126) ────────────────────────────────────

TEST_CASE("Eval - EvalComplex: an enemy knight on the rook's file does not demote an open file", "[eval]")
{
    // The discriminator: on unmodified HEAD, "open file" tests for absence of
    // ANY enemy piece (all_black/all_white), not just enemy pawns, so a knight
    // sharing the rook's file wrongly demotes it from open to half-open (-5 cp).
    // The knight's PST value is identical on d5 and e5, and material is
    // identical, so the open-file classification is the only thing that can
    // make these two scores differ.
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board knightOn(FEN_ROOK_OPEN_FILE_KNIGHT_ON);
    Board knightOff(FEN_ROOK_OPEN_FILE_KNIGHT_OFF);

    REQUIRE(eval->Evaluate(knightOn) == eval->Evaluate(knightOff));
}

TEST_CASE("Eval - EvalComplex: an enemy pawn on the rook's file still demotes it to half-open", "[eval]")
{
    // Guard: the fix must narrow the open-file test to pawns only, not remove
    // it. An enemy pawn on the file is still the defining case for half-open.
    // The pawn's PST value is identical on d5 and e5, so as above the only
    // possible source of a score difference is the file classification.
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board pawnOn(FEN_ROOK_OPEN_FILE_PAWN_ON);
    Board pawnOff(FEN_ROOK_OPEN_FILE_PAWN_OFF);

    // pawnOff (open file) must still score strictly higher than pawnOn (half-open).
    REQUIRE(eval->Evaluate(pawnOff) > eval->Evaluate(pawnOn));
}

TEST_CASE("Eval - EvalComplex: an own pawn behind the rook still counts as half-open/open (D5)", "[eval]")
{
    // Pins the deliberate D5 decision from
    // .claude/plans/passed-and-backwards-pawn-terms.md: the half-open test
    // only looks at own pawns AHEAD of the rook (g_bbFileUpMask), so an own
    // pawn BEHIND the rook on the same file still leaves the file half-open
    // (and open, since there's no enemy pawn either) — the rook's forward
    // file is genuinely clear. This is not a bug; changing it is out of scope
    // for issue #126.
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board pawnBehind(FEN_ROOK_OWN_PAWN_BEHIND);
    Board pawnAhead(FEN_ROOK_OWN_PAWN_AHEAD);

    REQUIRE(eval->Evaluate(pawnBehind) > eval->Evaluate(pawnAhead));
}

// ── Color-mirroring correctness (issue #125) ──────────────────────────────────
//
// Exposes EvalManager's protected mirroring helper for direct testing — no
// production visibility change; getEvalBoard stays protected on EvalManager.
struct EvalProbe final : EvalManager
{
    int Evaluate(const Board&) const override { return 0; }
    const char* GetType() const override { return "Probe"; }
    using EvalManager::getEvalBoard;
};

TEST_CASE("Eval - getEvalBoard mirrors a Black piece's square vertically, not by 180-degree rotation", "[eval]")
{
    // Direct proof of the issue #125 defect: the pre-fix implementation used
    // (63 - square), a 180-degree rotation. c3 -> c6 is the correct vertical
    // flip; the buggy code instead produced f6 (63 - 42 == 21 == f6).
    REQUIRE(EvalProbe::getEvalBoard(BLACK_QUEEN, c3) == c6);

    REQUIRE(EvalProbe::getEvalBoard(BLACK_QUEEN, a1) == a8);
    REQUIRE(EvalProbe::getEvalBoard(BLACK_QUEEN, h1) == h8);
    REQUIRE(EvalProbe::getEvalBoard(BLACK_QUEEN, a8) == a1);
}

TEST_CASE("Eval - getEvalBoard preserves file for every square (Black)", "[eval]")
{
    for (int sq = a8; sq < NUM_SQUARES; ++sq)
    {
        const auto square = static_cast<eSquare>(sq);
        CAPTURE(sq);
        REQUIRE(File(EvalProbe::getEvalBoard(BLACK_QUEEN, square)) == File(square));
    }
}

TEST_CASE("Eval - getEvalBoard inverts rank for every square (Black)", "[eval]")
{
    for (int sq = a8; sq < NUM_SQUARES; ++sq)
    {
        const auto square = static_cast<eSquare>(sq);
        CAPTURE(sq);
        REQUIRE(Rank(EvalProbe::getEvalBoard(BLACK_QUEEN, square)) == 7 - Rank(square));
    }
}

TEST_CASE("Eval - getEvalBoard leaves White squares unchanged for every square", "[eval]")
{
    for (int sq = a8; sq < NUM_SQUARES; ++sq)
    {
        const auto square = static_cast<eSquare>(sq);
        CAPTURE(sq);
        REQUIRE(EvalProbe::getEvalBoard(WHITE_QUEEN, square) == square);
    }
}

// ── FEN color-mirror helper (test scaffolding only — not engine functionality) ─
//
// Swaps the case of a single character; digits and other characters pass
// through unchanged.
static char SwapPieceCase(char c)
{
    if (c >= 'a' && c <= 'z')
        return static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return c;
}

// Reverses rank order and swaps every piece's color.
static std::string MirrorPlacement(const std::string& placement)
{
    std::vector<std::string> ranks;
    std::stringstream ss(placement);
    std::string rank;
    while (std::getline(ss, rank, '/'))
        ranks.push_back(rank);

    std::string result;
    for (auto it = ranks.rbegin(); it != ranks.rend(); ++it)
    {
        if (!result.empty())
            result += '/';
        for (char c : *it)
            result += SwapPieceCase(c);
    }
    return result;
}

// Swaps case of every castling character and re-emits in canonical KQkq order
// (the FEN parser is order-insensitive, but a canonical form keeps failures
// readable). "-" passes through unchanged.
static std::string MirrorCastling(const std::string& castling)
{
    if (castling == "-")
        return "-";

    std::string swapped;
    for (char c : castling)
        swapped += SwapPieceCase(c);

    std::string canonical;
    for (char c : { 'K', 'Q', 'k', 'q' })
        if (swapped.find(c) != std::string::npos)
            canonical += c;

    return canonical.empty() ? "-" : canonical;
}

// Mirrors the en-passant target's rank (e3 <-> e6, i.e. digit d -> 9 - d); the
// file is unchanged. "-" passes through unchanged.
static std::string MirrorEnPassant(const std::string& ep)
{
    if (ep == "-")
        return "-";

    const char file = ep.at(0);
    const int rank = ep.at(1) - '0';
    return std::string(1, file) + std::to_string(9 - rank);
}

// Color-mirrors a FEN: flips the board vertically and swaps every piece's
// color, so whichever color was to move now faces an identical relative
// situation as the other color in the mirror. Halfmove/fullmove counters pass
// through unchanged.
//
// A color-mirror of a legal position is always legal: the side not to move in
// the mirror is the image of the side not to move in the original, so if the
// original's non-mover isn't in check, neither is the mirror's. Only the
// original FENs (declared above) needed manual legality verification.
static std::string MirrorFen(std::string_view fen)
{
    std::istringstream iss{ std::string(fen) };
    std::string placement, active, castling, ep, halfmove, fullmove;
    iss >> placement >> active >> castling >> ep >> halfmove >> fullmove;

    return MirrorPlacement(placement) + ' '
         + (active == "w" ? "b" : "w") + ' '
         + MirrorCastling(castling) + ' '
         + MirrorEnPassant(ep) + ' '
         + halfmove + ' '
         + fullmove;
}

TEST_CASE("Eval - MirrorFen self-test: mirroring the start position flips only side to move", "[eval]")
{
    // A bug in this test-only helper must not be able to make the symmetry
    // tests below vacuously true; pin down its exact output on a known input.
    REQUIRE(MirrorFen(FEN_START) == "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
}

TEST_CASE("Eval - MirrorFen self-test: castling rights and en-passant square are mirrored", "[eval]")
{
    // FEN_START leaves both helpers untested: its "KQkq" maps to itself under a
    // case swap, and its en-passant field is "-". Evaluate() ignores both fields,
    // so only this self-test can catch MirrorCastling/MirrorEnPassant going wrong.
    // 1. e4 c5 2. — Black has just played c7-c5, so the ep target is c6.
    REQUIRE(MirrorFen("rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2")
            == "rnbqkbnr/pppp1ppp/8/4p3/2P5/8/PP1PPPPP/RNBQKBNR b KQkq c3 0 2");

    // Partial rights must swap colour, not just pass through: White keeps only
    // kingside, Black only queenside, so the mirror must invert that pairing.
    REQUIRE(MirrorFen("r3k3/8/8/8/8/8/8/4K2R w Kq - 5 30")
            == "4k2r/8/8/8/8/8/8/R3K3 b Qk - 5 30");
}

// ── Whole-position color symmetry (issue #125) ────────────────────────────────
//
// Evaluate() is side-to-move-relative: it scores from the perspective of
// whichever color is on move. Mirroring swaps which color is on move along
// with the position, so the mover faces an identical relative situation in
// both the original and the mirror — the scores must be EQUAL, not negated.
// This is the single most likely thing for a future reader to get backwards.
static constexpr const char* kSymmetryFens[] = {
    FEN_START,
    FEN_QUEEN_C6,
    FEN_MIDDLEGAME_ROOKS,
    FEN_ENDGAME_KING_PST,
    FEN_MOPUP_LOSER_KING_CORNER,
    // Covers the WHITE_7TH_ROW = 1 / BLACK_7TH_ROW = 6 pairing, the only
    // direction-aware constant pair otherwise unexercised by a symmetry case.
    FEN_ROOK_ON_7TH,
    // Issue #126 coverage: enemy knight sharing the rook's file (open-file
    // fix) and enemy pawn sharing it (half-open guard) — the two positions
    // most likely to have a color-asymmetric open-file classification.
    FEN_ROOK_OPEN_FILE_KNIGHT_ON,
    FEN_ROOK_OPEN_FILE_PAWN_ON,
};

TEST_CASE("Eval - EvalSimple is color-symmetric: a position and its mirror score equally", "[eval]")
{
    const char* fen = GENERATE(from_range(kSymmetryFens));
    CAPTURE(fen);

    auto eval = EvalManager::Create(EvalManager::EvalTypes::SIMPLE);
    Board board(fen);
    Board mirrored(MirrorFen(fen));

    REQUIRE(eval->Evaluate(board) == eval->Evaluate(mirrored));
}

TEST_CASE("Eval - EvalComplex is color-symmetric: a position and its mirror score equally", "[eval]")
{
    const char* fen = GENERATE(from_range(kSymmetryFens));
    CAPTURE(fen);

    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    Board board(fen);
    Board mirrored(MirrorFen(fen));

    REQUIRE(eval->Evaluate(board) == eval->Evaluate(mirrored));
}
