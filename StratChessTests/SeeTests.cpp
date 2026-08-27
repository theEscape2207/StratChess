// SeeTests.cpp — Catch2 tests for See::see_ge()
//
// Every position here is hand-computed, and each is asserted as a bracket: see_ge(m, v) is true
// and see_ge(m, v + 1) is false, which pins the exact exchange value through a boolean interface.
// A one-sided assertion would pass against a see_ge that returned a constant.
//
// The cases are chosen for the mistakes hand-picked cases usually miss — an x-ray battery, an
// en-passant victim that is not on the destination square, and both halves of the rule that a
// king attacker terminates the swap.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "MoveGenerator.h"
#include "MoveHelper.h"
#include "See.h"
#include "defines.h"
#include <algorithm>

namespace {

	Move FindMove(const Board& board, eSquare from, eSquare to)
	{
		MoveList list;
		MoveGenerator::ComputeLegalMoves(board, list);
		const auto it =
		    std::find_if(list.begin(), list.end(), [&](const Move& m) { return m.from() == from && m.to() == to; });
		REQUIRE(it != list.end());
		return *it;
	}

	// Pins the exact static exchange value through the boolean interface.
	void RequireSeeExactly(const Board& board, const Move& move, int expected)
	{
		REQUIRE(See::see_ge(board, move, expected));
		REQUIRE_FALSE(See::see_ge(board, move, expected + 1));
	}

} // namespace

TEST_CASE("See - a capture nothing defends is worth the victim", "[see]")
{
	// Rd1xd5 takes an undefended queen.
	const Board board("4k3/8/8/3q4/8/8/8/3RK3 w - - 0 1");
	RequireSeeExactly(board, FindMove(board, d1, d5), 900);
}

TEST_CASE("See - QxP defended by a pawn loses the queen", "[see]")
{
	// The case #306 made reachable: quiescence generates officer captures, and delta pruning
	// cannot filter this one — it prunes captures that cannot raise alpha, not captures that lose.
	const Board board("4k3/8/2p5/3p4/8/8/8/3QK3 w - - 0 1");
	RequireSeeExactly(board, FindMove(board, d1, d5), 100 - 900);
}

TEST_CASE("See - RxN defended by a pawn loses the exchange", "[see]")
{
	const Board board("4k3/8/2p5/3n4/8/8/8/3RK3 w - - 0 1");
	RequireSeeExactly(board, FindMove(board, d1, d5), 300 - 500);
}

TEST_CASE("See - an x-ray battery is counted: rook behind rook", "[see]")
{
	// Both sides have doubled rooks on the d-file, against a pawn on d5:
	// Rd2xd5, Rd7xd5, Rd1xd5, Rd8xd5 = 100 - 500 + 500 - 500 = -400.
	//
	// The black back rook is the one that matters. Rd8 does not attack d5 while Rd7 stands in
	// front of it, so it only enters the swap list when the recapture vacates d7 — an x-ray the
	// initial occupancy cannot supply, unlike the white battery, whose back rook is uncovered by
	// the root move itself. Blind to it, SEE stops after Rd1xd5 and reports +100: a winning
	// capture rather than a losing one.
	const Board board("3rk3/3r4/8/3p4/8/8/3R4/3RK3 w - - 0 1");
	const Move move = FindMove(board, d2, d5);

	RequireSeeExactly(board, move, 100 - 500 + 500 - 500);
	REQUIRE_FALSE(See::see_ge(board, move, 1)); // the tier a battery-blind SEE would get wrong
}

TEST_CASE("See - en passant removes the victim, which is not on the destination square", "[see]")
{
	// exd6 e.p. takes the d5 pawn, and Rd4 can only reach d6 through the square that pawn
	// vacates. A SEE that toggled move.to() instead of clearing the victim would leave the rook
	// blocked and report +100 rather than the true 0.
	const Board board("4k3/8/8/3pP3/3r4/8/8/4K3 w - d6 0 1");
	const Move move = FindMove(board, e5, d6);
	REQUIRE(MoveHelper::IsEnPassant(move));

	RequireSeeExactly(board, move, 100 - 100);
}

TEST_CASE("See - a king capture onto a defended square is losing", "[see]")
{
	// Kd1xd2 with the pawn defended by c3. The swap must terminate on the king attacker rather
	// than enter the arithmetic, where its 10000 cp notional value would dominate the list.
	const Board board("4k3/8/8/8/8/2p5/3p4/3K4 w - - 0 1");
	REQUIRE_FALSE(See::see_ge(board, FindMove(board, d1, d2), 0));
}

TEST_CASE("See - a king capture nothing defends keeps the victim", "[see]")
{
	const Board board("4k3/8/8/8/8/8/3p4/3K4 w - - 0 1");
	RequireSeeExactly(board, FindMove(board, d1, d2), 100);
}

TEST_CASE("See - a king recapture the opponent still attacks is unavailable", "[see]")
{
	// Rd1xd7 is defended only by the black king, and Bh3 still bears on d7, so Kxd7 is illegal.
	// The exchange therefore ends one capture earlier than the swap list assumed: +100, not -400.
	const Board with_second_attacker("4k3/3p4/8/8/8/7B/8/3RK3 w - - 0 1");
	RequireSeeExactly(with_second_attacker, FindMove(with_second_attacker, d1, d7), 100);

	// The same capture without the bishop: the king recapture is legal and White loses the rook.
	const Board unaided("4k3/3p4/8/8/8/8/8/3RK3 w - - 0 1");
	RequireSeeExactly(unaided, FindMove(unaided, d1, d7), 100 - 500);
}

TEST_CASE("See - a promotion is credited once, at the root of the swap list", "[see]")
{
	// b7xa8=Q takes a rook and makes a queen; the black king on b8 recaptures on a8.
	// +500 (rook) + 800 (promotion gain) - 900 (the new queen) = +400.
	const Board board("rk6/1P6/8/8/8/8/8/4K3 w - - 0 1");

	MoveList list;
	MoveGenerator::ComputeLegalMoves(board, list);
	const auto it = std::find_if(list.begin(), list.end(), [](const Move& m) {
		return m.from() == b7 && m.to() == a8 && MoveHelper::AsType(m) == MoveType::PROMOTION_QUEEN_CAPTURE;
	});
	REQUIRE(it != list.end());

	RequireSeeExactly(board, *it, 500 + 800 - 900);
}
