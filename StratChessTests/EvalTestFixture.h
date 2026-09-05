#pragma once
// EvalTestFixture.h — shared helpers for Evaluator tests
//
// Validates the direction and relative magnitude of evaluation scores.
// Exact centipawn values are intentionally NOT tested — positional tables
// and future evaluation changes would make exact-value tests fragile.
//
// Pattern: Board board(fen); then Evaluator().Evaluate(board)
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

// Symmetric starting position — should evaluate to ~0.
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

// King safety (issue #97). Every case below keeps a queen on each side so the
// phase is nonzero — all four king-safety contributions are middlegame-only and
// blend to exactly 0 at phase 0, which would make a bare-king case vacuous.
//
// The frame is White Qd1 against Black Qd8/Kg8, so only the White king and the
// pawns distinguish one case from the next. The queens sit on the d-file rather
// than the a-file because a queen on a1 attacks h8 along the long diagonal,
// which makes any case wanting a king in that corner an illegal position — and
// Board falls back to the starting position for one rather than failing loudly.

// Shield states, all three with the same three White pawns on the board so the
// comparison is of placement only: on their starting squares, pushed one rank,
// and moved off the king's three files entirely.
static constexpr const char* FEN_KING_SHIELD_INTACT = "3q2k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1";
static constexpr const char* FEN_KING_SHIELD_PUSHED = "3q2k1/5ppp/8/8/8/5P1P/6P1/3Q2K1 w - - 0 1";
static constexpr const char* FEN_KING_SHIELD_ABSENT = "3q2k1/5ppp/8/8/8/8/1PPP4/3Q2K1 w - - 0 1";

// The absence sentinel (D4). A pawn LEVEL with the king is a real pawn and must
// not read as "no pawn on this file", which indexing by distance-from-the-king
// would have made indistinguishable. White Ke4 with a pawn on d4 against the
// same pawn moved to a4, off the king's files.
static constexpr const char* FEN_KING_PAWN_LEVEL = "3q2k1/8/8/8/3PK3/8/8/3Q4 w - - 0 1";
static constexpr const char* FEN_KING_PAWN_OFF_FILE = "3q2k1/8/8/8/P3K3/8/8/3Q4 w - - 0 1";

// Storm direction: an enemy pawn BEHIND the king is not storming it. White Kg4
// with a Black pawn ahead of it on g5, against the same pawn behind it on g3.
static constexpr const char* FEN_KING_STORM_AHEAD = "3q2k1/8/8/6p1/6K1/8/8/3Q4 w - - 0 1";
static constexpr const char* FEN_KING_STORM_BEHIND = "3q2k1/8/8/8/6K1/6p1/8/3Q4 w - - 0 1";

// The blocked-storm condition (D4): our shield pawn stands at exactly the
// storming pawn's rank minus one. Same Black pawn on g3 in both; White's pawn
// is on g2 (directly in its path) or on f2 (not).
static constexpr const char* FEN_KING_STORM_BLOCKED = "3q2k1/8/8/8/8/6p1/6P1/3Q2K1 w - - 0 1";
static constexpr const char* FEN_KING_STORM_UNBLOCKED = "3q2k1/8/8/8/8/6p1/5P2/3Q2K1 w - - 0 1";

// King-file openness (D5), isolated on the g-file. White has no g-pawn in the
// half-open and open cases, and Black's g-pawn is the only difference between
// them. That pawn IS inside White's storm scan and does index the storm table —
// the row it lands in is zero today, which is why the king-files row is the only
// thing that moves. The test asserts that zero rather than assuming it, so a
// #117 retune breaks it loudly instead of silently ending the isolation.
static constexpr const char* FEN_KING_FILE_CLOSED = "3q2k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1";
static constexpr const char* FEN_KING_FILE_HALF_OPEN = "3q2k1/5ppp/8/8/8/8/4PP1P/3Q2K1 w - - 0 1";
static constexpr const char* FEN_KING_FILE_OPEN = "3q2k1/4pp1p/8/8/8/8/4PP1P/3Q2K1 w - - 0 1";

// Attack pressure (D6). One frame throughout: Black Kg8 behind f7/g7/h7, White
// Kg1, and a Black queen on d8 so the phase stays high. Only what White has
// pointed at the black king changes, so the king-attack row is the only thing
// that can move -- Black's own shelter, storm and file rows are identical in
// every one of them.
//
// White's Qh5 is not a check: h6/h7 stops on the pawn and the h5-e8 diagonal
// stops on f7. The knight on a1 in the one-attacker case reaches nothing inside
// the zone, which is what makes it, rather than an empty square, the control.
static constexpr const char* FEN_KING_ATTACK_NONE = "3q2k1/5ppp/8/8/8/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_KING_ATTACK_QUEEN = "3q2k1/5ppp/8/7Q/8/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_KING_ATTACK_ONE = "3q2k1/5ppp/8/7Q/8/8/8/N5K1 w - - 0 1";
static constexpr const char* FEN_KING_ATTACK_TWO = "3q2k1/5ppp/8/6NQ/8/8/8/6K1 w - - 0 1";
// One queen again, but on a1, where the long diagonal stops on Black's g7 pawn.
// The attacker count is still 1; the number of zone squares it covers drops from
// six to two (f6 and g7 itself, which is attacked though occupied), which is what
// isolates the second half of the danger count.
static constexpr const char* FEN_KING_ATTACK_QUEEN_FAR = "3q2k1/5ppp/8/8/8/8/8/Q5K1 w - - 0 1";

// The same frame again, with one White piece placed so that each piece type
// covers exactly THREE zone squares -- Ne6 hits f8/g5/g7, Be4 hits f5/g6/h7,
// Ra5 and Qa5 hit f5/g5/h5 along the fifth rank while the queen's diagonals
// leave the zone entirely. Equal coverage and one attacker each is what makes
// the remaining difference between them the attacker's WEIGHT and nothing else.
static constexpr const char* FEN_KING_ATTACK_BUCKET_KNIGHT = "3q2k1/5ppp/4N3/8/8/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_KING_ATTACK_BUCKET_BISHOP = "3q2k1/5ppp/8/8/4B3/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_KING_ATTACK_BUCKET_ROOK = "3q2k1/5ppp/8/R7/8/8/8/6K1 w - - 0 1";
static constexpr const char* FEN_KING_ATTACK_BUCKET_QUEEN = "3q2k1/5ppp/8/Q7/8/8/8/6K1 w - - 0 1";

// The worst king this evaluator can be handed: no White pawn on f, g or h, and
// Black pawns on f3, g3 and h3 — every shelter file at its absence entry, every
// storm file near its maximum, and the three files still penalised for
// openness. The declared bound has to hold here or it is not a bound.
static constexpr const char* FEN_KING_SAFETY_WORST = "3q2k1/8/8/8/8/5ppp/1PPP4/3Q2K1 w - - 0 1";

// Color-symmetry regression cases (issue #125) — see MirrorFen below.

// White queen on c6: the case that exposes the pre-fix getEvalBoard defect.
// getEvalBoard(BLACK, sq) used (63 - sq), a 180-degree rotation that mirrors
// files as well as ranks, instead of a vertical flip (sq ^ 56). Before this fix
// the queen PST was not file-symmetric (c6 = 4, f6 = 3), so a White queen on c6
// and its color-mirror (a Black queen on c3) scored 1 cp apart. Black king
// on h8 is not attacked: c6's rank/file/diagonals reach a6-h6, c1-c8, a8, b7,
// d7, e8, d5, e4, f3, g2, h1, b5, a4 — not h8. White to move.
//
// It is not attacked, but it IS zone-attacked: c6's rank reaches f6, g6 and h6,
// three squares of Kh8's king zone. So this position drives eval_king_attack too,
// which is what makes the whole-position symmetry case non-vacuous for that term.
//
// This is the ONLY whole-position case below that discriminates: the others
// contain no queen at all, or (FEN_MOPUP_LOSER_KING_CORNER) a queen whose
// rotated and flipped images happen to hold the same value. The direct
// getEvalBoard tests are the real guard — do not delete them as "redundant
// with the position tests".
static constexpr const char* FEN_QUEEN_C6 = "7k/8/2Q5/8/8/8/8/4K3 w - - 0 1";

// Middlegame with rooks on open/half-open files: both sides have 11800
// material, exercising the rook open/half-open-file terms under color
// mirroring. It says nothing about king placement: the middlegame king PST is
// flat, so only the endgame endpoint bleeding through the taper is nonzero.
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

// Derives rather than widening Evaluator's protected PST helpers to public,
// which would open them to every production caller just to reach them from a
// test.
struct EvalProbe final : Evaluator {
	using Evaluator::getEvalBoard;
	using Evaluator::GetPositionalScore;
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
    // Issue #97: the king-safety terms are direction-aware through a
    // defender-relative rank index, and every case above is either shield-
    // symmetric or at phase 0, where all four contributions are 0 anyway.
    FEN_KING_SHIELD_PUSHED,
    FEN_KING_STORM_BLOCKED,
    FEN_KING_FILE_HALF_OPEN,
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
// The reconstruction identity every breakdown must satisfy: the per-term net
// columns plus the endgame adjustment equal the white-POV score. Written once
// here rather than per test, so a new term is added to it in exactly one place
// and cannot be silently omitted from a test that would then still pass.
inline int BreakdownWhitePov(const EvalBreakdown& terms)
{
	return (terms.material[WHITE] - terms.material[BLACK]) + (terms.pawns[WHITE] - terms.pawns[BLACK]) +
	       (terms.rooks[WHITE] - terms.rooks[BLACK]) + (terms.pst[WHITE] - terms.pst[BLACK]) +
	       (terms.mopup[WHITE] - terms.mopup[BLACK]) + (terms.bishops[WHITE] - terms.bishops[BLACK]) +
	       (terms.castling[WHITE] - terms.castling[BLACK]) + (terms.mobility[WHITE] - terms.mobility[BLACK]) +
	       (terms.king_shelter[WHITE] - terms.king_shelter[BLACK]) +
	       (terms.king_storm[WHITE] - terms.king_storm[BLACK]) + (terms.king_files[WHITE] - terms.king_files[BLACK]) +
	       (terms.king_attack[WHITE] - terms.king_attack[BLACK]) + terms.endgame_adjustment;
}

// EvaluatorTestFixture is a friend of Evaluator (STRAT_ENABLE_TEST_ACCESS,
// same mechanism as AIPerplex/UciHandler's fixtures) that builds an
// EvalContext from a Board and forwards to each term, so terms can be
// asserted on directly instead of only inferred from whole-position deltas.
struct EvaluatorTestFixture {
	static int Pawns(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_pawns(ctx, color), ctx.phase);
	}
	static int Rooks(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_rooks(ctx, color), ctx.phase);
	}
	static int Pst(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_pst(ctx, color), ctx.phase);
	}
	static int Mopup(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_mopup(ctx, color), ctx.phase);
	}
	static int Bishops(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_bishops(ctx, color), ctx.phase);
	}
	static int Mobility(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_mobility(ctx, color), ctx.phase);
	}
	static int Castling(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_castling(ctx, color), ctx.phase);
	}

	static int KingShelter(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_king_pawn_cover(ctx, color).shelter, ctx.phase);
	}
	static int KingStorm(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_king_pawn_cover(ctx, color).storm, ctx.phase);
	}
	static int KingFiles(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_king_pawn_cover(ctx, color).files, ctx.phase);
	}
	// Unblended, so a test can assert the eg endpoints are 0 rather than infer
	// it from a low-phase position.
	static KingPawnCover KingCover(const Board& board, eColor color)
	{
		return Evaluator::eval_king_pawn_cover(BuildContext(board), color);
	}
	static int KingAttack(const Board& board, eColor color)
	{
		const EvalContext ctx = BuildContext(board);
		return BlendPhase(Evaluator::eval_king_attack(ctx, color), ctx.phase);
	}
	// Unblended, so a case can compare two positions of different phase, or
	// assert the eg endpoint is 0 rather than infer it from a low-phase board.
	static ScorePair KingAttackPair(const Board& board, eColor color)
	{
		return Evaluator::eval_king_attack(BuildContext(board), color);
	}
	static int ZoneAttackers(const Board& board, eColor color, eMobilePiece type)
	{
		return BuildContext(board).attacks.zone_attackers[color][type];
	}
	static int ZoneAttacks(const Board& board, eColor color) { return BuildContext(board).attacks.zone_attacks[color]; }
	static constexpr eSquare KingZoneAnchor(eSquare kingSq) { return Evaluator::KingZoneAnchor(kingSq); }
	static constexpr BITBOARD KingZone(eSquare kingSq, eColor color) { return Evaluator::KingZone(kingSq, color); }
	static constexpr int KingDangerPenalty(int danger) { return Evaluator::KingDangerPenalty(danger); }
	static constexpr int KingDistanceProbe(eSquare a, eSquare b) { return Evaluator::KingDistance(a, b); }
	static constexpr int KingSafetyMaxPenalty = Evaluator::KING_SAFETY_MAX_PENALTY;
	// The pawn-cover half of that bound on its own. A case asserting the shelter
	// tables are REACHABLE has to measure against this, not against the combined
	// figure, which no pawn structure alone can approach.
	static constexpr int KingPawnCoverWorst = Evaluator::KING_PAWN_COVER_WORST;
	static constexpr int KingDangerCap = Evaluator::KING_DANGER_CAP;
	static constexpr int KingDangerDivisor = Evaluator::KING_DANGER_DIVISOR;

	// The scaled-class factors are private tuning parameters; naming them here
	// keeps the tests asserting the classification rather than restating the
	// numbers, which a sweep is expected to change.
	static constexpr int RookAndMinorVsRookScale = Evaluator::ROOK_AND_MINOR_VS_ROOK_SCALE;
	static constexpr int RookVsMinorScale = Evaluator::ROOK_VS_MINOR_SCALE;
	static constexpr int RookVsRookScale = Evaluator::ROOK_VS_ROOK_SCALE;

	// The score before any endgame scale — what Evaluate() would have returned
	// without the classifier. Lets a scaled case assert the discount reached the
	// returned score, rather than only that it was reported in the breakdown.
	static int RawWhitePov(const Board& board) { return Evaluator::RawWhitePov(BuildContext(board)); }

	static ScorePair BishopsPair(const Board& board, eColor color)
	{
		return Evaluator::eval_bishops(BuildContext(board), color);
	}
	static ScorePair CastlingPair(const Board& board, eColor color)
	{
		return Evaluator::eval_castling(BuildContext(board), color);
	}

	static ScorePair MobilityPair(const Board& board, eColor color)
	{
		return Evaluator::eval_mobility(BuildContext(board), color);
	}

	// Raw (mg, eg) pairs, for asserting a tapered term's ENDPOINTS rather
	// than its value at one position's particular phase.
	static ScorePair RooksPair(const Board& board, eColor color)
	{
		return Evaluator::eval_rooks(BuildContext(board), color);
	}
	static ScorePair PstPair(const Board& board, eColor color)
	{
		return Evaluator::eval_pst(BuildContext(board), color);
	}
	static int Phase(const Board& board) { return BuildContext(board).phase; }

	// Named constants, exposed so term-level tests can express exact
	// expected values without duplicating magic numbers.
	static int DoubledPawnPenalty() { return Evaluator::DOUBLED_PAWN_PENALTY; }
	static int IsolatedPawnPenalty() { return Evaluator::ISOLATED_PAWN_PENALTY; }
	static int RookOn7thBonus() { return Evaluator::ROOK_ON_7TH_BONUS; }
	static int OpenFile() { return Evaluator::OPEN_FILE; }
	static int HalfOpenFile() { return Evaluator::HALF_OPEN_FILE; }

  private:
	// Forwards to Evaluator::BuildContext — the production construction
	// site (Eval.cpp) — rather than reimplementing it here. Issue #99 will
	// eventually replace the `11500` phase threshold BuildContext uses
	// internally; having only one construction site means that change can't
	// leave this fixture silently testing the old threshold.
	static EvalContext BuildContext(const Board& board) { return Evaluator::BuildContext(board); }
};
