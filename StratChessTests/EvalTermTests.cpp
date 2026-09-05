#include "EvalTestFixture.h"

TEST_CASE("Eval - eval_pawns: pawns with no isolation and no doubling score exactly 0", "[eval]")
{
	Board board(FEN_WHITE_NORMAL);

	REQUIRE(EvaluatorTestFixture::Pawns(board, WHITE) == 0);
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

	REQUIRE(EvaluatorTestFixture::Rooks(knightOn, WHITE) == EvaluatorTestFixture::Rooks(knightOff, WHITE));
}

TEST_CASE("Eval - eval_rooks: an own pawn behind the rook leaves the file fully open", "[eval]")
{
	// Pins a deliberate scope decision: the rook is fixed on
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

	REQUIRE(EvaluatorTestFixture::Rooks(pawnOffFile, WHITE) == EvaluatorTestFixture::Rooks(pawnBehindRook, WHITE));
}

// ── eval_mobility (issues #98, #113) ─────────────────────────────────────────

TEST_CASE("Eval - eval_mobility: a central knight outscores a cornered one", "[eval]")
{
	// The canonical mobility case: a knight on d4 reaches 8 squares, one on a1
	// reaches 2. Nothing else differs between the positions.
	//
	// The idle Black h7 pawn is what keeps K+N vs K off the board: that class is
	// scored 0 by EndgameScale, and BuildContext generates no attacks at all for
	// a dead-drawn class, so every mobility count would be 0. It covers only g6,
	// which neither knight reaches, so it changes nothing else. Same reason in
	// the two cases below.
	Board central("4k3/7p/8/8/3N4/8/8/4K3 w - - 0 1");
	Board cornered("4k3/7p/8/8/8/8/8/N3K3 w - - 0 1");

	REQUIRE(EvaluatorTestFixture::Mobility(central, WHITE) > EvaluatorTestFixture::Mobility(cornered, WHITE));
}

TEST_CASE("Eval - eval_mobility: a rook on an open file outscores one boxed in behind its own pawns", "[eval]")
{
	Board open("4k3/8/8/8/8/8/8/3RK3 w - - 0 1");
	Board boxed("4k3/8/8/8/8/8/3PPP2/3RK3 w - - 0 1");

	REQUIRE(EvaluatorTestFixture::Mobility(open, WHITE) > EvaluatorTestFixture::Mobility(boxed, WHITE));
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
	// The h7 pawn stands in both and covers only g6, out of the knight's reach.
	Board unguarded("4k3/7p/8/8/3N4/8/8/4K3 w - - 0 1");
	Board guarded("4k3/3p3p/8/8/3N4/8/8/4K3 w - - 0 1");

	REQUIRE(EvaluatorTestFixture::Mobility(guarded, WHITE) < EvaluatorTestFixture::Mobility(unguarded, WHITE));
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

	REQUIRE(EvaluatorTestFixture::Mobility(whiteSide, WHITE) == EvaluatorTestFixture::Mobility(blackSide, BLACK));

	// ...and the mask must actually be biting, or the equality above is vacuous.
	// Idle pawns again, mirrored: Black's h7 covers g6, White's h2 covers g3,
	// and neither knight reaches either square.
	Board whiteUnmasked("4k3/7p/8/3N4/8/8/8/4K3 w - - 0 1");
	Board blackUnmasked("4k3/8/8/8/3n4/8/7P/4K3 b - - 0 1");
	REQUIRE(EvaluatorTestFixture::Mobility(whiteSide, WHITE) < EvaluatorTestFixture::Mobility(whiteUnmasked, WHITE));
	REQUIRE(EvaluatorTestFixture::Mobility(blackSide, BLACK) < EvaluatorTestFixture::Mobility(blackUnmasked, BLACK));
}

TEST_CASE("Eval - eval_mobility: own pieces block, enemy pieces are capture targets", "[eval]")
{
	// Pins the D3 convention: `attacks & ~occupied[own]`, so an enemy piece on a
	// reachable square still counts (it can be captured) while an own piece does
	// not. A bishop on c1 with the b2 square occupied either way isolates it.
	Board ownBlocker("4k3/8/8/8/8/8/1P6/2B1K3 w - - 0 1");
	Board enemyBlocker("4k3/8/8/8/8/8/1p6/2B1K3 w - - 0 1");

	REQUIRE(EvaluatorTestFixture::Mobility(enemyBlocker, WHITE) > EvaluatorTestFixture::Mobility(ownBlocker, WHITE));
}

TEST_CASE("Eval - eval_mobility: the queen is scored, not skipped (issue #113)", "[eval]")
{
	// #113 exists so the queen is not left out if mobility scopes down to cheap
	// pieces. A lone queen must produce a nonzero term.
	Board queen("4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1");

	REQUIRE(EvaluatorTestFixture::Mobility(queen, WHITE) > 0);
}

TEST_CASE("Eval - eval_mobility: a bare king contributes nothing", "[eval]")
{
	// The king is deliberately excluded -- king mobility belongs to #97, where
	// it can be weighed against attacker counts rather than paid per square.
	//
	// The idle pawn is load-bearing: bare kings are a dead-drawn class, and the
	// attack pass is skipped entirely for one, so a bare-kings position would
	// score 0 whatever the generation loop does -- including if it started
	// counting the king. With the pawn the loop actually runs, and only its
	// piece selection can produce the 0.
	Board kings("4k3/7p/8/8/8/8/8/4K3 w - - 0 1");

	REQUIRE(EvaluatorTestFixture::Mobility(kings, WHITE) == 0);
	REQUIRE(EvaluatorTestFixture::Mobility(kings, BLACK) == 0);
}

TEST_CASE("Eval - eval_mobility: a dead-drawn material class is not counted at all", "[eval]")
{
	// BuildContext skips the whole attack generation pass when endgame_scale is
	// 0, because Evaluate() returns GameValues::Draw for such a position without
	// consulting a single term. The two positions differ only by an idle Black
	// pawn on h7 -- which covers nothing the knight reaches, so the ONLY thing
	// separating a full count from no count is the material class.
	//
	// Pinned as a test because it is otherwise invisible: nothing the engine
	// plays can see it, and a later change generating attacks unconditionally
	// would put the cost back on exactly the endings the early-out exists for.
	Board deadDrawn("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
	Board scorable("4k3/7p/8/8/3N4/8/8/4K3 w - - 0 1");

	REQUIRE(EvaluatorTestFixture::Mobility(deadDrawn, WHITE) == 0);
	REQUIRE(EvaluatorTestFixture::Mobility(scorable, WHITE) > 0);
}

TEST_CASE("Eval - eval_pawns: doubled and isolated a-file pawns score exactly -(doubled + 2*isolated)", "[eval]")
{
	// FEN_WHITE_DOUBLED: White Pa2 + Pa3, no b-file pawn. Both pawns are
	// isolated (the only neighbouring file, b, has no White pawn); the lower
	// pawn (a2) additionally has a3 in its forward mask, so exactly one
	// doubled penalty applies — only the pawn with another pawn strictly
	// ahead of it on the same file triggers that check.
	Board board(FEN_WHITE_DOUBLED);

	const int expected =
	    -(EvaluatorTestFixture::DoubledPawnPenalty() + 2 * EvaluatorTestFixture::IsolatedPawnPenalty());
	REQUIRE(EvaluatorTestFixture::Pawns(board, WHITE) == expected);
}

TEST_CASE("Eval - eval_rooks: the 7th-rank bonus is endgame-weighted, the file bonus is not", "[eval]")
{
	// FEN_ROOK_ON_7TH: White Re7 alone against a bare king. The fully open
	// file (no pawns of either colour) is phase-independent and so appears at
	// both endpoints; the 7th-rank bonus is endgame-weighted (D3, issue #99)
	// and so appears only at eg. Asserting the endpoints rather than the
	// blended value keeps this independent of the position's own phase.
	Board board(FEN_ROOK_ON_7TH);

	const ScorePair rooks = EvaluatorTestFixture::RooksPair(board, WHITE);

	REQUIRE(rooks.mg == EvaluatorTestFixture::OpenFile());
	REQUIRE(rooks.eg == EvaluatorTestFixture::OpenFile() + EvaluatorTestFixture::RookOn7thBonus());
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

	const int expected = EvaluatorTestFixture::HalfOpenFile() +
	                     (EvaluatorTestFixture::OpenFile() - EvaluatorTestFixture::HalfOpenFile());
	REQUIRE(EvaluatorTestFixture::Rooks(knightOn, WHITE) == expected);
	REQUIRE(EvaluatorTestFixture::Rooks(knightOff, WHITE) == expected);
}

TEST_CASE("Eval - eval_rooks: term-level D5 own-pawn-behind result is exactly equal, not just directionally so",
          "[eval]")
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

	const int expected = EvaluatorTestFixture::OpenFile();
	REQUIRE(EvaluatorTestFixture::Rooks(pawnOffFile, WHITE) == expected);
	REQUIRE(EvaluatorTestFixture::Rooks(pawnBehindRook, WHITE) == expected);
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

	const ScorePair pst = EvaluatorTestFixture::PstPair(board, WHITE);

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

	const ScorePair pst = EvaluatorTestFixture::PstPair(board, WHITE);
	const int phase = EvaluatorTestFixture::Phase(board);
	CAPTURE(pst.mg, pst.eg, phase);

	// Two queens a side and nothing else: 4 + 4 + 4 + 4 = 16.
	REQUIRE(phase == 16);
	REQUIRE(EvaluatorTestFixture::Pst(board, WHITE) == BlendPhase(pst, phase));
}

// ── eval_bishops (issue #111) ────────────────────────────────────────────────

TEST_CASE("Eval - eval_bishops: pair requires opposite square colours", "[eval]")
{
	SECTION("Two bishops on opposite colours score the pair")
	{
		// White Bc1 (dark) + Bf1 (light); Black has one bishop only.
		Board board("4k3/8/8/8/8/8/8/2B1KB2 w - - 0 1");
		const ScorePair pair = EvaluatorTestFixture::BishopsPair(board, WHITE);
		CHECK(pair.mg > 0);
		CHECK(pair.eg > pair.mg); // worth more as the board opens
	}
	SECTION("Two bishops on the SAME colour are not a pair")
	{
		// Bc1 and Ba3 are both dark squares — reachable by underpromotion.
		// A plain popcount >= 2 would wrongly pay here.
		Board board("4k3/8/8/8/8/B7/8/2B1K3 w - - 0 1");
		const ScorePair pair = EvaluatorTestFixture::BishopsPair(board, WHITE);
		CHECK(pair.mg == 0);
		CHECK(pair.eg == 0);
	}
	SECTION("A single bishop scores nothing")
	{
		Board board("4k3/8/8/8/8/8/8/2B1K3 w - - 0 1");
		CHECK(EvaluatorTestFixture::Bishops(board, WHITE) == 0);
	}
	SECTION("Kingless board is safe and scores nothing")
	{
		Board board;
		CHECK(EvaluatorTestFixture::Bishops(board, WHITE) == 0);
		CHECK(EvaluatorTestFixture::Bishops(board, BLACK) == 0);
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

	const int connectedMg = EvaluatorTestFixture::RooksPair(connected, WHITE).mg;
	const int blockedMg = EvaluatorTestFixture::RooksPair(blocked, WHITE).mg;

	CHECK(connectedMg > blockedMg);
}

TEST_CASE("Eval - eval_rooks: a lone rook is never connected", "[eval]")
{
	Board one("4k3/8/8/8/8/8/8/R6K w - - 0 1");
	Board two("4k3/8/8/8/8/8/8/R2R3K w - - 0 1");

	CHECK(EvaluatorTestFixture::RooksPair(two, WHITE).mg > EvaluatorTestFixture::RooksPair(one, WHITE).mg);
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
	const ScorePair rooks = EvaluatorTestFixture::RooksPair(board, WHITE);

	CHECK(rooks.mg == 75);
	CHECK(rooks.eg == 61);
}

// ── eval_castling (issue #115) ───────────────────────────────────────────────

TEST_CASE("Eval - eval_castling: silent while any castling right remains", "[eval]")
{
	// Rights present => the side has decided nothing, so no bonus and no penalty.
	Board board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
	CHECK(EvaluatorTestFixture::CastlingPair(board, WHITE).mg == 0);
	CHECK(EvaluatorTestFixture::CastlingPair(board, BLACK).mg == 0);
}

TEST_CASE("Eval - eval_castling: bonus once rights are gone and the king is tucked away", "[eval]")
{
	// White king on g1 with no rights left — the castled-kingside picture.
	Board board("4k3/8/8/8/8/8/8/5RK1 w - - 0 1");
	const ScorePair pair = EvaluatorTestFixture::CastlingPair(board, WHITE);
	CHECK(pair.mg > 0);
	CHECK(pair.eg == 0); // middlegame-only; the endgame king wants the centre
}

TEST_CASE("Eval - eval_castling: penalty when rights are lost with the king still central", "[eval]")
{
	// King on e1, no rights — lost the option without ever castling.
	Board board("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
	CHECK(EvaluatorTestFixture::CastlingPair(board, WHITE).mg < 0);
}

TEST_CASE("Eval - eval_castling: both corners count, not just the kingside one", "[eval]")
{
	// a1 is the queenside analogue of h1. An earlier form tested
	// `file >= 1 && file <= 2` for the queenside, which covered b1 and c1 but
	// silently excluded a1 -- so Kh1 scored the bonus while its mirror Ka1 took
	// the penalty, a 45 cp mg swing on a square that Kb1-a1 reaches routinely in
	// opposite-side-castling Sicilians.
	for (const char* fen : {"4k3/8/8/8/8/8/8/K7 w - - 0 1",    // a1, queenside corner
	                        "4k3/8/8/8/8/8/8/1K6 w - - 0 1",   // b1
	                        "4k3/8/8/8/8/8/8/2K5 w - - 0 1",   // c1
	                        "4k3/8/8/8/8/8/8/6K1 w - - 0 1",   // g1
	                        "4k3/8/8/8/8/8/8/7K w - - 0 1"}) { // h1, kingside corner
		CAPTURE(fen);
		Board board(fen);
		CHECK(EvaluatorTestFixture::CastlingPair(board, WHITE).mg > 0);
	}
}

TEST_CASE("Eval - eval_castling: the two corners score identically", "[eval]")
{
	Board kingside("4k3/8/8/8/8/8/8/7K w - - 0 1");
	Board queenside("4k3/8/8/8/8/8/8/K7 w - - 0 1");

	CHECK(EvaluatorTestFixture::CastlingPair(kingside, WHITE).mg ==
	      EvaluatorTestFixture::CastlingPair(queenside, WHITE).mg);
}

TEST_CASE("Eval - eval_castling: the f-file is neutral, softening the one-step cliff", "[eval]")
{
	// A binary sheltered/exposed test made Kg1->Kf1 swing the full bonus-to-
	// penalty distance on one quiet king move, with nothing to smooth it -- the
	// middlegame king PST is flat, so this term is the only square-based signal
	// there. f scores zero instead, halving the worst single-step swing.
	Board f1("4k3/8/8/8/8/8/8/5K2 w - - 0 1");
	Board g1("4k3/8/8/8/8/8/8/6K1 w - - 0 1");
	Board e1("4k3/8/8/8/8/8/8/4K3 w - - 0 1");

	CHECK(EvaluatorTestFixture::CastlingPair(f1, WHITE).mg == 0);
	CHECK(EvaluatorTestFixture::CastlingPair(g1, WHITE).mg > 0);
	CHECK(EvaluatorTestFixture::CastlingPair(e1, WHITE).mg < 0);
}

TEST_CASE("Eval - eval_castling: a king off its home rank never counts as castled", "[eval]")
{
	// g2, not g1: the king has walked, so the shelter picture does not apply.
	Board board("4k3/8/8/8/8/8/6K1/8 w - - 0 1");
	CHECK(EvaluatorTestFixture::CastlingPair(board, WHITE).mg < 0);
}

TEST_CASE("Eval - eval_castling: kingless board is safe", "[eval]")
{
	// Default-constructed Board has no king; castling_rights defaults to ALL,
	// so this also exercises the rights-present early return.
	Board board;
	CHECK(EvaluatorTestFixture::Castling(board, WHITE) == 0);
	CHECK(EvaluatorTestFixture::Castling(board, BLACK) == 0);
}

TEST_CASE("Eval - eval_mopup: only the winning color receives a nonzero contribution", "[eval]")
{
	// FEN_MOPUP_LOSER_KING_CORNER: White K+Q vs Black K+R, pawnless, White
	// winning by 400 cp (the decisive-material threshold).
	Board board(FEN_MOPUP_LOSER_KING_CORNER);

	REQUIRE(EvaluatorTestFixture::Mopup(board, WHITE) > 0);
	REQUIRE(EvaluatorTestFixture::Mopup(board, BLACK) == 0);
}

TEST_CASE("Eval - eval_mopup: gated off for both colors below the decisive material threshold", "[eval]")
{
	// FEN_MOPUP_MARGINAL_CORNER: K+N vs K+B, materially equal — below the
	// 400 cp threshold, so neither color should get a contribution.
	Board board(FEN_MOPUP_MARGINAL_CORNER);

	REQUIRE(EvaluatorTestFixture::Mopup(board, WHITE) == 0);
	REQUIRE(EvaluatorTestFixture::Mopup(board, BLACK) == 0);
}

TEST_CASE("Eval - the per-term functions sum exactly to Evaluator::Evaluate()'s result", "[eval]")
{
	// Structural regression check: rebuilds Evaluate()'s side-to-move-relative
	// formula from the independently-tested terms plus raw material, and
	// confirms it matches Evaluate() itself across every whole-position FEN
	// already used for the color-symmetry cases above. Guards against the
	// term wrappers drifting out of sync with what Evaluate() actually calls.
	const char* fen = GENERATE(from_range(kSymmetryFens));
	CAPTURE(fen);

	Board board(fen);
	const Evaluator eval;

	const int matWhite = board.GetMaterialScore(WHITE);
	const int matBlack = board.GetMaterialScore(BLACK);

	const int bonusWhite =
	    EvaluatorTestFixture::Pawns(board, WHITE) + EvaluatorTestFixture::Rooks(board, WHITE) +
	    EvaluatorTestFixture::Pst(board, WHITE) + EvaluatorTestFixture::Mopup(board, WHITE) +
	    EvaluatorTestFixture::Bishops(board, WHITE) + EvaluatorTestFixture::Castling(board, WHITE) +
	    EvaluatorTestFixture::Mobility(board, WHITE) + EvaluatorTestFixture::KingShelter(board, WHITE) +
	    EvaluatorTestFixture::KingStorm(board, WHITE) + EvaluatorTestFixture::KingFiles(board, WHITE) +
	    EvaluatorTestFixture::KingAttack(board, WHITE);
	const int bonusBlack =
	    EvaluatorTestFixture::Pawns(board, BLACK) + EvaluatorTestFixture::Rooks(board, BLACK) +
	    EvaluatorTestFixture::Pst(board, BLACK) + EvaluatorTestFixture::Mopup(board, BLACK) +
	    EvaluatorTestFixture::Bishops(board, BLACK) + EvaluatorTestFixture::Castling(board, BLACK) +
	    EvaluatorTestFixture::Mobility(board, BLACK) + EvaluatorTestFixture::KingShelter(board, BLACK) +
	    EvaluatorTestFixture::KingStorm(board, BLACK) + EvaluatorTestFixture::KingFiles(board, BLACK) +
	    EvaluatorTestFixture::KingAttack(board, BLACK);

	// The endgame scale is not a term, so it cannot be rebuilt from the term
	// functions; it is taken from the breakdown, whose own agreement with
	// Evaluate() is asserted separately.
	const int adjustment = eval.Breakdown(board).endgame_adjustment;

	const eColor toMove = board.GetCurrentColor();
	const int whitePov = (matWhite + bonusWhite) - (matBlack + bonusBlack) + adjustment;
	const int expected = (toMove == WHITE) ? whitePov : -whitePov;

	REQUIRE(eval.Evaluate(board) == expected);
}

// ── Evaluator::Breakdown() (issue #129 phase 2) ───────────────────────────────
//
// Breakdown() is the public, production path to the per-term values that the
// UCI 'eval' command prints. The tests below tie it to the terms that are
// already individually asserted above, rather than testing it in isolation —
// the failure mode worth guarding is Breakdown() quietly reporting something
// other than what Evaluate() sums, which no amount of self-consistent output
// would reveal.

TEST_CASE("Eval - Breakdown(): every row equals the term function it reports", "[eval]")
{
	const char* fen = GENERATE(from_range(kSymmetryFens));
	CAPTURE(fen);

	Board board(fen);
	const Evaluator eval;

	const EvalBreakdown terms = eval.Breakdown(board);

	for (const eColor color : {WHITE, BLACK}) {
		CAPTURE(static_cast<int>(color));
		REQUIRE(terms.material[color] == board.GetMaterialScore(color));
		REQUIRE(terms.pawns[color] == EvaluatorTestFixture::Pawns(board, color));
		REQUIRE(terms.rooks[color] == EvaluatorTestFixture::Rooks(board, color));
		REQUIRE(terms.pst[color] == EvaluatorTestFixture::Pst(board, color));
		REQUIRE(terms.mopup[color] == EvaluatorTestFixture::Mopup(board, color));
		REQUIRE(terms.bishops[color] == EvaluatorTestFixture::Bishops(board, color));
		REQUIRE(terms.castling[color] == EvaluatorTestFixture::Castling(board, color));
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
	const Evaluator eval;

	const EvalBreakdown terms = eval.Breakdown(board);

	REQUIRE(terms.total == eval.Evaluate(board));

	const int whitePov = BreakdownWhitePov(terms);

	const int expectedTotal = (board.GetCurrentColor() == WHITE) ? whitePov : -whitePov;
	REQUIRE(terms.total == expectedTotal);
}

TEST_CASE("Eval - Breakdown(): phase matches the context Evaluate() builds", "[eval]")
{
	// Phase is reported because it is not derivable from the rows, and it is
	// what places every tapered term between its mg and eg endpoints. Asserting
	// it against Breakdown() keeps the reported value pinned to the one
	// construction site (BuildContext) rather than to a duplicated formula.
	const Evaluator eval;

	SECTION("full starting material is MAX_GAME_PHASE")
	{
		Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
		REQUIRE(eval.Breakdown(board).phase == MAX_GAME_PHASE);
	}

	SECTION("bare kings plus a queen is the queen's weight alone")
	{
		Board board("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
		REQUIRE(eval.Breakdown(board).phase == 4);
	}

	SECTION("bare kings are phase 0")
	{
		Board board("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
		REQUIRE(eval.Breakdown(board).phase == 0);
	}

	SECTION("pawns do not contribute to phase")
	{
		// Same pieces as the bare-kings case plus a full pawn set: phase must
		// not move, or the taper would drift with pawn trades rather than with
		// the piece material that actually defines the game's stage.
		Board board("4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - - 0 1");
		REQUIRE(eval.Breakdown(board).phase == 0);
	}

	SECTION("promotion overshoot clamps rather than extrapolating")
	{
		// Three queens plus a rook per side: raw phase 2*(12 + 2) = 28, i.e.
		// strictly ABOVE MAX_GAME_PHASE, so the clamp is actually exercised.
		// (Three queens a side alone is exactly 24 and would leave the clamp a
		// no-op — the test would then pass with the clamp deleted.)
		Board board("qqq1k2r/8/8/8/8/8/8/QQQ1K2R w - - 0 1");
		REQUIRE(eval.Breakdown(board).phase == MAX_GAME_PHASE);
	}
}
