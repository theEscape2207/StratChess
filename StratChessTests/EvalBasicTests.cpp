#include "EvalTestFixture.h"
// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("Eval - EvalSimple: starting position is near-symmetric (within 200 cp)", "[eval]")
{
	Board board(FEN_START);

	int score = EvalManager::Create(EvalManager::EvalTypes::SIMPLE)->Evaluate(board);

	REQUIRE(score >= -200);
	REQUIRE(score <= 200);
}

TEST_CASE("Eval - EvalComplex: starting position is near-symmetric (within 200 cp)", "[eval]")
{
	Board board(FEN_START);

	int score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

	REQUIRE(score >= -200);
	REQUIRE(score <= 200);
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
	//
	// Evaluate() now settles a piece-less board through the endgame classifier
	// before any term runs, so this assertion alone no longer reaches the
	// guard. The term-level case further down ("a kingless board reaches the
	// terms") is what keeps it covered.
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
	Board board(FEN_BLACK_EXTRA_QUEEN); // black to move

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

	int simple_score = EvalManager::Create(EvalManager::EvalTypes::SIMPLE)->Evaluate(board);
	int complex_score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

	REQUIRE(simple_score > 0);
	REQUIRE(complex_score > 0);
}

TEST_CASE("Eval - EvalComplex penalises doubled pawns relative to normal structure", "[eval]")
{
	Board board;

	REQUIRE(board.SetupFromFEN(FEN_WHITE_DOUBLED));
	int doubled_score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

	REQUIRE(board.SetupFromFEN(FEN_WHITE_NORMAL));
	int normal_score = EvalManager::Create(EvalManager::EvalTypes::COMPLEX)->Evaluate(board);

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

TEST_CASE("Eval - EvalComplex mop-up: decisively-won pawnless ending scores higher with the losing king cornered",
          "[eval]")
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
