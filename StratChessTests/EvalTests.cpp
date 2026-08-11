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

#include <algorithm>
#include <limits>
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

// Endgame: White Ke1 + Re7 (rook on 7th rank). Black Kg8.
// Reduced material triggers ENDGAME stage; rook-on-7th bonus should apply.
// White to move. The black king stands off the e-file, so the rook does not
// attack it — legal. No pawns of either colour, so the e-file counts as open.
static constexpr const char* FEN_ROOK_ON_7TH =
    "6k1/4R3/8/8/8/8/8/4K3 w - - 0 1";

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

// D5, controlled pair: rook fixed on e6 in both, White pawn on d4 (off the
// rook's file) vs e4 (on it, behind the rook). The White pawn's PST value is
// identical on d4 and e4 (14), both pawns are isolated, and neither file holds
// an enemy pawn — so if an own pawn behind the rook leaves the file fully OPEN
// (D5 as implemented), these two must score exactly EQUAL. Widening the
// own-pawn test to the whole file would break that equality by 15 cp, which is
// what makes this pair, unlike the >-assertion above, actually pin D5.
static constexpr const char* FEN_ROOK_OWN_PAWN_OFF_FILE =
    "6k1/8/4R3/8/3P4/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_ROOK_OWN_PAWN_BEHIND_SAME_ROOK =
    "6k1/8/4R3/8/4P3/8/8/6K1 w - - 0 1";

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

TEST_CASE("Eval - EvalComplex: a kingless board evaluates to 0 (pre-#127 behaviour, regression)", "[eval]")
{
    // Default-constructed Board has an empty mailbox and zeroed bitboards —
    // no kings, no pieces at all. UciHandler::board_ is exactly this: it is
    // never seeded with the start position, so UCI's `eval` command run
    // before any `position` command evaluates precisely this board (see
    // StratChessTests/UCITests.cpp, "cmd_eval: works before any position
    // command, does not crash").
    //
    // Before the #127 restructure this was well-defined and always 0: the
    // king PST lived inside a loop over ALL_PIECES, which never iterates on
    // an empty board, and the mop-up block returned early on
    // absMatDiff == 0 < MOPUP_MATERIAL_THRESHOLD before ever touching a king
    // square. The restructure's context build initially called
    // Board::GetFirstPiece unconditionally on both king bitboards —
    // GetFirstPiece has an assert(mask != 0) precondition that Debug catches
    // (this test was added because Debug `[uci]` was observed to crash on
    // it) and Release silently violates, indexing g_Eval_Bitboards out of
    // bounds. EvalContext::king_sq is NO_SQUARE for a color with no king
    // (see the comment on that field in Eval.h), and eval_pst/eval_mopup
    // both check for it, restoring the pre-#127 "always 0" result exactly.
    Board board;

    REQUIRE(EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board) == 0);
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

    REQUIRE(board.SetupFromFEN(FEN_WHITE_DOUBLED));
    int doubled_score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

    REQUIRE(board.SetupFromFEN(FEN_WHITE_NORMAL));
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

TEST_CASE("Eval - EvalComplex: an own pawn ahead of the rook blocks the file bonus", "[eval]")
{
    // The half-open test looks only at own pawns AHEAD of the rook
    // (g_bbFileUpMask), so moving the rook from behind its own pawn to in
    // front of it forfeits the file bonus entirely.
    //
    // Note this pair is NOT fully controlled — the rook moves e6 -> e2, which
    // also shifts its PST value by 1 — so it can only assert a direction. The
    // equality test below is what actually pins D5.
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board pawnBehind(FEN_ROOK_OWN_PAWN_BEHIND);
    Board pawnAhead(FEN_ROOK_OWN_PAWN_AHEAD);

    REQUIRE(eval->Evaluate(pawnBehind) > eval->Evaluate(pawnAhead));
}

// ── Color-mirroring correctness (issue #125) ──────────────────────────────────
//
// Exposes EvalManager's protected mirroring/PST helpers for direct testing —
// no production visibility change; both stay protected on EvalManager. Also
// used by the term-level tests below (issue #127 restructure) to compute an
// independently-derived expected PST value.
struct EvalProbe final : EvalManager
{
    int Evaluate(const Board&) const override { return 0; }
    const char* GetType() const override { return "Probe"; }
    using EvalManager::getEvalBoard;
    using EvalManager::GetPositionalScore;
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
    // case swap, and its en-passant field is "-". Evaluate() reads the castling
    // field (eval_castling, issue #115) but not the en-passant one, so a broken
    // MirrorCastling would silently make the symmetry cases compare two
    // differently-scored positions -- this self-test is the only thing that
    // catches either helper going wrong.
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

// Castling asymmetry (issue #115). Neither side has rights left; White's king
// is tucked on g1 while Black's sits on e8, so eval_castling pays White and
// penalises Black. Mirroring swaps the position and the side to move together,
// so the score stays EQUAL as above; what this case pins is that the term's
// direction-aware WHITE_BACK_ROW / BLACK_BACK_ROW pairing is the right way
// round, since getting it backwards makes the two sides disagree and breaks
// that equality. Legal: Rf1 covers the f-file only, so Black (the non-mover)
// is not in check.
static constexpr const char* FEN_CASTLED_VS_CENTRAL_KING =
    "4k3/8/8/8/8/8/8/5RK1 w - - 0 1";

// Bishop-pair asymmetry (issue #111). White holds bishops on both square
// colours (c1 dark, f1 light); Black has a single bishop on g7 — so the pair
// bonus applies to exactly one side, which is what makes the mirror
// discriminating. Legal: neither king is attacked.
static constexpr const char* FEN_BISHOP_PAIR_VS_SINGLE =
    "4k3/6b1/8/8/8/8/8/2B1KB2 w - - 0 1";

// Passed pawns on OPPOSITE EDGE FILES, at deliberately different advancement
// (issue #116). Both properties matter: opposite edges mean a mask that wrapped
// around the a/h boundary shows up in a whole-position score rather than only in
// the mask unit tests, and unequal advancement means a broken per-colour
// `advanced` index cannot cancel between the two sides. Legal: kings on e8/e1,
// neither attacked.
static constexpr const char* FEN_EDGE_FILE_PASSERS =
    "4k3/8/8/p7/8/7P/8/4K3 w - - 0 1";

// Blockaded passer (issue #116). White's e7 pawn has the black king squarely on
// its stop square, so the blockade discount applies to exactly one side. This is
// the only place `eval_pawns` reads a non-pawn bitboard (`ctx.occupied[enemy]`),
// and it is per-colour, so without this the mirror battery never exercises it.
// Legal: the kings are not adjacent and neither is in check.
static constexpr const char* FEN_BLOCKADED_PASSER =
    "4k3/4P3/8/8/8/8/8/4K3 w - - 0 1";

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
    // The two new terms' direction-aware cases. Every other FEN here either
    // keeps its castling rights (eval_castling stays silent) or has no bishop
    // pair, so without these two neither term is discriminated by a mirror.
    FEN_CASTLED_VS_CENTRAL_KING,
    FEN_BISHOP_PAIR_VS_SINGLE,
    // Issue #116: the passer term is direction-aware through a new mask pair and
    // a per-colour rank index, and no other FEN here puts a passer on an edge
    // file, which is where a wraparound asymmetry would hide.
    FEN_EDGE_FILE_PASSERS,
    FEN_BLOCKADED_PASSER,
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

// ── Term-level tests (issue #127 restructure) ─────────────────────────────────
//
// EvalComplex::Evaluate() is now a thin context-build-and-sum wrapper around
// four private per-term functions (eval_pawns, eval_rooks, eval_pst,
// eval_mopup), each taking (const EvalContext&, eColor) and returning that
// color's contribution only — see .claude/plans/eval-context-restructure.md.
// The term accessors return each term BLENDED at the position's own phase
// (issue #99) — the value that term actually contributes to Evaluate() there.
// Endpoint behaviour (mg vs eg) is asserted separately by the tapering tests,
// which drive phase directly rather than inferring it.
//
// EvalComplexTestFixture is a friend of EvalComplex (STRAT_ENABLE_TEST_ACCESS,
// same mechanism as AIPerplex/UciHandler's fixtures) that builds an
// EvalContext from a Board and forwards to each term, so terms can be
// asserted on directly instead of only inferred from whole-position deltas.
struct EvalComplexTestFixture
{
    static int Pawns(const Board& board, eColor color)
    {
        const EvalContext ctx = BuildContext(board);
        return BlendPhase(EvalComplex::eval_pawns(ctx, color), ctx.phase);
    }
    static int Rooks(const Board& board, eColor color)
    {
        const EvalContext ctx = BuildContext(board);
        return BlendPhase(EvalComplex::eval_rooks(ctx, color), ctx.phase);
    }
    static int Pst(const Board& board, eColor color)
    {
        const EvalContext ctx = BuildContext(board);
        return BlendPhase(EvalComplex::eval_pst(ctx, color), ctx.phase);
    }
    static int Mopup(const Board& board, eColor color)
    {
        const EvalContext ctx = BuildContext(board);
        return BlendPhase(EvalComplex::eval_mopup(ctx, color), ctx.phase);
    }
    static int Bishops(const Board& board, eColor color)
    {
        const EvalContext ctx = BuildContext(board);
        return BlendPhase(EvalComplex::eval_bishops(ctx, color), ctx.phase);
    }
    static int Mobility(const Board& board, eColor color)
    {
        const EvalContext ctx = BuildContext(board);
        return BlendPhase(EvalComplex::eval_mobility(ctx, color), ctx.phase);
    }
    static int Castling(const Board& board, eColor color)
    {
        const EvalContext ctx = BuildContext(board);
        return BlendPhase(EvalComplex::eval_castling(ctx, color), ctx.phase);
    }

    static ScorePair BishopsPair(const Board& board, eColor color)
    {
        return EvalComplex::eval_bishops(BuildContext(board), color);
    }
    static ScorePair CastlingPair(const Board& board, eColor color)
    {
        return EvalComplex::eval_castling(BuildContext(board), color);
    }

    static ScorePair MobilityPair(const Board& board, eColor color)
    {
        return EvalComplex::eval_mobility(BuildContext(board), color);
    }

    // Raw (mg, eg) pairs, for asserting a tapered term's ENDPOINTS rather
    // than its value at one position's particular phase.
    static ScorePair RooksPair(const Board& board, eColor color)
    {
        return EvalComplex::eval_rooks(BuildContext(board), color);
    }
    static ScorePair PstPair(const Board& board, eColor color)
    {
        return EvalComplex::eval_pst(BuildContext(board), color);
    }
    static int Phase(const Board& board)
    {
        return BuildContext(board).phase;
    }

    // Named constants, exposed so term-level tests can express exact
    // expected values without duplicating magic numbers.
    static int DoubledPawnPenalty()  { return EvalComplex::DOUBLED_PAWN_PENALTY; }
    static int IsolatedPawnPenalty() { return EvalComplex::ISOLATED_PAWN_PENALTY; }
    static int RookOn7thBonus()      { return EvalComplex::ROOK_ON_7TH_BONUS; }
    static int OpenFile()            { return EvalComplex::OPEN_FILE; }
    static int HalfOpenFile()        { return EvalComplex::HALF_OPEN_FILE; }

private:
    // Forwards to EvalComplex::BuildContext — the production construction
    // site (Eval.cpp) — rather than reimplementing it here. Issue #99 will
    // eventually replace the `11500` phase threshold BuildContext uses
    // internally; having only one construction site means that change can't
    // leave this fixture silently testing the old threshold.
    static EvalContext BuildContext(const Board& board)
    {
        return EvalComplex::BuildContext(board);
    }
};

TEST_CASE("Eval - eval_pawns: pawns with no isolation and no doubling score exactly 0", "[eval]")
{
    Board board(FEN_WHITE_NORMAL);

    REQUIRE(EvalComplexTestFixture::Pawns(board, WHITE) == 0);
}

TEST_CASE("Eval - eval_rooks: an enemy knight on the rook's file does not demote an open file", "[eval]")
{
    // Issue #126's discriminator: "open file" must test for absence of enemy
    // PAWNS, not of any enemy piece. Before the fix, a knight sharing the rook's
    // file wrongly demoted it from open to half-open (-5 cp). The knight's PST
    // value is identical on d5 and e5 and material is identical, so the file
    // classification is the only thing that can make the ROOK term differ.
    //
    // Asserted term-level rather than on whole-position Evaluate(): moving the
    // knight legitimately changes mobility (#98), so the totals differ even
    // though the open-file classification does not. The term-level assertion is
    // what this test always meant.
    Board knightOn(FEN_ROOK_OPEN_FILE_KNIGHT_ON);
    Board knightOff(FEN_ROOK_OPEN_FILE_KNIGHT_OFF);

    REQUIRE(EvalComplexTestFixture::Rooks(knightOn, WHITE)
            == EvalComplexTestFixture::Rooks(knightOff, WHITE));
}

TEST_CASE("Eval - eval_rooks: an own pawn behind the rook leaves the file fully open (D5)", "[eval]")
{
    // Pins the deliberate D5 decision from
    // .claude/plans/passed-and-backwards-pawn-terms.md. The rook is fixed on
    // e6 in both positions; only the White pawn moves, from d4 (off the file)
    // to e4 (on the file, behind the rook). Its PST value is identical on both
    // squares and it is isolated either way, so the file classification is the
    // only thing that could make the ROOK term differ.
    //
    // Equality is the whole point: a ">" assertion would still pass if the
    // pawn-behind case were demoted to merely half-open. Only exact equality
    // proves the file is still scored as fully OPEN.
    //
    // Asserted on eval_rooks rather than on whole-position Evaluate(): moving a
    // pawn legitimately changes mobility (#98), so the two positions' total
    // scores are no longer equal even though the rook term is. Comparing totals
    // to prove a claim about one term was over-coupling that a later term was
    // always going to break.
    Board pawnOffFile(FEN_ROOK_OWN_PAWN_OFF_FILE);
    Board pawnBehindRook(FEN_ROOK_OWN_PAWN_BEHIND_SAME_ROOK);

    REQUIRE(EvalComplexTestFixture::Rooks(pawnOffFile, WHITE)
            == EvalComplexTestFixture::Rooks(pawnBehindRook, WHITE));
}

// ── eval_mobility (issues #98, #113) ─────────────────────────────────────────

TEST_CASE("Eval - eval_mobility: a central knight outscores a cornered one", "[eval]")
{
    // The canonical mobility case: a knight on d4 reaches 8 squares, one on a1
    // reaches 2. Nothing else differs between the positions.
    Board central("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    Board cornered("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");

    REQUIRE(EvalComplexTestFixture::Mobility(central, WHITE)
            > EvalComplexTestFixture::Mobility(cornered, WHITE));
}

TEST_CASE("Eval - eval_mobility: a rook on an open file outscores one boxed in behind its own pawns", "[eval]")
{
    Board open("4k3/8/8/8/8/8/8/3RK3 w - - 0 1");
    Board boxed("4k3/8/8/8/8/8/3PPP2/3RK3 w - - 0 1");

    REQUIRE(EvalComplexTestFixture::Mobility(open, WHITE)
            > EvalComplexTestFixture::Mobility(boxed, WHITE));
}

TEST_CASE("Eval - eval_mobility: squares covered by an enemy pawn do not count (safe mobility)", "[eval]")
{
    // Same White knight on d4 in both. A knight on d4 reaches b3, b5, c2, c6,
    // e2, e6, f3 and f5; the Black pawn on d7 covers c6 and e6, two of them.
    // The pawn blocks nothing directly -- a knight jumps -- so the only thing
    // that can change the count is the safe-mobility mask.
    //
    // A pawn on d6 would prove nothing: it covers c5 and e5, and a knight on d4
    // reaches neither.
    Board unguarded("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    Board guarded("4k3/3p4/8/8/3N4/8/8/4K3 w - - 0 1");

    REQUIRE(EvalComplexTestFixture::Mobility(guarded, WHITE)
            < EvalComplexTestFixture::Mobility(unguarded, WHITE));
}

TEST_CASE("Eval - eval_mobility: the safe-mobility mask is colour-symmetric", "[eval]")
{
    // The case above asserts WHITE's mobility against a BLACK pawn, so it only
    // ever exercises the white pawn-attack shifts. This mirrors it, which is the
    // only thing that touches the black `<<9`/`<<7` expression in BuildContext.
    //
    // Worth its own case because the failure is silent: a transposed shift or a
    // swapped file mask would compile, pass every other test, and quietly cost
    // Black a few squares per node in every game of a 20,000-game run. Issue
    // #125 was this exact class of defect.
    //
    // The pawn must be on an EDGE FILE. A central pawn cannot discriminate:
    // both file masks pass it through, so `p<<9 | p<<7` is the same set however
    // the two shifts are ordered, and a transposition survives the test. The
    // masks exist only to stop an a- or h-file pawn wrapping around the board,
    // so only an a- or h-file pawn tests them.
    //
    // Black pawn a7 covers b6 and nothing else (the other diagonal would wrap).
    // The knight on d5 reaches b6, so the count drops from 8 to 7. Under
    // transposed shifts the pawn's attack lands on h7 instead, the knight
    // reaches none of it, and the count stays 8.
    Board whiteSide("4k3/p7/8/3N4/8/8/8/4K3 w - - 0 1");
    Board blackSide("4k3/8/8/8/3n4/8/P7/4K3 b - - 0 1");

    REQUIRE(EvalComplexTestFixture::Mobility(whiteSide, WHITE)
            == EvalComplexTestFixture::Mobility(blackSide, BLACK));

    // ...and the mask must actually be biting, or the equality above is vacuous.
    Board whiteUnmasked("4k3/8/8/3N4/8/8/8/4K3 w - - 0 1");
    Board blackUnmasked("4k3/8/8/8/3n4/8/8/4K3 b - - 0 1");
    REQUIRE(EvalComplexTestFixture::Mobility(whiteSide, WHITE)
            < EvalComplexTestFixture::Mobility(whiteUnmasked, WHITE));
    REQUIRE(EvalComplexTestFixture::Mobility(blackSide, BLACK)
            < EvalComplexTestFixture::Mobility(blackUnmasked, BLACK));
}

TEST_CASE("Eval - eval_mobility: own pieces block, enemy pieces are capture targets", "[eval]")
{
    // Pins the D3 convention: `attacks & ~occupied[own]`, so an enemy piece on a
    // reachable square still counts (it can be captured) while an own piece does
    // not. A bishop on c1 with the b2 square occupied either way isolates it.
    Board ownBlocker("4k3/8/8/8/8/8/1P6/2B1K3 w - - 0 1");
    Board enemyBlocker("4k3/8/8/8/8/8/1p6/2B1K3 w - - 0 1");

    REQUIRE(EvalComplexTestFixture::Mobility(enemyBlocker, WHITE)
            > EvalComplexTestFixture::Mobility(ownBlocker, WHITE));
}

TEST_CASE("Eval - eval_mobility: the queen is scored, not skipped (issue #113)", "[eval]")
{
    // #113 exists so the queen is not left out if mobility scopes down to cheap
    // pieces. A lone queen must produce a nonzero term.
    Board queen("4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1");

    REQUIRE(EvalComplexTestFixture::Mobility(queen, WHITE) > 0);
}

TEST_CASE("Eval - eval_mobility: a bare king contributes nothing", "[eval]")
{
    // The king is deliberately excluded -- king mobility belongs to #97, where
    // it can be weighed against attacker counts rather than paid per square.
    Board kings("4k3/8/8/8/8/8/8/4K3 w - - 0 1");

    REQUIRE(EvalComplexTestFixture::Mobility(kings, WHITE) == 0);
    REQUIRE(EvalComplexTestFixture::Mobility(kings, BLACK) == 0);
}

TEST_CASE("Eval - eval_pawns: doubled and isolated a-file pawns score exactly -(doubled + 2*isolated)", "[eval]")
{
    // FEN_WHITE_DOUBLED: White Pa2 + Pa3, no b-file pawn. Both pawns are
    // isolated (the only neighbouring file, b, has no White pawn); the lower
    // pawn (a2) additionally has a3 in its forward mask, so exactly one
    // doubled penalty applies — only the pawn with another pawn strictly
    // ahead of it on the same file triggers that check.
    Board board(FEN_WHITE_DOUBLED);

    const int expected = -(EvalComplexTestFixture::DoubledPawnPenalty()
                          + 2 * EvalComplexTestFixture::IsolatedPawnPenalty());
    REQUIRE(EvalComplexTestFixture::Pawns(board, WHITE) == expected);
}

TEST_CASE("Eval - eval_rooks: the 7th-rank bonus is endgame-weighted, the file bonus is not", "[eval]")
{
    // FEN_ROOK_ON_7TH: White Re7 alone against a bare king. The fully open
    // file (no pawns of either colour) is phase-independent and so appears at
    // both endpoints; the 7th-rank bonus is endgame-weighted (D3, issue #99)
    // and so appears only at eg. Asserting the endpoints rather than the
    // blended value keeps this independent of the position's own phase.
    Board board(FEN_ROOK_ON_7TH);

    const ScorePair rooks = EvalComplexTestFixture::RooksPair(board, WHITE);

    REQUIRE(rooks.mg == EvalComplexTestFixture::OpenFile());
    REQUIRE(rooks.eg == EvalComplexTestFixture::OpenFile() + EvalComplexTestFixture::RookOn7thBonus());
}

TEST_CASE("Eval - eval_rooks: term-level result matches the #126 open-file guard exactly", "[eval]")
{
    // Re-runs the issue #126 open-file case (an enemy knight sharing the
    // file must not demote it) directly against the extracted term, not just
    // through the whole-position score — pins the term itself, not merely
    // its net effect once summed with unrelated PST noise.
    //
    // Asserting an exact magnitude, not merely that the two positions score
    // equally: eval_rooks reads nothing but pawn bitboards (see its
    // implementation), so the knightOn/knightOff pair differs only in a
    // piece the term provably never looks at — an equality assertion would
    // still pass even if eval_rooks ignored the file classification entirely
    // and returned a constant. Both positions have no pawns of either colour
    // on the board at all, so the file is fully open regardless of the
    // knight, and there is no rank-7 bonus (the rook sits on e1): the full
    // open-file total, half-open plus the extra open-file increment.
    Board knightOn(FEN_ROOK_OPEN_FILE_KNIGHT_ON);
    Board knightOff(FEN_ROOK_OPEN_FILE_KNIGHT_OFF);

    const int expected = EvalComplexTestFixture::HalfOpenFile()
                        + (EvalComplexTestFixture::OpenFile() - EvalComplexTestFixture::HalfOpenFile());
    REQUIRE(EvalComplexTestFixture::Rooks(knightOn, WHITE) == expected);
    REQUIRE(EvalComplexTestFixture::Rooks(knightOff, WHITE) == expected);
}

TEST_CASE("Eval - eval_rooks: term-level D5 own-pawn-behind result is exactly equal, not just directionally so", "[eval]")
{
    // Same pair as the whole-position D5 test above, asserted directly on
    // the extracted term rather than inferred through the full evaluation.
    //
    // Asserting an exact magnitude rather than pairwise equality, for the
    // same reason as the knight-file test above: the two FENs differ only in
    // where the lone White pawn sits (d4 vs e4), and eval_rooks's own-pawn
    // check is what's under test — an equality-only assertion would not
    // catch eval_rooks degenerating to a constant. Rook e6 is behind its own
    // pawn (or off its file) with no enemy pawns anywhere and is not on the
    // 7th rank, so the file is fully open: plain OPEN_FILE, no 7th-rank bonus.
    Board pawnOffFile(FEN_ROOK_OWN_PAWN_OFF_FILE);
    Board pawnBehindRook(FEN_ROOK_OWN_PAWN_BEHIND_SAME_ROOK);

    const int expected = EvalComplexTestFixture::OpenFile();
    REQUIRE(EvalComplexTestFixture::Rooks(pawnOffFile, WHITE) == expected);
    REQUIRE(EvalComplexTestFixture::Rooks(pawnBehindRook, WHITE) == expected);
}

TEST_CASE("Eval - eval_pst: the king's two PST endpoints are the two king tables", "[eval]")
{
    // Kings sit on e1/e8, outside every queen's line of attack (manually
    // verified: no queen shares a rank, file, or diagonal with either king),
    // so the position is legal despite the unusual material.
    //
    // Post-#99 the king is no longer assigned one table by a material
    // threshold: g_Eval_Bitboards[5] and [6] are its mg and eg endpoints. Each
    // endpoint must equal the independently-computed non-king PST sum (via
    // EvalProbe::GetPositionalScore, a different call path than eval_pst's own
    // per-type loops) plus exactly one lookup into the corresponding table.
    Board board("q3k2q/8/8/8/8/8/8/Q3K2Q w - - 0 1");

    const int expectedNonKing =
        EvalProbe::GetPositionalScore(a1, WHITE_QUEEN) + EvalProbe::GetPositionalScore(h1, WHITE_QUEEN);
    const int kingMg = g_Eval_Bitboards[5][EvalProbe::getEvalBoard(WHITE_KING, e1)];
    const int kingEg = g_Eval_Bitboards[6][EvalProbe::getEvalBoard(WHITE_KING, e1)];

    const ScorePair pst = EvalComplexTestFixture::PstPair(board, WHITE);

    REQUIRE(pst.mg == expectedNonKing + kingMg);
    REQUIRE(pst.eg == expectedNonKing + kingEg);
    // The two tables genuinely disagree here, so the blend below is not a
    // no-op masquerading as one.
    REQUIRE(kingMg != kingEg);
}

TEST_CASE("Eval - eval_pst: the reported king contribution is its endpoints blended at the position phase", "[eval]")
{
    // Ties the term's value to its own endpoints and the position's own phase,
    // so a future change to either the tables or the phase weights cannot
    // leave the blend silently inconsistent with them.
    Board board("q3k2q/8/8/8/8/8/8/Q3K2Q w - - 0 1");

    const ScorePair pst = EvalComplexTestFixture::PstPair(board, WHITE);
    const int phase = EvalComplexTestFixture::Phase(board);
    CAPTURE(pst.mg, pst.eg, phase);

    // Two queens a side and nothing else: 4 + 4 + 4 + 4 = 16.
    REQUIRE(phase == 16);
    REQUIRE(EvalComplexTestFixture::Pst(board, WHITE) == BlendPhase(pst, phase));
}

// ── eval_bishops (issue #111) ────────────────────────────────────────────────

TEST_CASE("Eval - eval_bishops: pair requires opposite square colours", "[eval]")
{
    SECTION("Two bishops on opposite colours score the pair")
    {
        // White Bc1 (dark) + Bf1 (light); Black has one bishop only.
        Board board("4k3/8/8/8/8/8/8/2B1KB2 w - - 0 1");
        const ScorePair pair = EvalComplexTestFixture::BishopsPair(board, WHITE);
        CHECK(pair.mg > 0);
        CHECK(pair.eg > pair.mg);   // worth more as the board opens
    }
    SECTION("Two bishops on the SAME colour are not a pair")
    {
        // Bc1 and Ba3 are both dark squares — reachable by underpromotion.
        // A plain popcount >= 2 would wrongly pay here.
        Board board("4k3/8/8/8/8/B7/8/2B1K3 w - - 0 1");
        const ScorePair pair = EvalComplexTestFixture::BishopsPair(board, WHITE);
        CHECK(pair.mg == 0);
        CHECK(pair.eg == 0);
    }
    SECTION("A single bishop scores nothing")
    {
        Board board("4k3/8/8/8/8/8/8/2B1K3 w - - 0 1");
        CHECK(EvalComplexTestFixture::Bishops(board, WHITE) == 0);
    }
    SECTION("Kingless board is safe and scores nothing")
    {
        Board board;
        CHECK(EvalComplexTestFixture::Bishops(board, WHITE) == 0);
        CHECK(EvalComplexTestFixture::Bishops(board, BLACK) == 0);
    }
}

// ── connected rooks (issue #114, inside eval_rooks) ──────────────────────────

TEST_CASE("Eval - eval_rooks: connected rooks require a clear line between them", "[eval]")
{
    // Same back rank, nothing between, versus the same two rooks with a piece
    // wedged between them. Comparing the two isolates the connection bonus from
    // the open-file and 7th-rank bonuses, which are identical in both.
    Board connected("4k3/8/8/8/8/8/8/R2R3K w - - 0 1");
    Board blocked("4k3/8/8/8/8/8/8/R1BR3K w - - 0 1");

    const int connectedMg = EvalComplexTestFixture::RooksPair(connected, WHITE).mg;
    const int blockedMg   = EvalComplexTestFixture::RooksPair(blocked, WHITE).mg;

    CHECK(connectedMg > blockedMg);
}

TEST_CASE("Eval - eval_rooks: a lone rook is never connected", "[eval]")
{
    Board one("4k3/8/8/8/8/8/8/R6K w - - 0 1");
    Board two("4k3/8/8/8/8/8/8/R2R3K w - - 0 1");

    CHECK(EvalComplexTestFixture::RooksPair(two, WHITE).mg
        > EvalComplexTestFixture::RooksPair(one, WHITE).mg);
}

TEST_CASE("Eval - eval_rooks: connected pairs are counted exactly, not doubled", "[eval]")
{
    // The inequality tests above would also pass if every pair were counted
    // twice, so pin the arithmetic. Three collinear rooks on an otherwise empty
    // rank 1: a1-d1 and d1-h1 are connected, a1-h1 is blocked by the d1 rook,
    // so this is 2 pairs and not 3 -- three mutually-connected rooks are in fact
    // geometrically impossible, since pairwise alignment forces collinearity and
    // then the middle rook blocks the outer pair.
    //
    // Pawnless, so all three files are open: 3 * OPEN_FILE(15) = 45, plus
    // 2 * CONNECTED_ROOKS_BONUS_MG(15) = 30, giving mg 75. The endgame endpoint
    // takes CONNECTED_ROOKS_BONUS_EG(8) instead: 45 + 16 = 61.
    Board board("4k3/8/8/8/4K3/8/8/R2R3R w - - 0 1");
    const ScorePair rooks = EvalComplexTestFixture::RooksPair(board, WHITE);

    CHECK(rooks.mg == 75);
    CHECK(rooks.eg == 61);
}

// ── eval_castling (issue #115) ───────────────────────────────────────────────

TEST_CASE("Eval - eval_castling: silent while any castling right remains", "[eval]")
{
    // Rights present => the side has decided nothing, so no bonus and no penalty.
    Board board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    CHECK(EvalComplexTestFixture::CastlingPair(board, WHITE).mg == 0);
    CHECK(EvalComplexTestFixture::CastlingPair(board, BLACK).mg == 0);
}

TEST_CASE("Eval - eval_castling: bonus once rights are gone and the king is tucked away", "[eval]")
{
    // White king on g1 with no rights left — the castled-kingside picture.
    Board board("4k3/8/8/8/8/8/8/5RK1 w - - 0 1");
    const ScorePair pair = EvalComplexTestFixture::CastlingPair(board, WHITE);
    CHECK(pair.mg > 0);
    CHECK(pair.eg == 0);   // middlegame-only; the endgame king wants the centre
}

TEST_CASE("Eval - eval_castling: penalty when rights are lost with the king still central", "[eval]")
{
    // King on e1, no rights — lost the option without ever castling.
    Board board("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(EvalComplexTestFixture::CastlingPair(board, WHITE).mg < 0);
}

TEST_CASE("Eval - eval_castling: both corners count, not just the kingside one", "[eval]")
{
    // a1 is the queenside analogue of h1. An earlier form tested
    // `file >= 1 && file <= 2` for the queenside, which covered b1 and c1 but
    // silently excluded a1 -- so Kh1 scored the bonus while its mirror Ka1 took
    // the penalty, a 45 cp mg swing on a square that Kb1-a1 reaches routinely in
    // opposite-side-castling Sicilians.
    for (const char* fen : { "4k3/8/8/8/8/8/8/K7 w - - 0 1",      // a1, queenside corner
                             "4k3/8/8/8/8/8/8/1K6 w - - 0 1",     // b1
                             "4k3/8/8/8/8/8/8/2K5 w - - 0 1",     // c1
                             "4k3/8/8/8/8/8/8/6K1 w - - 0 1",     // g1
                             "4k3/8/8/8/8/8/8/7K w - - 0 1" }) {  // h1, kingside corner
        CAPTURE(fen);
        Board board(fen);
        CHECK(EvalComplexTestFixture::CastlingPair(board, WHITE).mg > 0);
    }
}

TEST_CASE("Eval - eval_castling: the two corners score identically", "[eval]")
{
    Board kingside("4k3/8/8/8/8/8/8/7K w - - 0 1");
    Board queenside("4k3/8/8/8/8/8/8/K7 w - - 0 1");

    CHECK(EvalComplexTestFixture::CastlingPair(kingside, WHITE).mg
       == EvalComplexTestFixture::CastlingPair(queenside, WHITE).mg);
}

TEST_CASE("Eval - eval_castling: the f-file is neutral, softening the one-step cliff", "[eval]")
{
    // A binary sheltered/exposed test made Kg1->Kf1 swing the full bonus-to-
    // penalty distance on one quiet king move, with nothing to smooth it -- the
    // middlegame king PST is rank-only, so this term is the only file signal
    // there. f scores zero instead, halving the worst single-step swing.
    Board f1("4k3/8/8/8/8/8/8/5K2 w - - 0 1");
    Board g1("4k3/8/8/8/8/8/8/6K1 w - - 0 1");
    Board e1("4k3/8/8/8/8/8/8/4K3 w - - 0 1");

    CHECK(EvalComplexTestFixture::CastlingPair(f1, WHITE).mg == 0);
    CHECK(EvalComplexTestFixture::CastlingPair(g1, WHITE).mg > 0);
    CHECK(EvalComplexTestFixture::CastlingPair(e1, WHITE).mg < 0);
}

TEST_CASE("Eval - eval_castling: a king off its home rank never counts as castled", "[eval]")
{
    // g2, not g1: the king has walked, so the shelter picture does not apply.
    Board board("4k3/8/8/8/8/8/6K1/8 w - - 0 1");
    CHECK(EvalComplexTestFixture::CastlingPair(board, WHITE).mg < 0);
}

TEST_CASE("Eval - eval_castling: kingless board is safe", "[eval]")
{
    // Default-constructed Board has no king; castling_rights defaults to ALL,
    // so this also exercises the rights-present early return.
    Board board;
    CHECK(EvalComplexTestFixture::Castling(board, WHITE) == 0);
    CHECK(EvalComplexTestFixture::Castling(board, BLACK) == 0);
}

TEST_CASE("Eval - eval_mopup: only the winning color receives a nonzero contribution", "[eval]")
{
    // FEN_MOPUP_LOSER_KING_CORNER: White K+Q vs Black K+R, pawnless, White
    // winning by 400 cp (the decisive-material threshold).
    Board board(FEN_MOPUP_LOSER_KING_CORNER);

    REQUIRE(EvalComplexTestFixture::Mopup(board, WHITE) > 0);
    REQUIRE(EvalComplexTestFixture::Mopup(board, BLACK) == 0);
}

TEST_CASE("Eval - eval_mopup: gated off for both colors below the decisive material threshold", "[eval]")
{
    // FEN_MOPUP_MARGINAL_CORNER: K+N vs K+B, materially equal — below the
    // 400 cp threshold, so neither color should get a contribution.
    Board board(FEN_MOPUP_MARGINAL_CORNER);

    REQUIRE(EvalComplexTestFixture::Mopup(board, WHITE) == 0);
    REQUIRE(EvalComplexTestFixture::Mopup(board, BLACK) == 0);
}

TEST_CASE("Eval - the per-term functions sum exactly to EvalComplex::Evaluate()'s result", "[eval]")
{
    // Structural regression check: rebuilds Evaluate()'s side-to-move-relative
    // formula from the independently-tested terms plus raw material, and
    // confirms it matches Evaluate() itself across every whole-position FEN
    // already used for the color-symmetry cases above. Guards against the
    // term wrappers drifting out of sync with what Evaluate() actually calls.
    const char* fen = GENERATE(from_range(kSymmetryFens));
    CAPTURE(fen);

    Board board(fen);
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    const int matWhite = board.GetMaterialScore(WHITE);
    const int matBlack = board.GetMaterialScore(BLACK);

    const int bonusWhite = EvalComplexTestFixture::Pawns(board, WHITE)
                          + EvalComplexTestFixture::Rooks(board, WHITE)
                          + EvalComplexTestFixture::Pst(board, WHITE)
                          + EvalComplexTestFixture::Mopup(board, WHITE)
                          + EvalComplexTestFixture::Bishops(board, WHITE)
                          + EvalComplexTestFixture::Castling(board, WHITE)
                          + EvalComplexTestFixture::Mobility(board, WHITE);
    const int bonusBlack = EvalComplexTestFixture::Pawns(board, BLACK)
                          + EvalComplexTestFixture::Rooks(board, BLACK)
                          + EvalComplexTestFixture::Pst(board, BLACK)
                          + EvalComplexTestFixture::Mopup(board, BLACK)
                          + EvalComplexTestFixture::Bishops(board, BLACK)
                          + EvalComplexTestFixture::Castling(board, BLACK)
                          + EvalComplexTestFixture::Mobility(board, BLACK);

    const eColor toMove = board.GetCurrentColor();
    const int expected = (toMove == WHITE)
        ? (matWhite + bonusWhite) - (matBlack + bonusBlack)
        : (matBlack + bonusBlack) - (matWhite + bonusWhite);

    REQUIRE(eval->Evaluate(board) == expected);
}

// ── EvalComplex::Breakdown() (issue #129 phase 2) ─────────────────────────────
//
// Breakdown() is the public, production path to the per-term values that the
// UCI 'eval' command prints (.claude/plans/uci-eval-command-term-breakdown.md,
// D7). The tests below tie it to the terms that are already individually
// asserted above, rather than testing it in isolation — the failure mode worth
// guarding is Breakdown() quietly reporting something other than what
// Evaluate() sums, which no amount of self-consistent output would reveal.

TEST_CASE("Eval - Breakdown(): every row equals the term function it reports", "[eval]")
{
    const char* fen = GENERATE(from_range(kSymmetryFens));
    CAPTURE(fen);

    Board board(fen);
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    auto* complexEval = dynamic_cast<EvalComplex*>(eval.get());
    REQUIRE(complexEval != nullptr);

    const EvalBreakdown terms = complexEval->Breakdown(board);

    for (const eColor color : { WHITE, BLACK }) {
        CAPTURE(static_cast<int>(color));
        REQUIRE(terms.material[color] == board.GetMaterialScore(color));
        REQUIRE(terms.pawns[color]    == EvalComplexTestFixture::Pawns(board, color));
        REQUIRE(terms.rooks[color]    == EvalComplexTestFixture::Rooks(board, color));
        REQUIRE(terms.pst[color]      == EvalComplexTestFixture::Pst(board, color));
        REQUIRE(terms.mopup[color]    == EvalComplexTestFixture::Mopup(board, color));
        REQUIRE(terms.bishops[color]  == EvalComplexTestFixture::Bishops(board, color));
        REQUIRE(terms.castling[color] == EvalComplexTestFixture::Castling(board, color));
    }
}

TEST_CASE("Eval - Breakdown(): total agrees with Evaluate(), and the rows reproduce it", "[eval]")
{
    // Two assertions. `total` equals Evaluate()'s result, and the rows account
    // for that total exactly: material plus every term, summed
    // white-minus-black, up to the side-to-move sign. The second is what makes
    // the printed net column trustworthy.
    //
    // Note what this does *not* establish. D8 says `total` is Evaluate()'s own
    // return value rather than a re-derivation of its sign flip — that is a
    // structural property of Breakdown()'s implementation, and no black-box
    // assertion can distinguish it from a re-derivation that happens to be
    // correct. It is enforced by the code and by review, not here. What these
    // assertions do catch is a re-derivation that is *wrong*, which is the
    // failure that would actually mislead someone reading the output.
    const char* fen = GENERATE(from_range(kSymmetryFens));
    CAPTURE(fen);

    Board board(fen);
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    auto* complexEval = dynamic_cast<EvalComplex*>(eval.get());
    REQUIRE(complexEval != nullptr);

    const EvalBreakdown terms = complexEval->Breakdown(board);

    REQUIRE(terms.total == eval->Evaluate(board));

    const int whitePov = (terms.material[WHITE] - terms.material[BLACK])
                       + (terms.pawns[WHITE]    - terms.pawns[BLACK])
                       + (terms.rooks[WHITE]    - terms.rooks[BLACK])
                       + (terms.pst[WHITE]      - terms.pst[BLACK])
                       + (terms.mopup[WHITE]    - terms.mopup[BLACK])
                       + (terms.bishops[WHITE]  - terms.bishops[BLACK])
                       + (terms.castling[WHITE] - terms.castling[BLACK])
                       + (terms.mobility[WHITE] - terms.mobility[BLACK]);

    const int expectedTotal = (board.GetCurrentColor() == WHITE) ? whitePov : -whitePov;
    REQUIRE(terms.total == expectedTotal);
}

TEST_CASE("Eval - Breakdown(): phase matches the context Evaluate() builds", "[eval]")
{
    // Phase is reported because it is not derivable from the rows, and it is
    // what places every tapered term between its mg and eg endpoints. Asserting
    // it against Breakdown() keeps the reported value pinned to the one
    // construction site (BuildContext) rather than to a duplicated formula.
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    auto* complexEval = dynamic_cast<EvalComplex*>(eval.get());
    REQUIRE(complexEval != nullptr);

    SECTION("full starting material is MAX_GAME_PHASE") {
        Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        REQUIRE(complexEval->Breakdown(board).phase == MAX_GAME_PHASE);
    }

    SECTION("bare kings plus a queen is the queen's weight alone") {
        Board board("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
        REQUIRE(complexEval->Breakdown(board).phase == 4);
    }

    SECTION("bare kings are phase 0") {
        Board board("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
        REQUIRE(complexEval->Breakdown(board).phase == 0);
    }

    SECTION("pawns do not contribute to phase") {
        // Same pieces as the bare-kings case plus a full pawn set: phase must
        // not move, or the taper would drift with pawn trades rather than with
        // the piece material that actually defines the game's stage.
        Board board("4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - - 0 1");
        REQUIRE(complexEval->Breakdown(board).phase == 0);
    }

    SECTION("promotion overshoot clamps rather than extrapolating") {
        // Three queens plus a rook per side: raw phase 2*(12 + 2) = 28, i.e.
        // strictly ABOVE MAX_GAME_PHASE, so the clamp is actually exercised.
        // (Three queens a side alone is exactly 24 and would leave the clamp a
        // no-op — the test would then pass with the clamp deleted.)
        Board board("qqq1k2r/8/8/8/8/8/8/QQQ1K2R w - - 0 1");
        REQUIRE(complexEval->Breakdown(board).phase == MAX_GAME_PHASE);
    }
}

// ── Tapering behaviour (issue #99) ───────────────────────────────────────────

TEST_CASE("Eval - BlendPhase is exact at both endpoints", "[eval]")
{
    // The classic tapering bug is an off-by-one that makes neither endpoint
    // reproduce its own input, so assert both directly rather than inferring
    // them from a blended position.
    const ScorePair s{ 100, -40 };

    REQUIRE(BlendPhase(s, MAX_GAME_PHASE) == 100);
    REQUIRE(BlendPhase(s, 0) == -40);
}

TEST_CASE("Eval - BlendPhase moves monotonically between the endpoints", "[eval]")
{
    // Deliberately NOT a pair that divides evenly by MAX_GAME_PHASE: {240, 0}
    // gives exactly 10*phase at every step and so never truncates, which would
    // let an arbitrarily-rounding implementation pass. This pair crosses zero
    // and divides unevenly, exercising truncation in both directions.
    const ScorePair s{ 100, -40 };

    int previous = BlendPhase(s, 0);
    REQUIRE(previous == -40);
    for (int phase = 1; phase <= MAX_GAME_PHASE; ++phase) {
        const int current = BlendPhase(s, phase);
        CAPTURE(phase, previous, current);
        REQUIRE(current >= previous);
        previous = current;
    }
    REQUIRE(previous == 100);
}

TEST_CASE("Eval - king centralization is worth more as the phase drops", "[eval]")
{
    // The substantive effect of tapering: the same central king placement should
    // be scored progressively better as pieces come off, instead of jumping by
    // up to 100 cp the moment a material threshold is crossed.
    //
    // Both positions have the White king centralized on d4 and the Black king
    // offside on h8. The only difference is how much non-pawn material is left,
    // i.e. the phase.
    Board opening("3qk2r/8/8/8/3K4/8/8/3Q4 w - - 0 1");      // queens + a rook: high phase
    Board ending("4k3/8/8/8/3K4/8/8/8 w - - 0 1");            // bare kings: phase 0

    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    auto* complexEval = dynamic_cast<EvalComplex*>(eval.get());
    REQUIRE(complexEval != nullptr);

    const EvalBreakdown high = complexEval->Breakdown(opening);
    const EvalBreakdown low  = complexEval->Breakdown(ending);

    // Subtract White's queen PST explicitly rather than relying on it being 0.
    // It happens to be 0 on d1 today, but #117 is a PST-tuning issue: a queen
    // table change would otherwise silently turn this into a test of the queen.
    const int highKingOnly = high.pst[WHITE] - EvalProbe::GetPositionalScore(d1, WHITE_QUEEN);
    const int lowKingOnly  = low.pst[WHITE];
    CAPTURE(high.phase, low.phase, highKingOnly, lowKingOnly);

    REQUIRE(high.phase > low.phase);
    REQUIRE(lowKingOnly > highKingOnly);
}

TEST_CASE("Eval - crossing the old stage threshold no longer produces a cliff", "[eval]")
{
    // The property this change exists to create. Before #99, a capture that took
    // min(material) across 11500 flipped the king from the middlegame table to
    // the endgame one, moving a centralized king's score by up to ~100 cp in a
    // single ply.
    //
    // Both positions must sit on OPPOSITE sides of that retired threshold for
    // this to test anything, which means BOTH sides have to start above it —
    // the threshold took min() over the two colors. An earlier version of this
    // test used two bare-ish positions that were both already below it, so the
    // old code produced a delta of 0 and the test passed identically before and
    // after the change.
    //
    //   before: both sides K+Q+R+N = 11720 -> old MIDDLEGAME
    //   after:  Black loses the knight, 11400 -> old ENDGAME
    //
    // Under the old code removing that one knight swung the white-minus-black
    // king contribution by ~70 cp. It must now move by a small amount.
    Board before("1n1qk2r/8/8/8/3K4/8/8/1N1Q3R w - - 0 1");
    Board after("3qk2r/8/8/8/3K4/8/8/1N1Q3R w - - 0 1");

    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    auto* complexEval = dynamic_cast<EvalComplex*>(eval.get());
    REQUIRE(complexEval != nullptr);

    const EvalBreakdown b = complexEval->Breakdown(before);
    const EvalBreakdown a = complexEval->Breakdown(after);

    // Guard the premise: the two positions must actually differ in phase, and
    // both must be well clear of the endpoints, or there is no taper to test.
    CAPTURE(b.phase, a.phase);
    REQUIRE(b.phase != a.phase);
    REQUIRE(a.phase > 0);
    REQUIRE(b.phase < MAX_GAME_PHASE);

    // Net king-driven swing, isolated by removing the departing knight's own PST
    // from the before-position (it is the only piece that leaves).
    const int knightPst = EvalProbe::GetPositionalScore(b8, BLACK_KNIGHT);
    const int netBefore = (b.pst[WHITE] - b.pst[BLACK]);
    const int netAfter  = (a.pst[WHITE] - a.pst[BLACK]);
    const int swing = netAfter - (netBefore + knightPst);
    const int swingAbs = (swing < 0) ? -swing : swing;
    CAPTURE(netBefore, netAfter, knightPst, swing);

    REQUIRE(swingAbs < 20);
}

TEST_CASE("Eval - mop-up: walking the winning king toward the loser must raise the score (#118 item 4)", "[eval]")
{
    // The bug this fixes. Mop-up pays the winning king MOPUP_KINGDIST_WEIGHT (4)
    // per step of approach, while that same king's endgame PST charges it 10 cp
    // per step of centralization surrendered to walk toward the corner. The two
    // terms are pulling in opposite directions and the PST wins, so mop-up only
    // ever *softened* a disincentive to approach — it never reversed it. That is
    // the most likely reason #70 measured ≈0 Elo.
    //
    // Pawnless K+Q vs K+R: a 400 cp lead (exactly MOPUP_MATERIAL_THRESHOLD) and
    // phase 6 (Q=4 + R=2), so mop-up is gated on. The Black king is cornered on
    // a8; White's king moves from d4 to c5, strictly closer to it (Chebyshev
    // 4 -> 3) and no other piece moves.
    //
    // Legality checked: with White to move the Black king on a8 is attacked by
    // nothing (Qd1 covers the d-file, rank 1, and the d1-a4/d1-h5 diagonals),
    // and the kings are never adjacent.
    Board farther("k6r/8/8/8/3K4/8/8/3Q4 w - - 0 1");
    Board closer("k6r/8/8/2K5/8/8/8/3Q4 w - - 0 1");

    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    auto* complexEval = dynamic_cast<EvalComplex*>(eval.get());
    REQUIRE(complexEval != nullptr);

    // Named farBreakdown/nearBreakdown, not far/near: those are macros from
    // <windows.h> (minwindef.h defines both as empty), so a plain `far`/`near`
    // local silently breaks the moment any header in this translation unit
    // starts transitively including it.
    const EvalBreakdown farBreakdown = complexEval->Breakdown(farther);
    const EvalBreakdown nearBreakdown = complexEval->Breakdown(closer);

    // Guard the premise: if either position stopped being a gated mop-up
    // position this test would pass vacuously.
    CAPTURE(farBreakdown.phase, farBreakdown.mopup[WHITE], nearBreakdown.mopup[WHITE]);
    REQUIRE(farBreakdown.mopup[WHITE] > 0);
    REQUIRE(nearBreakdown.mopup[WHITE] > 0);
    REQUIRE(nearBreakdown.mopup[WHITE] > farBreakdown.mopup[WHITE]);

    // The actual property: White's total positional contribution must improve
    // when its king closes in. Before the fix the king PST's centralization loss
    // outweighs mop-up's approach bonus and this is negative.
    const int farTotal  = farBreakdown.pst[WHITE]  + farBreakdown.mopup[WHITE];
    const int nearTotal = nearBreakdown.pst[WHITE] + nearBreakdown.mopup[WHITE];
    CAPTURE(farBreakdown.pst[WHITE], nearBreakdown.pst[WHITE], farTotal, nearTotal);

    REQUIRE(nearTotal > farTotal);
}

// ---------------------------------------------------------------------------
// Passed and backwards pawns (issue #116)
//
// The span masks are tested for CONTENT first, before anything that consumes
// them: a wrong mask produces a term that is subtly wrong in every position at
// once, and no whole-position delta test would localise it.
// ---------------------------------------------------------------------------

TEST_CASE("PassedMask - white d4 covers exactly the c/d/e files ahead", "[eval]")
{
    BITBOARD expected = 0;
    for (const int sq : { c5, c6, c7, c8, d5, d6, d7, d8, e5, e6, e7, e8 })
        expected |= (1ULL << sq);

    REQUIRE(g_bbPassedMaskWhite[d4] == expected);
}

TEST_CASE("PassedMask - black d5 covers exactly the c/d/e files ahead", "[eval]")
{
    BITBOARD expected = 0;
    for (const int sq : { c4, c3, c2, c1, d4, d3, d2, d1, e4, e3, e2, e1 })
        expected |= (1ULL << sq);

    REQUIRE(g_bbPassedMaskBlack[d5] == expected);
}

TEST_CASE("PassedMask - edge files do not wrap around the board", "[eval]")
{
    // The classic generator bug: shifting a file mask sideways wraps a2's span
    // onto the h-file. Built with explicit file bounds instead, so assert it.
    BITBOARD hFile = 0;
    for (const int sq : { h1, h2, h3, h4, h5, h6, h7, h8 })
        hFile |= (1ULL << sq);
    BITBOARD aFile = 0;
    for (const int sq : { a1, a2, a3, a4, a5, a6, a7, a8 })
        aFile |= (1ULL << sq);

    REQUIRE((g_bbPassedMaskWhite[a2] & hFile) == 0);
    REQUIRE((g_bbPassedMaskWhite[h2] & aFile) == 0);
    REQUIRE((g_bbPassedMaskBlack[a7] & hFile) == 0);
    REQUIRE((g_bbPassedMaskBlack[h7] & aFile) == 0);
}

TEST_CASE("PassedMask - nothing is ahead of a pawn on the promotion rank", "[eval]")
{
    REQUIRE(g_bbPassedMaskWhite[d8] == 0);
    REQUIRE(g_bbPassedMaskBlack[d1] == 0);
}

TEST_CASE("Eval - EvalComplex rewards a passed pawn over a blocked one", "[eval]")
{
    // Identical but for the black pawn on d7, which stands in the white e5
    // pawn's adjacent-file span and so denies it passed status.
    Board passed("4k3/8/8/4P3/8/8/8/4K3 w - - 0 1");
    Board blocked("4k3/3p4/8/4P3/8/8/8/4K3 w - - 0 1");

    const int passedScore  = EvalComplexTestFixture::Pawns(passed, WHITE);
    const int blockedScore = EvalComplexTestFixture::Pawns(blocked, WHITE);

    CAPTURE(passedScore, blockedScore);
    REQUIRE(passedScore > blockedScore);
}

TEST_CASE("Eval - EvalComplex passer bonus grows as the pawn advances", "[eval]")
{
    // Both kings on a8/a-file, deliberately off the pawn's file: with the black
    // king on e8 the seventh-rank pawn would be BLOCKADED, so the pair would vary
    // advancement and blockade status together and the comparison would no longer
    // isolate rank.
    Board third("k7/8/8/8/8/4P3/8/4K3 w - - 0 1");
    Board seventh("k7/4P3/8/8/8/8/8/4K3 w - - 0 1");

    const int thirdScore   = EvalComplexTestFixture::Pawns(third, WHITE);
    const int seventhScore = EvalComplexTestFixture::Pawns(seventh, WHITE);

    CAPTURE(thirdScore, seventhScore);
    REQUIRE(seventhScore > thirdScore);
}

TEST_CASE("Eval - EvalComplex passer is worth more in the endgame than the middlegame", "[eval]")
{
    // Same pawn structure. The queens and rooks only move the phase, and
    // eval_pawns reads no piece other than pawns, so any difference here is the
    // mg/eg endpoints of the passer bonus and nothing else.
    Board middlegame("rq2k3/8/8/4P3/8/8/8/RQ2K3 w - - 0 1");
    Board endgame("4k3/8/8/4P3/8/8/8/4K3 w - - 0 1");

    const int middlegameScore = EvalComplexTestFixture::Pawns(middlegame, WHITE);
    const int endgameScore    = EvalComplexTestFixture::Pawns(endgame, WHITE);

    CAPTURE(middlegameScore, endgameScore);
    REQUIRE(endgameScore > middlegameScore);
}

TEST_CASE("Eval - EvalComplex detects an a-file passer", "[eval]")
{
    // Guards the wraparound case end to end: a black h-pawn must not stop the
    // white a-pawn from counting as passed, while a black b-pawn must.
    Board aFilePasser("4k3/7p/8/P7/8/8/8/4K3 w - - 0 1");
    Board aFileBlocked("4k3/1p6/8/P7/8/8/8/4K3 w - - 0 1");

    const int passerScore  = EvalComplexTestFixture::Pawns(aFilePasser, WHITE);
    const int blockedScore = EvalComplexTestFixture::Pawns(aFileBlocked, WHITE);

    CAPTURE(passerScore, blockedScore);
    REQUIRE(passerScore > blockedScore);
}

TEST_CASE("Eval - EvalComplex penalises a backwards pawn", "[eval]")
{
    // White b2 is backwards: its only neighbour (c3) has advanced past it, and
    // its stop square b3 is attacked by the black a4 pawn and defended by no
    // white pawn.
    //
    // The control moves that black pawn a4 -> a5, which stops it attacking b3
    // (it now covers b4) and changes NOTHING else: a5 is still inside b2's
    // forward span, so b2 remains not-passed in both positions. Moving it to the
    // h-file instead would also take it out of that span and hand b2 a passer
    // bonus, and the assertion would then pass on the passer swing while telling
    // us nothing about clause (b).
    Board backwards("4k3/8/8/8/p7/2P5/1P6/4K3 w - - 0 1");
    Board notAttacked("4k3/8/8/p7/8/2P5/1P6/4K3 w - - 0 1");

    const int backwardsScore   = EvalComplexTestFixture::Pawns(backwards, WHITE);
    const int notAttackedScore = EvalComplexTestFixture::Pawns(notAttacked, WHITE);

    CAPTURE(backwardsScore, notAttackedScore);
    REQUIRE(backwardsScore < notAttackedScore);
}

TEST_CASE("Eval - EvalComplex does not penalise a pawn its neighbour is level with", "[eval]")
{
    // Clause (a) only. The two positions differ by the white a2 pawn and nothing
    // else: it sits LEVEL with b2 on an adjacent file, so b2 is no longer behind
    // every neighbour and is not backwards -- even though b3 is still attacked by
    // a4 and still undefended, so clause (b) holds in both.
    //
    // a2 itself contributes nothing to compare against: it is not passed (the
    // black a4 pawn is in its span), not isolated (b2 is adjacent), not doubled,
    // and not backwards (b2 is level with it in turn). So the whole difference is
    // b2's penalty disappearing.
    Board levelNeighbour("4k3/8/8/8/p7/2P5/PP6/4K3 w - - 0 1");
    Board backwards("4k3/8/8/8/p7/2P5/1P6/4K3 w - - 0 1");

    const int levelScore     = EvalComplexTestFixture::Pawns(levelNeighbour, WHITE);
    const int backwardsScore = EvalComplexTestFixture::Pawns(backwards, WHITE);

    CAPTURE(levelScore, backwardsScore);
    REQUIRE(levelScore > backwardsScore);
}

TEST_CASE("Eval - EvalComplex passer bonus is monotonic across every rank", "[eval]")
{
    // The bonus is scaled by rank and then again by the blockade factor, and each
    // scaling truncates. Truncation cannot be reasoned about in general -- it has
    // to be checked at every rank, because a single non-monotonic step means an
    // advancing pawn can lose value by moving forward.
    //
    // Walked twice: once free, once with the black king held on the pawn's stop
    // square at every step, because the blockade discount is the SECOND scaling and
    // only the blockaded walk exercises both truncations.
    //
    // Free walk -- black king parked on a8, off the pawn's file, so it never
    // blockades and never moves the phase.
    const char* byRank[] = {
        "k7/8/8/8/8/8/4P3/4K3 w - - 0 1",   // e2
        "k7/8/8/8/8/4P3/8/4K3 w - - 0 1",   // e3
        "k7/8/8/8/4P3/8/8/4K3 w - - 0 1",   // e4
        "k7/8/8/4P3/8/8/8/4K3 w - - 0 1",   // e5
        "k7/8/4P3/8/8/8/8/4K3 w - - 0 1",   // e6
        "k7/4P3/8/8/8/8/8/4K3 w - - 0 1",   // e7
    };

    int previous = std::numeric_limits<int>::min();
    for (const char* fen : byRank) {
        Board board(fen);
        const int score = EvalComplexTestFixture::Pawns(board, WHITE);
        CAPTURE(fen, score, previous);
        REQUIRE(score >= previous);
        previous = score;
    }

    // Blockaded walk -- the black king sits on the stop square at every rank, so
    // every value goes through `scale * BLOCKADED / 16` as well.
    const char* byRankBlockaded[] = {
        "8/8/8/8/8/4k3/4P3/4K3 w - - 0 1",   // e2, king e3
        "8/8/8/8/4k3/4P3/8/4K3 w - - 0 1",   // e3, king e4
        "8/8/8/4k3/4P3/8/8/4K3 w - - 0 1",   // e4, king e5
        "8/8/4k3/4P3/8/8/8/4K3 w - - 0 1",   // e5, king e6
        "8/4k3/4P3/8/8/8/8/4K3 w - - 0 1",   // e6, king e7
        "4k3/4P3/8/8/8/8/8/4K3 w - - 0 1",   // e7, king e8
    };

    previous = std::numeric_limits<int>::min();
    for (const char* fen : byRankBlockaded) {
        Board board(fen);
        const int score = EvalComplexTestFixture::Pawns(board, WHITE);
        CAPTURE(fen, score, previous);
        REQUIRE(score >= previous);
        previous = score;
    }
}

TEST_CASE("Eval - EvalComplex discounts a passer whose stop square is blockaded", "[eval]")
{
    // Same white passer on e6 both times; the black king either sits on its stop
    // square e7, where the pawn cannot move at all until it is dislodged, or stands
    // aside on a8.
    //
    // The blockade test is the one place `eval_pawns` reads a non-pawn bitboard
    // (`ctx.occupied[enemy]`), and the only square of it that can matter is the stop
    // square -- so moving the king between those two squares changes exactly one
    // input to one term, and the whole delta is the discount.
    Board blockaded("4k3/4P3/8/8/8/8/8/4K3 w - - 0 1");
    Board freeToRun("k7/4P3/8/8/8/8/8/4K3 w - - 0 1");

    const int blockadedScore = EvalComplexTestFixture::Pawns(blockaded, WHITE);
    const int freeScore      = EvalComplexTestFixture::Pawns(freeToRun, WHITE);

    CAPTURE(blockadedScore, freeScore);
    REQUIRE(freeScore > blockadedScore);
}

TEST_CASE("Eval - EvalComplex does not score the rear pawn of a doubled pair as passed", "[eval]")
{
    // The rear pawn can never advance past its own partner, so only the front
    // pawn of the pair is passed. Both positions have the same two white pawns
    // on the same two ranks and no black pawns at all; they differ only in
    // whether the pawns share a file.
    Board doubled("4k3/8/4P3/4P3/8/8/8/4K3 w - - 0 1");
    Board separated("4k3/8/4P3/2P5/8/8/8/4K3 w - - 0 1");

    const int doubledScore   = EvalComplexTestFixture::Pawns(doubled, WHITE);
    const int separatedScore = EvalComplexTestFixture::Pawns(separated, WHITE);

    // Separated wins by two passer bonuses against one, minus the doubled and
    // isolated penalties -- a margin far larger than the 10 cp that separated
    // them when the rear pawn was scored as a passer too.
    CAPTURE(doubledScore, separatedScore);
    REQUIRE(separatedScore > doubledScore + 50);
}
