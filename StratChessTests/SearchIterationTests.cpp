// SearchIterationTests.cpp — Catch2 tests for search iteration integration and private helpers:
//   fixed-depth repetition PV search
//   handle_empty_move_emergency() — 3 cases (mate path, emergency path, stale PV row)
//   should_try_null_move()        — 10 cases, one per guard branch (disabled, PV, in-check,
//                                   depth, mate-score, zugzwang, single-piece zugzwang,
//                                   two-piece eligible, consecutive-null, otherwise-eligible)

#include "SearchTestFixture.h"
#include <catch2/catch_test_macros.hpp>
#include "PVIntegrity.h"
#include "PVTable.h"
#include "defines.h"

TEST_CASE("Search keeps the accepted depth-one result when its observer stops the search", "[search][service_api]")
{
	const Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	AIPerplex ai(AIPerplexConfig{.default_depth = 4, .hash_mb = 1, .verbose_logging = false});
	int observations = 0;
	IterationInfo accepted;
	const SearchResult result = ai.Search(board, SearchLimits::fixed_depth(4), [&](const IterationInfo& info) {
		++observations;
		if (info.depth == 1) {
			accepted = info;
			ai.Stop();
		}
	});

	REQUIRE(observations == 1);
	REQUIRE(accepted.depth == 1);
	REQUIRE_FALSE(accepted.pv.empty());
	CHECK(result.best_move == accepted.pv.front());
	CHECK(result.best_score == accepted.score);
	CHECK(result.depth_completed == accepted.depth);
}

// White's only non-losing line is a perpetual check, so every PV ends at a 5-ply repetition.
TEST_CASE("Search - a repetition PV does not end a fixed-depth search early", "[search]")
{
	AIPerlexTestFixture fix("6k1/6p1/8/8/4Q3/2q5/r4PPP/6K1 w - - 0 1");

	REQUIRE(fix.result_to_depth(12).depth_completed == 12);
}

// ============================================================================
// handle_empty_move_emergency tests
// ============================================================================

TEST_CASE("Search - handle_empty_move_emergency: mate score returns false (no move needed)", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position

	AIPerlexTestFixture::State s{};
	s.best_move = Move{};                           // null — no move found
	s.best_score = GameValues::Mate_Threshold + 50; // mate detected

	REQUIRE(fix.emergency(s) == false); // game is over, no move needed
	// best_move remains null — caller must not play
	REQUIRE(s.best_move.is_null());
}

TEST_CASE("Search - handle_empty_move_emergency: non-mate emergency sets a legal move", "[search]")
{
	// default ctor sets up a real, playable starting position so the
	// emergency path finds legal moves
	AIPerlexTestFixture fix;

	AIPerlexTestFixture::State s{};
	s.best_move = Move{}; // null — emergency condition
	s.best_score = 0;     // not a mate score

	const bool result = fix.emergency(s);

	REQUIRE(result == true);         // emergency move was found
	REQUIRE(!s.best_move.is_null()); // a move was set
}

TEST_CASE("Search - handle_empty_move_emergency: a stale row 1 is not spliced onto the emergency move", "[search][pv]")
{
	// PVTable::update copies row ply + 1 onto the end of row ply, and row 1 at this point holds
	// whatever subtree last reached ply 1 — a different position. Without clearing it first the
	// emergency move is published with a tail that describes nothing.
	AIPerlexTestFixture fix;
	fix.seed_pv_row(1, AnyLegalMove());
	REQUIRE(fix.pv_length(1) == 1); // the stale row the emergency path must not read

	AIPerlexTestFixture::State s{};
	s.best_move = Move{};
	s.best_score = 0;

	REQUIRE(fix.emergency(s));

	REQUIRE(fix.pv_length(0) == 1); // exactly the emergency move, nothing spliced on
	REQUIRE(fix.pv_move(0) == s.best_move);
	REQUIRE(pv_replays_legally(fix.board_, fix.pv_line(0)));
}

// ============================================================================
// should_try_null_move tests
// ============================================================================

TEST_CASE("Search - should_try_null_move: disabled returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.set_null_move_enabled(false);

	REQUIRE(fix.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: PV node returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.set_null_move_enabled(true);

	REQUIRE(fix.try_null_move(4, 0, 1, /*is_pv_node=*/true, false) == false);
}

TEST_CASE("Search - should_try_null_move: in check returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.set_null_move_enabled(true);

	REQUIRE(fix.try_null_move(4, 0, 1, false, /*in_check=*/true) == false);
}

TEST_CASE("Search - should_try_null_move: depth below minimum returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.set_null_move_enabled(true);
	fix.set_null_move_min_depth(3);

	REQUIRE(fix.try_null_move(/*depth=*/2, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: mate-score beta returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.set_null_move_enabled(true);

	REQUIRE(fix.try_null_move(4, GameValues::Mate_Threshold, 1, false, false) == false);
	REQUIRE(fix.try_null_move(4, -GameValues::Mate_Threshold, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: zugzwang (no non-pawn material) returns false", "[search]")
{
	// White: king + pawn only. Black: king only. No non-pawn material for
	// the side to move (white) -> zugzwang guard must refuse NMP.
	AIPerlexTestFixture fix("8/8/8/3k4/8/3K4/3P4/8 w - - 0 1");
	fix.set_null_move_enabled(true);

	REQUIRE(fix.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: single non-pawn piece returns false (issue #66)", "[search]")
{
	// QFORK-001 (issue #66): KQ vs KR is won via domination/zugzwang — Black
	// loses only because he must move. Letting the side with a lone rook
	// "pass" makes the null search report that Black holds, hiding the win.
	// The zugzwang guard must refuse NMP whenever the side to move has fewer
	// than two non-pawn pieces.
	AIPerlexTestFixture black_to_move("8/8/8/3r4/4k3/8/8/3QK3 b - - 0 1");
	black_to_move.set_null_move_enabled(true);
	REQUIRE(black_to_move.try_null_move(4, 0, 1, false, false) == false);

	// Same position, White to move: a lone queen is refused too.
	AIPerlexTestFixture white_to_move("8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1");
	white_to_move.set_null_move_enabled(true);
	REQUIRE(white_to_move.try_null_move(4, 0, 1, false, false) == false);

	// One knight + six pawns is still refused: the guard counts non-pawn
	// pieces, deliberately ignoring pawns (material-count-based, not
	// phase-based).
	AIPerlexTestFixture knight_and_pawns("4k3/8/8/8/8/8/PPPPPPN1/4K3 w - - 0 1");
	knight_and_pawns.set_null_move_enabled(true);
	REQUIRE(knight_and_pawns.try_null_move(4, 0, 1, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: two non-pawn pieces returns true", "[search]")
{
	// Queen + knight for the side to move: above the single-piece zugzwang
	// guard threshold, so NMP stays available.
	AIPerlexTestFixture fix("8/8/8/3r4/4k3/8/8/2NQK3 w - - 0 1");
	fix.set_null_move_enabled(true);

	REQUIRE(fix.try_null_move(4, 0, 1, false, false) == true);
}

TEST_CASE("Search - should_try_null_move: consecutive null move returns false", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.set_null_move_enabled(true);
	fix.set_last_move_was_null(2, true); // ply 2 was reached via a null move

	REQUIRE(fix.try_null_move(4, 0, /*ply=*/2, false, false) == false);
}

TEST_CASE("Search - should_try_null_move: otherwise-eligible position returns true", "[search]")
{
	AIPerlexTestFixture fix; // default ctor sets up the starting position
	fix.set_null_move_enabled(true);
	fix.set_null_move_min_depth(3);

	REQUIRE(fix.try_null_move(4, 0, 1, false, false) == true);
}
