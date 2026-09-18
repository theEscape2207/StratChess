// SearchContemptTests.cpp — contempt on search-detected draw scores (#452).
//
// Contempt makes a draw score slightly below equality for the side the engine is playing, so it
// declines a repetition in a position it believes equal. Three things have to hold and none of
// them is visible from a single-colour test:
//
//   * the sign follows the node's SIDE TO MOVE against the root colour, not ply parity. Every
//     sign case below is therefore asserted at the SAME ply with opposite sides to move, which a
//     parity implementation cannot distinguish;
//   * the fabricated values an aborted frame unwinds with stay neutral, because they are not
//     game results;
//   * the transposition table is not allowed to carry a score from one contempt context into a
//     search running under another.
//
// Requires STRAT_ENABLE_TEST_ACCESS. See Docs/TestDesign.md.

#include "SearchTestFixture.h"

namespace {

	// Rook and kings only, so every move below is quiet and reversible: the position can repeat,
	// and the halfmove clock only ever rises.
	constexpr const char* FEN_OSCILLATE_WHITE = "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

	// The same material with BLACK to move and the clock one move short of the fifty-move limit.
	// Five quiet moves from here reach ply 5 with WHITE to move and the clock past 100 — the
	// mirror image of the repetition case below, which reaches ply 5 with BLACK to move.
	constexpr const char* FEN_CLOCK_99_BLACK = "8/8/3k4/8/8/3K4/8/R7 b - - 99 60";

	// Black to move and stalemated: no legal move, not in check.
	constexpr const char* FEN_STALEMATE_BLACK = "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1";

	constexpr int WIDE_ALPHA = -30000;
	constexpr int WIDE_BETA = 30000;

	// Five quiet moves from FEN_OSCILLATE_WHITE: the position after the fifth repeats the one
	// after the first, which is a repetition inside the search tree (RepetitionTests TC9).
	// Ply 5, so the side to move is BLACK.
	int repetition_score(const AIPerlexTestFixture& fix, int contempt, eColor root_color)
	{
		fix.set_contempt(contempt);
		fix.set_root_color(root_color);
		return fix.search_node_after({"a1h1", "d6e6", "h1a1", "e6d6", "a1h1"}, /*depth=*/3, WIDE_ALPHA, WIDE_BETA,
		                             /*is_pv_node=*/false);
	}

	// Five quiet moves from FEN_CLOCK_99_BLACK take the halfmove clock from 99 to 104. Also ply 5,
	// but the side to move is WHITE.
	//
	// The king WALKS rather than oscillating, and the rook's two squares are paired with different
	// king squares, so no position recurs. check_draws() short-circuits `is_repetition(ply) ||
	// clock >= limit` (ThreadData.h), so a shuffle that repeats would be adjudicated by the
	// repetition branch and this helper would never exercise the clock at all.
	int fifty_move_score(const AIPerlexTestFixture& fix, int contempt, eColor root_color)
	{
		fix.set_contempt(contempt);
		fix.set_root_color(root_color);
		return fix.search_node_after({"d6e6", "a1a2", "e6f6", "a2a1", "f6g6"}, /*depth=*/3, WIDE_ALPHA, WIDE_BETA,
		                             /*is_pv_node=*/false);
	}

} // anonymous namespace

TEST_CASE("Contempt - the default of zero leaves every drawn endpoint at GameValues::Draw", "[search][contempt]")
{
	// Guards the helper being handed a non-zero offset at the shipped default. It is NOT evidence
	// that the search is unchanged at contempt 0 — negating zero cannot fail. That claim belongs to
	// Compare-SearchEquivalence.ps1, which compares node counts against the merge base.
	AIPerlexTestFixture repeat(FEN_OSCILLATE_WHITE);
	REQUIRE(repetition_score(repeat, 0, WHITE) == GameValues::Draw);
	REQUIRE(repetition_score(repeat, 0, BLACK) == GameValues::Draw);

	AIPerlexTestFixture clock(FEN_CLOCK_99_BLACK);
	REQUIRE(fifty_move_score(clock, 0, WHITE) == GameValues::Draw);

	AIPerlexTestFixture stale(FEN_STALEMATE_BLACK);
	stale.set_contempt(0);
	stale.set_root_color(WHITE);
	REQUIRE(stale.search_node_after({}, /*depth=*/2, WIDE_ALPHA, WIDE_BETA, /*is_pv_node=*/false) == GameValues::Draw);
}

TEST_CASE("Contempt - a repetition is a loss for the root colour and a gain for its opponent", "[search][contempt]")
{
	AIPerlexTestFixture fix(FEN_OSCILLATE_WHITE);

	// Ply 5, BLACK to move. Root BLACK: the side facing the draw is the one the engine plays, so
	// the draw is scored below equality in that node's own perspective.
	REQUIRE(repetition_score(fix, 25, BLACK) == -25);
	// Same node, same ply, root WHITE: the draw now favours the opponent on move.
	REQUIRE(repetition_score(fix, 25, WHITE) == 25);
}

TEST_CASE("Contempt - the sign follows the side to move, not ply parity", "[search][contempt]")
{
	// The decisive pair: two draws at the SAME ply (5) with OPPOSITE sides to move. A ply-parity
	// implementation returns the same sign for both and fails here. draw_score() takes no ply at
	// all, so this pins the contract rather than the arithmetic.
	AIPerlexTestFixture repeat(FEN_OSCILLATE_WHITE); // ply 5 → BLACK to move
	AIPerlexTestFixture clock(FEN_CLOCK_99_BLACK);   // ply 5 → WHITE to move

	REQUIRE(repetition_score(repeat, 30, WHITE) == 30);
	REQUIRE(fifty_move_score(clock, 30, WHITE) == -30);

	REQUIRE(repetition_score(repeat, 30, BLACK) == -30);
	REQUIRE(fifty_move_score(clock, 30, BLACK) == 30);
}

TEST_CASE("Contempt - the fifty-move rule and stalemate carry it too, not just repetition", "[search][contempt]")
{
	AIPerlexTestFixture clock(FEN_CLOCK_99_BLACK);
	REQUIRE(fifty_move_score(clock, 15, WHITE) == -15);
	REQUIRE(fifty_move_score(clock, 15, BLACK) == 15);

	// Stalemate at ply 0, BLACK to move, reported through adjustScoreForGameState() and cached by
	// the terminal store beside it.
	//
	// A FIXTURE PER ROOT COLOUR, deliberately. That store caches a contempt-tinted EXACT entry, and
	// the guard that would drop it on a colour change lives in Search() — which search_node_after()
	// does not go through, because it calls pvs() directly. Reusing one fixture here would replay
	// the first colour's cached score and assert nothing. Production has no such path: every search
	// enters through Search().
	AIPerlexTestFixture stale_black(FEN_STALEMATE_BLACK);
	stale_black.set_contempt(15);
	stale_black.set_root_color(BLACK);
	REQUIRE(stale_black.search_node_after({}, /*depth=*/2, WIDE_ALPHA, WIDE_BETA, /*is_pv_node=*/false) == -15);

	AIPerlexTestFixture stale_white(FEN_STALEMATE_BLACK);
	stale_white.set_contempt(15);
	stale_white.set_root_color(WHITE);
	REQUIRE(stale_white.search_node_after({}, /*depth=*/2, WIDE_ALPHA, WIDE_BETA, /*is_pv_node=*/false) == 15);
}

TEST_CASE("Contempt - draw_score is a pure function of side to move and root colour", "[search][contempt]")
{
	// The helper on its own, with no search around it. Both boards are the starting position, so
	// the only thing moving is the colour comparison.
	AIPerlexTestFixture fix;
	fix.set_contempt(40);

	fix.set_root_color(WHITE); // starting position: WHITE to move
	REQUIRE(fix.draw_score() == -40);
	fix.set_root_color(BLACK);
	REQUIRE(fix.draw_score() == 40);

	fix.set_contempt(0);
	fix.set_root_color(WHITE);
	REQUIRE(fix.draw_score() == GameValues::Draw);
}

TEST_CASE("Contempt - the fifty-move helper really is adjudicated by the clock", "[search][contempt]")
{
	// Guards the helper above rather than the engine. check_draws() tests repetition first and
	// short-circuits, so a move list that happened to repeat would make every "fifty-move"
	// assertion in this file a second repetition assertion without saying so.
	AIPerlexTestFixture fix(FEN_CLOCK_99_BLACK);
	const Board reached = fix.board_after({"d6e6", "a1a2", "e6f6", "a2a1", "f6g6"});

	REQUIRE(reached.halfmove_clock() >= HALFMOVE_CLOCK_LIMIT);
	REQUIRE_FALSE(reached.is_repetition(5));
	REQUIRE(reached.GetCurrentColor() == WHITE);
}

TEST_CASE("Contempt - fabricated unwind values stay neutral in both phases", "[search][contempt]")
{
	// The abort and time-limit returns are values for a frame that searched nothing, not game
	// results. Contempt must not reach them: a parent discards the value at its unwind guard, and
	// signing it would make an aborted search look like a lost one.
	SECTION("the main search's abort return")
	{
		AIPerlexTestFixture fix(FEN_OSCILLATE_WHITE);
		fix.set_contempt(50);
		fix.set_root_color(WHITE);
		fix.request_stop();

		REQUIRE(fix.search_node_after({"a1h1"}, /*depth=*/4, WIDE_ALPHA, WIDE_BETA, /*is_pv_node=*/false) ==
		        GameValues::Draw);
	}

	SECTION("quiescence's abort return")
	{
		// Its own case, because request_stop() latches IsAborted() and the main search returns at
		// the first of the four fabricated exits — quiescence's two are on a different call path
		// and a change routing them through draw_score() would not fail the section above.
		AIPerlexTestFixture fix(FEN_OSCILLATE_WHITE);
		fix.set_contempt(50);
		fix.set_root_color(WHITE);

		// quiesce_node_aborted(), not request_stop() + quiesce_node(): the latter arms the clock
		// through ApplyLimits(), which clears the latch, and the node would return an ordinary
		// evaluation while the test looked like it was asserting about the abort path.
		REQUIRE(fix.quiesce_node_aborted(WIDE_ALPHA, WIDE_BETA, AIPerlexTestFixture::QSEARCH_BUDGET, /*ply=*/1) ==
		        GameValues::Draw);
	}
}

TEST_CASE("Contempt - the transposition table is cleared when the contempt context changes", "[search][contempt]")
{
	// A contempt-derived draw score propagates into parent entries, so a stored bound depends on
	// the root colour and the contempt value — neither of which the Zobrist key carries.
	const Board white_root(FEN_OSCILLATE_WHITE);
	const Board black_root(FEN_CLOCK_99_BLACK);
	const auto limits = SearchLimits::fixed_depth(2);

	SECTION("a colour flip under non-zero contempt drops the old entries")
	{
		AIPerlexTestFixture fix;
		fix.set_contempt(20);
		(void)fix.ai->Search(white_root, limits);
		fix.store_tt_marker();
		REQUIRE(fix.has_tt_marker());

		(void)fix.ai->Search(black_root, limits);
		REQUIRE_FALSE(fix.has_tt_marker());
	}

	SECTION("a magnitude change under one colour drops them too")
	{
		AIPerlexTestFixture fix;
		fix.set_contempt(20);
		(void)fix.ai->Search(white_root, limits);
		fix.store_tt_marker();

		fix.set_contempt(60);
		(void)fix.ai->Search(white_root, limits);
		REQUIRE_FALSE(fix.has_tt_marker());
	}

	SECTION("turning contempt back off drops the entries it tinted")
	{
		// The case a colour-only guard misses: a nominally neutral search must not consume bounds
		// that a contempt search produced.
		AIPerlexTestFixture fix;
		fix.set_contempt(20);
		(void)fix.ai->Search(white_root, limits);
		fix.store_tt_marker();

		fix.set_contempt(0);
		(void)fix.ai->Search(black_root, limits);
		REQUIRE_FALSE(fix.has_tt_marker());
	}

	SECTION("an unchanged context keeps them")
	{
		AIPerlexTestFixture fix;
		fix.set_contempt(20);
		(void)fix.ai->Search(white_root, limits);
		fix.store_tt_marker();

		(void)fix.ai->Search(white_root, limits);
		REQUIRE(fix.has_tt_marker());
	}

	SECTION("at the shipped default a colour change clears nothing")
	{
		// The guard must be inert at contempt 0: nothing tinted those entries, and clearing on an
		// ordinary colour change would be a behaviour change at the default.
		AIPerlexTestFixture fix;
		fix.set_contempt(0);
		(void)fix.ai->Search(white_root, limits);
		fix.store_tt_marker();

		(void)fix.ai->Search(black_root, limits);
		REQUIRE(fix.has_tt_marker());
	}
}

TEST_CASE("Contempt - SCORE_DROP still rejects an iteration that collapses to a drawn score", "[search][contempt]")
{
	// CASE 4 of assess_iteration_quality() compared against a literal zero. Contempt moves what a
	// draw scores, so the test is a band; at contempt 0 that band is exactly {0}.
	AIPerlexTestFixture fix;
	const Move any = AnyLegalMove();

	AIPerlexTestFixture::Metrics m{};
	m.depth = 4;
	m.current_move = any;
	m.nodes_searched = 5000;
	m.pv_length = 3;
	m.interrupted = true;
	m.move_changed = false;
	m.completion_ratio = 0.5;

	AIPerlexTestFixture::State s{};
	s.depth_completed = 3;
	s.best_score = 300; // abs > score_draw_threshold (20)
	s.nodes_at_completed_depth = 5000;

	SECTION("at contempt 0, the historical equality against zero")
	{
		fix.set_contempt(0);
		m.current_score = 0;
		REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::SCORE_DROP);

		// One centipawn away from a draw is a real score and must not be rejected.
		m.current_score = -1;
		REQUIRE(fix.assess(m, s) != AIPerlexTestFixture::RejectionReason::SCORE_DROP);
	}

	SECTION("at contempt 20, BOTH drawn values are rejected and nothing between them is")
	{
		fix.set_contempt(20);

		// The search-detected draw, tinted.
		m.current_score = -20;
		REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::SCORE_DROP);

		// The eval-detected draw, untinted: Evaluate() returns GameValues::Draw for the dead-drawn
		// material class and contempt never reaches it. Testing only the tinted value would drop
		// this half of the gate at every non-zero contempt — an iteration whose PV liquidates into
		// a provably dead ending would be accepted where it used to be rejected.
		m.current_score = GameValues::Draw;
		REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::SCORE_DROP);

		// Everything strictly between them, and outside them, is a real evaluation and must
		// survive. A band would reject all of these.
		for (const int genuine : {19, -19, -1, -21, 5, -100}) {
			m.current_score = genuine;
			REQUIRE(fix.assess(m, s) != AIPerlexTestFixture::RejectionReason::SCORE_DROP);
		}
	}

	SECTION("at contempt 100, the domain's top, the two drawn values are 0 and -100")
	{
		fix.set_contempt(100);
		for (const int drawn : {0, -100}) {
			m.current_score = drawn;
			REQUIRE(fix.assess(m, s) == AIPerlexTestFixture::RejectionReason::SCORE_DROP);
		}

		for (const int genuine : {-99, -1, 1, 100, -101}) {
			m.current_score = genuine;
			REQUIRE(fix.assess(m, s) != AIPerlexTestFixture::RejectionReason::SCORE_DROP);
		}
	}
}
