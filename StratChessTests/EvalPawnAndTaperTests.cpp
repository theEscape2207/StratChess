#include "EvalTestFixture.h"
// ── Tapering behaviour (issue #99) ───────────────────────────────────────────

TEST_CASE("Eval - BlendPhase is exact at both endpoints", "[eval]")
{
	// The classic tapering bug is an off-by-one that makes neither endpoint
	// reproduce its own input, so assert both directly rather than inferring
	// them from a blended position.
	const ScorePair s{100, -40};

	REQUIRE(BlendPhase(s, MAX_GAME_PHASE) == 100);
	REQUIRE(BlendPhase(s, 0) == -40);
}

TEST_CASE("Eval - BlendPhase moves monotonically between the endpoints", "[eval]")
{
	// Deliberately NOT a pair that divides evenly by MAX_GAME_PHASE: {240, 0}
	// gives exactly 10*phase at every step and so never truncates, which would
	// let an arbitrarily-rounding implementation pass. This pair crosses zero
	// and divides unevenly, exercising truncation in both directions.
	const ScorePair s{100, -40};

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
	Board opening("3qk2r/8/8/8/3K4/8/8/3Q4 w - - 0 1"); // queens + a rook: high phase
	Board ending("4k3/8/8/8/3K4/8/8/8 w - - 0 1");      // bare kings: phase 0

	auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
	auto* complexEval = dynamic_cast<EvalComplex*>(eval.get());
	REQUIRE(complexEval != nullptr);

	const EvalBreakdown high = complexEval->Breakdown(opening);
	const EvalBreakdown low = complexEval->Breakdown(ending);

	// Subtract White's queen PST explicitly rather than relying on it being 0.
	// It happens to be 0 on d1 today, but #117 is a PST-tuning issue: a queen
	// table change would otherwise silently turn this into a test of the queen.
	const int highKingOnly = high.pst[WHITE] - EvalProbe::GetPositionalScore(d1, WHITE_QUEEN);
	const int lowKingOnly = low.pst[WHITE];
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
	const int netAfter = (a.pst[WHITE] - a.pst[BLACK]);
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
	// Pawnless K+Q vs K+R: a 400 cp lead (exactly MOPUP_MATERIAL_THRESHOLD), so
	// mop-up is gated on. The Black king is cornered on a8; White's king moves
	// from d4 to c5, strictly closer to it (Chebyshev 4 -> 3) and no other piece
	// moves.
	//
	// The defending ROOK is load-bearing: it is what keeps the item 5 gate open
	// here, and with mop-up off eval_pst stops suppressing the winner's
	// centralizing king table, so approaching the corner would cost centipawns
	// instead of earning them.
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
	const int farTotal = farBreakdown.pst[WHITE] + farBreakdown.mopup[WHITE];
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
	for (const int sq : {c5, c6, c7, c8, d5, d6, d7, d8, e5, e6, e7, e8})
		expected |= (1ULL << sq);

	REQUIRE(g_bbPassedMaskWhite[d4] == expected);
}

TEST_CASE("PassedMask - black d5 covers exactly the c/d/e files ahead", "[eval]")
{
	BITBOARD expected = 0;
	for (const int sq : {c4, c3, c2, c1, d4, d3, d2, d1, e4, e3, e2, e1})
		expected |= (1ULL << sq);

	REQUIRE(g_bbPassedMaskBlack[d5] == expected);
}

TEST_CASE("PassedMask - edge files do not wrap around the board", "[eval]")
{
	// The classic generator bug: shifting a file mask sideways wraps a2's span
	// onto the h-file. Built with explicit file bounds instead, so assert it.
	BITBOARD hFile = 0;
	for (const int sq : {h1, h2, h3, h4, h5, h6, h7, h8})
		hFile |= (1ULL << sq);
	BITBOARD aFile = 0;
	for (const int sq : {a1, a2, a3, a4, a5, a6, a7, a8})
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

	const int passedScore = EvalComplexTestFixture::Pawns(passed, WHITE);
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

	const int thirdScore = EvalComplexTestFixture::Pawns(third, WHITE);
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
	const int endgameScore = EvalComplexTestFixture::Pawns(endgame, WHITE);

	CAPTURE(middlegameScore, endgameScore);
	REQUIRE(endgameScore > middlegameScore);
}

TEST_CASE("Eval - EvalComplex detects an a-file passer", "[eval]")
{
	// Guards the wraparound case end to end: a black h-pawn must not stop the
	// white a-pawn from counting as passed, while a black b-pawn must.
	Board aFilePasser("4k3/7p/8/P7/8/8/8/4K3 w - - 0 1");
	Board aFileBlocked("4k3/1p6/8/P7/8/8/8/4K3 w - - 0 1");

	const int passerScore = EvalComplexTestFixture::Pawns(aFilePasser, WHITE);
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

	const int backwardsScore = EvalComplexTestFixture::Pawns(backwards, WHITE);
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

	const int levelScore = EvalComplexTestFixture::Pawns(levelNeighbour, WHITE);
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
	    "k7/8/8/8/8/8/4P3/4K3 w - - 0 1", // e2
	    "k7/8/8/8/8/4P3/8/4K3 w - - 0 1", // e3
	    "k7/8/8/8/4P3/8/8/4K3 w - - 0 1", // e4
	    "k7/8/8/4P3/8/8/8/4K3 w - - 0 1", // e5
	    "k7/8/4P3/8/8/8/8/4K3 w - - 0 1", // e6
	    "k7/4P3/8/8/8/8/8/4K3 w - - 0 1", // e7
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
	    "8/8/8/8/8/4k3/4P3/4K3 w - - 0 1", // e2, king e3
	    "8/8/8/8/4k3/4P3/8/4K3 w - - 0 1", // e3, king e4
	    "8/8/8/4k3/4P3/8/8/4K3 w - - 0 1", // e4, king e5
	    "8/8/4k3/4P3/8/8/8/4K3 w - - 0 1", // e5, king e6
	    "8/4k3/4P3/8/8/8/8/4K3 w - - 0 1", // e6, king e7
	    "4k3/4P3/8/8/8/8/8/4K3 w - - 0 1", // e7, king e8
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
	const int freeScore = EvalComplexTestFixture::Pawns(freeToRun, WHITE);

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

	const int doubledScore = EvalComplexTestFixture::Pawns(doubled, WHITE);
	const int separatedScore = EvalComplexTestFixture::Pawns(separated, WHITE);

	// Separated wins by two passer bonuses against one, minus the doubled and
	// isolated penalties -- a margin far larger than the 10 cp that separated
	// them when the rear pawn was scored as a passer too.
	CAPTURE(doubledScore, separatedScore);
	REQUIRE(separatedScore > doubledScore + 50);
}

TEST_CASE("Eval - EvalComplex mop-up: gated on the defender's force, not its phase", "[eval]")
{
	// Q+R vs Q is the case issue #118 item 5 named: a lone queen is phase 4 and
	// passed the retired `loser phase <= 6` gate, so the winner was paid for
	// chasing a king its opponent could always check away from.
	Board defendingQueen(FEN_MOPUP_DEFENDER_HAS_QUEEN);
	for (const eColor color : {WHITE, BLACK})
		REQUIRE(EvalComplexTestFixture::Mopup(defendingQueen, color) == 0);

	// The complement, and what stops the gate being satisfied by switching
	// mop-up off for every defended ending: a rook cannot check the winner's
	// king away for ever, so K+Q vs K+R is won by cornering and keeps its
	// mop-up.
	Board defendingRook(FEN_MOPUP_LOSER_KING_CORNER);
	REQUIRE(EvalComplexTestFixture::Mopup(defendingRook, WHITE) > 0);
	REQUIRE(EvalComplexTestFixture::Mopup(defendingRook, BLACK) == 0);
}
