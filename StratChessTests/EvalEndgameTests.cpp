#include "EvalTestFixture.h"
// ── Drawish material recognition (issue #128) ─────────────────────────────────
//
// The evaluator scored a bare minor against a lone king at roughly +300 and a
// pair of knights at +580. Those are draws by material whatever the kings are
// doing, so the assertions below are on the exact value rather than on a
// direction: any nonzero score in these classes is the defect.
//
// Every case is stated for both colors and both sides to move. Reading one
// color's counts where the other's is meant is the obvious way to get a
// classifier wrong, and it is invisible in a single-orientation test.

namespace {
	// The exact-draw classes. Kings are placed differently in each entry — a
	// piece-count rule must not care where they stand.
	constexpr const char* kDrawnByMaterialFens[] = {
	    "8/8/8/3k4/8/8/8/3K4 w - - 0 1",    // bare kings
	    "8/8/8/8/8/8/8/K6k w - - 0 1",      // bare kings, both on the back rank
	    "8/8/8/3k4/8/8/3N4/3K4 w - - 0 1",  // K+N vs K
	    "7k/8/8/8/8/8/8/K1N5 w - - 0 1",    // K+N vs K, defender in the corner
	    "8/8/8/3k4/8/8/3B4/3K4 w - - 0 1",  // K+B vs K
	    "7k/8/8/8/4B3/8/8/K7 w - - 0 1",    // K+B vs K, defender in the corner
	    "8/8/8/3k4/8/8/2NN4/3K4 w - - 0 1", // K+NN vs K
	    "7k/8/8/8/3N1N2/8/8/K7 w - - 0 1",  // K+NN vs K, defender in the corner
	};

	// Material that still mates, and must keep its score.
	constexpr const char* kWinningEndgameFens[] = {
	    "8/8/8/3k4/8/8/2BN4/3K4 w - - 0 1",  // K+B+N vs K
	    "8/8/8/3k4/8/8/2BB4/3K4 w - - 0 1",  // K+B+B vs K
	    "8/8/8/3k4/8/8/4R3/3K4 w - - 0 1",   // K+R vs K
	    "8/8/8/3k4/8/8/4Q3/3K4 w - - 0 1",   // K+Q vs K
	    "8/8/8/3k4/8/8/2NNN3/3K4 w - - 0 1", // K+NNN vs K — three knights force mate
	};
} // namespace

TEST_CASE("Eval - material that cannot mate scores exactly a draw", "[eval]")
{
	const char* fen = GENERATE(from_range(kDrawnByMaterialFens));
	CAPTURE(fen);

	const Evaluator eval;

	// Four positions per entry: the attacker as White and as Black, each with
	// either side to move. The mirrored half is what catches a classifier that
	// reads one color's pieces where it means the other's; the side-to-move
	// half is cheap and keeps the cases uniform with the scaled classes to
	// come, where the sign does distinguish them.
	for (const std::string& colored : {std::string(fen), MirrorFen(fen)}) {
		for (const char stm : {'w', 'b'}) {
			std::string position = colored;
			position[position.find(' ') + 1] = stm;
			CAPTURE(position);

			Board board(position);
			REQUIRE(eval.Evaluate(board) == GameValues::Draw);
		}
	}
}

TEST_CASE("Eval - material that still mates keeps its score", "[eval]")
{
	// The complement of the case above, and what stops it being satisfied by
	// scaling every pawnless ending to zero. Each of these is a forced win, so
	// the search must still see the whole material lead.
	const char* fen = GENERATE(from_range(kWinningEndgameFens));
	CAPTURE(fen);

	const Evaluator eval;

	// Mirrored as well, for the same reason the drawn cases are: a classifier
	// that lost a win would do it in one orientation only.
	for (const std::string& colored : {std::string(fen), MirrorFen(fen)}) {
		CAPTURE(colored);
		Board board(colored);

		REQUIRE(eval.Evaluate(board) > 400);
		REQUIRE(eval.Breakdown(board).endgame_adjustment == 0);
	}
}

TEST_CASE("Eval - the defender's pawn keeps a drawn class unscaled", "[eval]")
{
	// K+NN vs K is drawn; K+NN vs K+P is the Troitsky win, because the pawn
	// gives the defender a move and takes the stalemate away. The pawn belongs
	// to the *defender* here, which is the half of the pawn test that the
	// attacker-side case cannot reach.
	Board defenderHasPawn("8/8/8/3k4/8/4p3/2NN4/3K4 w - - 0 1");

	const Evaluator eval;

	REQUIRE(eval.Breakdown(defenderHasPawn).endgame_scale == ENDGAME_SCALE_MAX);
	REQUIRE(eval.Evaluate(defenderHasPawn) > 400);
}

TEST_CASE("Eval - a pawn keeps a lone minor out of the drawn classes", "[eval]")
{
	// K+B+P vs K is won, and it is one capture away from K+B vs K. The pawn is
	// the whole difference, so a classifier looking only at the minors would
	// clamp this to zero — and the engine would see no reason to keep the pawn.
	Board withPawn("8/8/8/3k4/8/4P3/3B4/3K4 w - - 0 1");
	Board withoutPawn("8/8/8/3k4/8/8/3B4/3K4 w - - 0 1");

	const Evaluator eval;

	REQUIRE(eval.Evaluate(withPawn) > 300);
	REQUIRE(eval.Evaluate(withoutPawn) == GameValues::Draw);
}

TEST_CASE("Eval - minors on both sides are left unscaled", "[eval]")
{
	// Deliberately outside the first cut: K+N vs K+B is drawish in practice but
	// not by material, and nothing has measured it. This records the exclusion,
	// so widening the classifier to cover it is a visible change and not a
	// silent one.
	Board knightVsBishop(FEN_MOPUP_MARGINAL_CORNER);

	const Evaluator eval;

	REQUIRE(eval.Breakdown(knightVsBishop).endgame_scale == ENDGAME_SCALE_MAX);
}

// ── Scaled pawnless rook endings (issue #128) ────────────────────────────────
//
// Unlike the classes above these are not draws, so the assertions are on the
// scale and on the direction of the score, never on an exact value: the point
// is that a piece-sized lead is reported as much less than a piece while still
// being reported as a lead.

TEST_CASE("Eval - pawnless rook endings are scaled, not clamped", "[eval]")
{
	struct ScaledCase {
		const char* fen;
		int scale;
	};

	const ScaledCase scaled =
	    GENERATE(ScaledCase{FEN_ROOK_AND_MINOR_VS_ROOK, EvaluatorTestFixture::RookAndMinorVsRookScale},
	             ScaledCase{FEN_ROOK_VS_MINOR, EvaluatorTestFixture::RookVsMinorScale});
	CAPTURE(scaled.fen, scaled.scale);

	const Evaluator eval;

	// Both orientations: MirrorFen flips the side to move as well, so the
	// stronger side is the side to move in each, and the score is positive.
	for (const std::string& colored : {std::string(scaled.fen), MirrorFen(scaled.fen)}) {
		CAPTURE(colored);
		Board board(colored);

		const EvalBreakdown terms = eval.Breakdown(board);
		REQUIRE(terms.endgame_scale == scaled.scale);
		// White-POV, so the adjustment is negative only when White is the side
		// being discounted; the mirror flips its sign along with the score.
		REQUIRE(terms.endgame_adjustment != 0);

		// Still a lead, and strictly smaller than the unscaled evaluator's — the
		// two halves of "scaled rather than clamped". A clamp fails the first
		// assertion; a scale reported in the breakdown but never applied to the
		// returned score fails the second. Both sides are magnitudes: the mirror
		// runs the same case with the signs reversed.
		const int score = eval.Evaluate(board);
		const int raw = std::abs(EvaluatorTestFixture::RawWhitePov(board));
		CAPTURE(score, raw);
		REQUIRE(score > 0);
		REQUIRE(score < raw);
	}
}

TEST_CASE("Eval - rook against rook is discounted, keeping the sign of the positional score", "[eval]")
{
	// The class the other two cannot cover: material is level, so there is no
	// stronger side and no material floor under the score. Everything left is
	// positional, and the assertion is that it survives the scale with its sign
	// intact — a clamp to zero would lose the activity gradient the ending is
	// actually decided by.
	const Evaluator eval;

	for (const std::string& colored : {std::string(FEN_ROOK_VS_ROOK), MirrorFen(FEN_ROOK_VS_ROOK)}) {
		CAPTURE(colored);
		Board board(colored);

		REQUIRE(eval.Breakdown(board).endgame_scale == EvaluatorTestFixture::RookVsRookScale);

		// The fixture FEN is chosen so the unscaled score is not zero; without
		// that the two assertions below would both hold trivially.
		const int raw = EvaluatorTestFixture::RawWhitePov(board);
		REQUIRE(raw != 0);

		// Side-to-move POV against white POV, so compare magnitudes and take the
		// sign from the side to move. MirrorFen flips both, so the mirror runs
		// the same assertions with every sign reversed.
		const int score = eval.Evaluate(board);
		CAPTURE(score, raw);
		REQUIRE(score > 0);
		REQUIRE(score < std::abs(raw));
	}
}

TEST_CASE("Eval - the scaled rook classes are stated as exact counts", "[eval]")
{
	// A second rook or a second minor is a different ending, and a pawn takes
	// the position out of the class entirely. Each of these is one piece away
	// from a scaled case above and must keep its full score.
	const char* fen = GENERATE("4k2r/8/8/8/8/8/3R1R2/4K3 w - - 0 1",   // KRR vs KR
	                           "4k2r/8/8/8/8/5N2/3R1B2/4K3 w - - 0 1", // KR+BN vs KR
	                           "4k1nr/8/8/8/8/5N2/3R4/4K3 w - - 0 1",  // KR+N vs KR+N
	                           "4k2r/8/8/8/8/5N2/3R3P/4K3 w - - 0 1",  // KR+N+P vs KR
	                           "r3k3/3R4/8/8/8/8/7P/4K3 w - - 0 1");   // KR+P vs KR
	CAPTURE(fen);

	const Evaluator eval;

	for (const std::string& colored : {std::string(fen), MirrorFen(fen)}) {
		CAPTURE(colored);
		Board board(colored);
		REQUIRE(eval.Breakdown(board).endgame_scale == ENDGAME_SCALE_MAX);
	}
}

// ── Wrong-coloured-bishop fortress (issue #128) ──────────────────────────────
//
// The one class the classifier decides from a square rather than from a count,
// so it needs the complements a piece-count rule does not: the same material
// with the defending king out of the corner, with the other bishop, and with the
// pawn off the rook file are all still wins and are asserted as such.

TEST_CASE("Eval - the wrong-coloured-bishop fortress scores exactly a draw", "[eval]")
{
	// White's bishop cannot cover the promotion square and the black king is
	// already standing on it. Both rook files, since the promotion squares are
	// opposite colours and the bishop that draws differs between them.
	//
	// The defending king is placed on the promotion square AND on each of the
	// squares beside it: all four attack the promotion square, so the pawn can
	// never get there, and all four must classify as drawn.
	// The a-file cases share one layout — Pa5, Kc5, dark Bd2 — because neither
	// the pawn nor the bishop may attack any of the four corner squares: with a
	// king standing on one, either side to move would otherwise be in check.
	const char* fen = GENERATE("k7/8/8/P1K5/8/8/3B4/8 w - - 0 1",  // a-pawn, a8 light, dark bishop
	                           "1k6/8/8/P1K5/8/8/3B4/8 w - - 0 1", // defending king beside it, on b8
	                           "8/k7/8/P1K5/8/8/3B4/8 w - - 0 1",  // and on a7
	                           "8/1k6/8/P1K5/8/8/3B4/8 w - - 0 1", // and on b7
	                           "k7/8/8/P1K5/P7/8/3B4/8 w - - 0 1", // doubled rook pawns, still one corner
	                           "7k/8/7P/5K2/4B3/8/8/8 w - - 0 1"); // h-pawn, h8 dark, light bishop
	CAPTURE(fen);

	const Evaluator eval;

	for (const std::string& colored : {std::string(fen), MirrorFen(fen)}) {
		for (const char stm : {'w', 'b'}) {
			std::string position = colored;
			position[position.find(' ') + 1] = stm;
			CAPTURE(position);

			Board board(position);

			// Guard the premise: a FEN Board rejects leaves the board empty,
			// which is bare kings, which is a draw — satisfying the assertion
			// below without testing anything.
			REQUIRE(board.GetMaterialScore(WHITE) + board.GetMaterialScore(BLACK) >
			        2 * g_iPieceValues[ePiece::WHITE_KING >> 1]);

			REQUIRE(eval.Evaluate(board) == GameValues::Draw);
		}
	}
}

TEST_CASE("Eval - the fortress condition is the defending king, not the material", "[eval]")
{
	// Each of these is one detail away from the drawn case above and is won.
	// Without them a piece-count-only rule would pass the case above while
	// clamping every one of these to zero.
	const char* fen = GENERATE("4k3/8/P7/2K1B3/8/8/8/8 w - - 0 1",  // king out of the corner
	                           "k7/8/P7/2K2B2/8/8/8/8 w - - 0 1",   // the bishop that covers a8
	                           "k7/8/1P6/2K1B3/8/8/8/8 w - - 0 1",  // knight pawn, not a rook pawn
	                           "k7/8/P7/2K1BB2/8/8/8/8 w - - 0 1"); // a second bishop covers a8
	CAPTURE(fen);

	const Evaluator eval;

	for (const std::string& colored : {std::string(fen), MirrorFen(fen)}) {
		CAPTURE(colored);
		Board board(colored);

		REQUIRE(eval.Breakdown(board).endgame_scale == ENDGAME_SCALE_MAX);
		REQUIRE(eval.Evaluate(board) > 300);
	}
}

TEST_CASE("Eval - a kingless board reaches the terms that guard against it", "[eval]")
{
	// Companion to the whole-position kingless case near the top of this file,
	// which the classifier now short-circuits before any term runs. The defect
	// that case exists for is a NO_SQUARE king square being dereferenced in
	// eval_pst or eval_mopup — asserted here directly, on the two terms that
	// read king_sq, so it stays covered whatever Evaluate() does first.
	//
	// The production path still reaches them: UciHandler::cmd_eval calls
	// Breakdown(), which computes every term regardless of the scale.
	Board board;

	for (const eColor color : {WHITE, BLACK}) {
		CAPTURE(static_cast<int>(color));
		REQUIRE(EvaluatorTestFixture::Pst(board, color) == 0);
		REQUIRE(EvaluatorTestFixture::Mopup(board, color) == 0);
	}
}

TEST_CASE("Eval - Breakdown(): the endgame row accounts for the whole scale", "[eval]")
{
	// The #129 honesty invariant extended to the scale: the rows plus the
	// adjustment must still reproduce `total` exactly. Asserted on a scaled
	// position, where the adjustment is the largest number in the table.
	Board board("8/8/8/3k4/8/8/3N4/3K4 w - - 0 1");

	const Evaluator eval;

	const EvalBreakdown terms = eval.Breakdown(board);

	REQUIRE(terms.endgame_scale == 0);
	REQUIRE(terms.endgame_adjustment != 0);

	const int whitePov = BreakdownWhitePov(terms);

	REQUIRE(terms.total == whitePov);
}
