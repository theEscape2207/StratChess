#include "EvalTestFixture.h"
// ── Color-mirroring correctness (issue #125) ──────────────────────────────────
//
// Exposes Evaluator's protected mirroring/PST helpers for direct testing —
// no production visibility change; both stay protected on Evaluator. Also
// used by the term-level tests below (issue #127 restructure) to compute an
// independently-derived expected PST value.
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
	for (int sq = a8; sq < NUM_SQUARES; ++sq) {
		const auto square = static_cast<eSquare>(sq);
		CAPTURE(sq);
		REQUIRE(File(EvalProbe::getEvalBoard(BLACK_QUEEN, square)) == File(square));
	}
}

TEST_CASE("Eval - getEvalBoard inverts rank for every square (Black)", "[eval]")
{
	for (int sq = a8; sq < NUM_SQUARES; ++sq) {
		const auto square = static_cast<eSquare>(sq);
		CAPTURE(sq);
		REQUIRE(Rank(EvalProbe::getEvalBoard(BLACK_QUEEN, square)) == 7 - Rank(square));
	}
}

TEST_CASE("Eval - getEvalBoard leaves White squares unchanged for every square", "[eval]")
{
	for (int sq = a8; sq < NUM_SQUARES; ++sq) {
		const auto square = static_cast<eSquare>(sq);
		CAPTURE(sq);
		REQUIRE(EvalProbe::getEvalBoard(WHITE_QUEEN, square) == square);
	}
}

// ── FEN color-mirror helper (test scaffolding only — not engine functionality) ─
//
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
	REQUIRE(MirrorFen("rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2") ==
	        "rnbqkbnr/pppp1ppp/8/4p3/2P5/8/PP1PPPPP/RNBQKBNR b KQkq c3 0 2");

	// Partial rights must swap colour, not just pass through: White keeps only
	// kingside, Black only queenside, so the mirror must invert that pairing.
	REQUIRE(MirrorFen("r3k3/8/8/8/8/8/8/4K2R w Kq - 5 30") == "4k2r/8/8/8/8/8/8/R3K3 b Qk - 5 30");
}

// ── Whole-position color symmetry (issue #125) ────────────────────────────────
//
// Evaluate() is side-to-move-relative: it scores from the perspective of
// whichever color is on move. Mirroring swaps which color is on move along
// with the position, so the mover faces an identical relative situation in
// both the original and the mirror — the scores must be EQUAL, not negated.
// This is the single most likely thing for a future reader to get backwards.

TEST_CASE("Eval - color symmetry: a position and its mirror score equally", "[eval]")
{
	const char* fen = GENERATE(from_range(kSymmetryFens));
	CAPTURE(fen);

	const Evaluator eval;
	Board board(fen);
	Board mirrored(MirrorFen(fen));

	REQUIRE(eval.Evaluate(board) == eval.Evaluate(mirrored));
}

// ── Term-level tests (issue #127 restructure) ─────────────────────────────────
//
// Evaluator::Evaluate() is now a thin context-build-and-sum wrapper around
// four private per-term functions (eval_pawns, eval_rooks, eval_pst,
// eval_mopup), each taking (const EvalContext&, eColor) and returning that
// color's contribution only.
// The term accessors return each term BLENDED at the position's own phase
// (issue #99) — the value that term actually contributes to Evaluate() there.
// Endpoint behaviour (mg vs eg) is asserted separately by the tapering tests,
// which drive phase directly rather than inferring it.
//
