#pragma once
// EvalTestFixture.h — shared helpers for EvalSimple and EvalComplex tests
//
// Validates the direction and relative magnitude of evaluation scores.
// Exact centipawn values are intentionally NOT tested — positional tables
// and future evaluation changes would make exact-value tests fragile.
//
// Pattern: Board board(fen); then EvalManager::Create(type)->Evaluate(board)
//
// See Docs/TestDesign.md §Phase 0 for rationale.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
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
static constexpr const char* FEN_START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// White has all pawns + queen + king; black has all pawns + king only (no queen).
// White to move — white has a massive material advantage.
static constexpr const char* FEN_WHITE_EXTRA_QUEEN = "4k3/pppppppp/8/8/8/8/PPPPPPPP/3QK3 w - - 0 1";

// Black has all pawns + queen + king; white has all pawns + king only (no queen).
// Black to move — black has a massive material advantage.
static constexpr const char* FEN_BLACK_EXTRA_QUEEN = "3qk3/pppppppp/8/8/8/8/PPPPPPPP/4K3 b - - 0 1";

// White doubled pawns on the a-file (Pa2 + Pa3); black has Pa7 + Pb7 (normal structure).
// White to move.
static constexpr const char* FEN_WHITE_DOUBLED = "4k3/pp6/8/8/8/P7/P7/4K3 w - - 0 1";

// White normal pawn structure (Pa2 + Pb3); black has Pa7 + Pb7.
// White to move. Same material as FEN_WHITE_DOUBLED but no doubled pawn.
static constexpr const char* FEN_WHITE_NORMAL = "4k3/pp6/8/8/8/1P6/P7/4K3 w - - 0 1";

// Endgame: White Ke1 + Re7 (rook on 7th rank). Black Kg8.
// Reduced material triggers ENDGAME stage; rook-on-7th bonus should apply.
// White to move. The black king stands off the e-file, so the rook does not
// attack it — legal. No pawns of either colour, so the e-file counts as open.
static constexpr const char* FEN_ROOK_ON_7TH = "6k1/4R3/8/8/8/8/8/4K3 w - - 0 1";

// Mop-up evaluation (issue #70 / epic #110): White King+Queen vs Black King+Rook,
// pawnless, decisive material lead (900 - 500 = 400 cp). Black king cornered (a8)
// vs centered (c6) — everything else identical. White to move.
static constexpr const char* FEN_MOPUP_LOSER_KING_CORNER = "k6r/8/8/8/3Q4/8/8/4K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_LOSER_KING_CENTER = "7r/8/2k5/8/3Q4/8/8/4K3 w - - 0 1";

// Same as above, but with one pawn each (Pa2/pa7) — mop-up must be gated off
// once pawns are on the board.
static constexpr const char* FEN_MOPUP_LOSER_KING_CORNER_WITH_PAWNS = "k6r/p7/8/8/3Q4/8/P7/4K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_LOSER_KING_CENTER_WITH_PAWNS = "7r/p7/2k5/8/3Q4/8/P7/4K3 w - - 0 1";

// The defender-force gate (issue #118 item 5): a defending QUEEN, whose phase of
// 4 passed the retired phase-keyed gate. Same pawnless, decisive-lead shape as
// the cases above, so only the defender's force distinguishes it.
static constexpr const char* FEN_MOPUP_DEFENDER_HAS_QUEEN = "k7/1q6/8/8/3Q4/8/8/4K2R w - - 0 1";

// White King+Knight vs Black King+Bishop, pawnless, materially EQUAL (300 - 300 = 0).
// Same corner/center king placement idea — mop-up must be gated off below the
// decisive-material threshold.
static constexpr const char* FEN_MOPUP_MARGINAL_CORNER = "7k/8/8/8/5N2/8/8/b3K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_MARGINAL_CENTER = "8/8/2k5/8/5N2/8/8/b3K3 w - - 0 1";

// The two scaled pawnless rook classes (issue #128), White the stronger side:
// K+R+N vs K+R and K+R vs K+N. Nothing attacks either king in either.
static constexpr const char* FEN_ROOK_AND_MINOR_VS_ROOK = "4k2r/8/8/8/8/5N2/3R4/4K3 w - - 0 1";
static constexpr const char* FEN_ROOK_VS_MINOR = "4k2n/8/8/8/8/8/3R4/4K3 w - - 0 1";

// Level material, so the class has no stronger side and the whole score is
// positional (#436). White's rook is on the 7th and Black's is passive on its
// own back rank, which is what makes the unscaled score non-zero — a position
// scoring zero could not tell a scale from a clamp.
static constexpr const char* FEN_ROOK_VS_ROOK = "r3k3/3R4/8/8/8/8/8/4K3 w - - 0 1";

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
static constexpr const char* FEN_QUEEN_C6 = "7k/8/2Q5/8/8/8/8/4K3 w - - 0 1";

// Middlegame with rooks on open/half-open files: both sides have 11800 material
// (iMinScore > 11500 -> MIDDLEGAME), exercising the rook open/half-open-file
// terms and the middlegame king PST under color mirroring.
static constexpr const char* FEN_MIDDLEGAME_ROOKS = "2rr2k1/pp3ppp/5n2/8/8/2N5/PP3PPP/3RR1K1 w - - 0 1";

// Endgame with pawns: 10100 material each side trips the ENDGAME stage (and so
// the endgame king PST), while the pawns on the board keep the pawnless mop-up
// branch gated off.
static constexpr const char* FEN_ENDGAME_KING_PST = "8/5p2/4k3/8/8/2K5/3P4/8 w - - 0 1";

// Rook open-file definition (issue #126). White Re1 + Kf1, Black Kg8 + one
// minor/pawn on the rook's file (e) or one file off (d). The knight's PST
// value is identical on d5/e5 (10), and the pawn's PST value is identical on
// d5/e5 (14), so within each pair the ONLY eval difference is the open-file
// classification.
static constexpr const char* FEN_ROOK_OPEN_FILE_KNIGHT_ON = "6k1/8/8/4n3/8/8/8/4RK2 w - - 0 1";
static constexpr const char* FEN_ROOK_OPEN_FILE_KNIGHT_OFF = "6k1/8/8/3n4/8/8/8/4RK2 w - - 0 1";
static constexpr const char* FEN_ROOK_OPEN_FILE_PAWN_ON = "6k1/8/8/4p3/8/8/8/4RK2 w - - 0 1";
static constexpr const char* FEN_ROOK_OPEN_FILE_PAWN_OFF = "6k1/8/8/3p4/8/8/8/4RK2 w - - 0 1";

// Rook open-file, own-pawn-behind decision (D5, issue #126). Same fixed White
// pawn on e4 in both; the rook sits behind it (e6) or ahead of it (e2).
static constexpr const char* FEN_ROOK_OWN_PAWN_BEHIND = "6k1/8/4R3/8/4P3/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_ROOK_OWN_PAWN_AHEAD = "6k1/8/8/8/4P3/8/4R3/6K1 w - - 0 1";

// D5, controlled pair: rook fixed on e6 in both, White pawn on d4 (off the
// rook's file) vs e4 (on it, behind the rook). The White pawn's PST value is
// identical on d4 and e4 (14), both pawns are isolated, and neither file holds
// an enemy pawn — so if an own pawn behind the rook leaves the file fully OPEN
// (D5 as implemented), these two must score exactly EQUAL. Widening the
// own-pawn test to the whole file would break that equality by 15 cp, which is
// what makes this pair, unlike the >-assertion above, actually pin D5.
static constexpr const char* FEN_ROOK_OWN_PAWN_OFF_FILE = "6k1/8/4R3/8/3P4/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_ROOK_OWN_PAWN_BEHIND_SAME_ROOK = "6k1/8/4R3/8/4P3/8/8/6K1 w - - 0 1";

struct EvalProbe final : EvalManager {
	int Evaluate(const Board&) const override { return 0; }
	const char* GetType() const override { return "Probe"; }
	using EvalManager::getEvalBoard;
	using EvalManager::GetPositionalScore;
};
// Castling asymmetry (issue #115). Neither side has rights left; White's king
// is tucked on g1 while Black's sits on e8, so eval_castling pays White and
// penalises Black. Mirroring swaps the position and the side to move together,
// so the score stays EQUAL as above; what this case pins is that the term's
// direction-aware WHITE_BACK_ROW / BLACK_BACK_ROW pairing is the right way
// round, since getting it backwards makes the two sides disagree and breaks
// that equality. Legal: Rf1 covers the f-file only, so Black (the non-mover)
// is not in check.
static constexpr const char* FEN_CASTLED_VS_CENTRAL_KING = "4k3/8/8/8/8/8/8/5RK1 w - - 0 1";

// Bishop-pair asymmetry (issue #111). White holds bishops on both square
// colours (c1 dark, f1 light); Black has a single bishop on g7 — so the pair
// bonus applies to exactly one side, which is what makes the mirror
// discriminating. Legal: neither king is attacked.
static constexpr const char* FEN_BISHOP_PAIR_VS_SINGLE = "4k3/6b1/8/8/8/8/8/2B1KB2 w - - 0 1";

// Passed pawns on OPPOSITE EDGE FILES, at deliberately different advancement
// (issue #116). Both properties matter: opposite edges mean a mask that wrapped
// around the a/h boundary shows up in a whole-position score rather than only in
// the mask unit tests, and unequal advancement means a broken per-colour
// `advanced` index cannot cancel between the two sides. Legal: kings on e8/e1,
// neither attacked.
static constexpr const char* FEN_EDGE_FILE_PASSERS = "4k3/8/8/p7/8/7P/8/4K3 w - - 0 1";

// Blockaded passer (issue #116). White's e7 pawn has the black king squarely on
// its stop square, so the blockade discount applies to exactly one side. This is
// the only place `eval_pawns` reads a non-pawn bitboard (`ctx.occupied[enemy]`),
// and it is per-colour, so without this the mirror battery never exercises it.
// Legal: the kings are not adjacent and neither is in check.
static constexpr const char* FEN_BLOCKADED_PASSER = "4k3/4P3/8/8/8/8/8/4K3 w - - 0 1";

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
    // Issue #128: the only entries whose score passes through a FRACTIONAL
    // endgame scale. The exact-draw classes are zero on both sides of the
    // mirror and so cannot discriminate a sign error in the scaling arithmetic.
    FEN_ROOK_AND_MINOR_VS_ROOK,
    FEN_ROOK_VS_MINOR,
    FEN_ROOK_VS_ROOK,
};

// Swaps the case of a single character; digits and other characters pass
// through unchanged.

inline char SwapPieceCase(char c)
{
	if (c >= 'a' && c <= 'z')
		return static_cast<char>(c - 'a' + 'A');
	if (c >= 'A' && c <= 'Z')
		return static_cast<char>(c - 'A' + 'a');
	return c;
}

// Reverses rank order and swaps every piece's color.
inline std::string MirrorPlacement(const std::string& placement)
{
	std::vector<std::string> ranks;
	std::stringstream ss(placement);
	std::string rank;
	while (std::getline(ss, rank, '/'))
		ranks.push_back(rank);

	std::string result;
	for (auto it = ranks.rbegin(); it != ranks.rend(); ++it) {
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
inline std::string MirrorCastling(const std::string& castling)
{
	if (castling == "-")
		return "-";

	std::string swapped;
	for (char c : castling)
		swapped += SwapPieceCase(c);

	std::string canonical;
	for (char c : {'K', 'Q', 'k', 'q'})
		if (swapped.find(c) != std::string::npos)
			canonical += c;

	return canonical.empty() ? "-" : canonical;
}

// Mirrors the en-passant target's rank (e3 <-> e6, i.e. digit d -> 9 - d); the
// file is unchanged. "-" passes through unchanged.
inline std::string MirrorEnPassant(const std::string& ep)
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
inline std::string MirrorFen(std::string_view fen)
{
	std::istringstream iss{std::string(fen)};
	std::string placement, active, castling, ep, halfmove, fullmove;
	iss >> placement >> active >> castling >> ep >> halfmove >> fullmove;

	return MirrorPlacement(placement) + ' ' + (active == "w" ? "b" : "w") + ' ' + MirrorCastling(castling) + ' ' +
	       MirrorEnPassant(ep) + ' ' + halfmove + ' ' + fullmove;
}
// EvalComplexTestFixture is a friend of EvalComplex (STRAT_ENABLE_TEST_ACCESS,
// same mechanism as AIPerplex/UciHandler's fixtures) that builds an
// EvalContext from a Board and forwards to each term, so terms can be
// asserted on directly instead of only inferred from whole-position deltas.
struct EvalComplexTestFixture {
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

	// The scaled-class factors are private tuning parameters; naming them here
	// keeps the tests asserting the classification rather than restating the
	// numbers, which a sweep is expected to change.
	static constexpr int RookAndMinorVsRookScale = EvalComplex::ROOK_AND_MINOR_VS_ROOK_SCALE;
	static constexpr int RookVsMinorScale = EvalComplex::ROOK_VS_MINOR_SCALE;
	static constexpr int RookVsRookScale = EvalComplex::ROOK_VS_ROOK_SCALE;

	// The score before any endgame scale — what Evaluate() would have returned
	// without the classifier. Lets a scaled case assert the discount reached the
	// returned score, rather than only that it was reported in the breakdown.
	static int RawWhitePov(const Board& board) { return EvalComplex::RawWhitePov(BuildContext(board)); }

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
	static int Phase(const Board& board) { return BuildContext(board).phase; }

	// Named constants, exposed so term-level tests can express exact
	// expected values without duplicating magic numbers.
	static int DoubledPawnPenalty() { return EvalComplex::DOUBLED_PAWN_PENALTY; }
	static int IsolatedPawnPenalty() { return EvalComplex::ISOLATED_PAWN_PENALTY; }
	static int RookOn7thBonus() { return EvalComplex::ROOK_ON_7TH_BONUS; }
	static int OpenFile() { return EvalComplex::OPEN_FILE; }
	static int HalfOpenFile() { return EvalComplex::HALF_OPEN_FILE; }

  private:
	// Forwards to EvalComplex::BuildContext — the production construction
	// site (Eval.cpp) — rather than reimplementing it here. Issue #99 will
	// eventually replace the `11500` phase threshold BuildContext uses
	// internally; having only one construction site means that change can't
	// leave this fixture silently testing the old threshold.
	static EvalContext BuildContext(const Board& board) { return EvalComplex::BuildContext(board); }
};
