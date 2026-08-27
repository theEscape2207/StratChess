// SortTests.cpp — Catch2 tests for MoveSorter::ScoreMoves() priority ordering
//
// Tests that moves are scored in the expected priority order:
//   hash move > SEE >= 0 captures and all promotions > killer0 > killer1 > SEE < 0 captures
//   > quiet history
//
// SEE selects the tier and MoveHelper::Value() scores within it. Before SEE the losing tier was
// unreachable in every position, not just this one: Value() weights the attacker at 1/16, so a
// capture's victim contributes at least 100 while the attacker deducts at most 56.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "Sort.h"
#include "MoveFactory.h"
#include "MoveGenerator.h"
#include "MoveHelper.h"
#include "defines.h"
#include <climits>

// Rook endgame: White Ra1, Ke1 vs Black Ra2, Ke8.
// Ra1xa2 is the only capture; all other legal white moves are quiet.
//
// Deliberately promotion-free: the tiers under test are the capture and quiet ones, and a
// promotion belongs to neither. Promotions do not alias in FindScore() below -- Move equality is
// data == rhs.data, the full encoding including flags -- so a test that wants them can add them.
static constexpr const char* FEN_SORT = "4k3/8/8/8/8/8/r7/R3K3 w - - 0 1";

// Helper: find the score assigned to a specific move in out_scored_idx.
// Returns INT_MIN if not found.
static int FindScore(const std::array<std::pair<int, int>, MoveList::MAX_MOVES>& scored_idx, const MoveList& moveList,
                     int n, const Move& target)
{
	for (int i = 0; i < n; ++i) {
		if (moveList[scored_idx[i].second] == target)
			return scored_idx[i].first;
	}
	return INT_MIN;
}

TEST_CASE("Sort - Hash move scores 1'900'000 and sorts first", "[sort]")
{
	// The top tier. There is no tier above it: the previous iteration's PV move had one, it
	// was never supplied, and measurement showed it would name the hash move anyway.
	Board board(FEN_SORT);

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());
	REQUIRE(n > 2);

	const Move hash_move = moveList[1];
	const Move killer0, killer1;
	int32_t history[2][64][64] = {};

	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, hash_move, killer0, killer1, history, scored_idx);

	REQUIRE(FindScore(scored_idx, moveList, n, hash_move) == 1'900'000);
	REQUIRE(moveList[scored_idx[0].second] == hash_move);
}

TEST_CASE("Sort - an empty hash move promotes nothing", "[sort]")
{
	// An empty Move encodes h1 -> h1, which move generation never produces -- so "no hash move"
	// cannot accidentally match a real one. This is the property that made the removed PV tier
	// provably dead rather than merely unused.
	Board board(FEN_SORT);

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());
	REQUIRE(n > 0); // or the loops below would assert nothing and still pass

	// The property itself, asserted rather than inferred from the scores: the sentinel's from
	// and to are the same square, and no generated move's are. The score check alone would
	// still pass under an encoding that broke the sentinel while leaving from == to.
	REQUIRE(Move::EmptyMove().from() == Move::EmptyMove().to());
	for (int i = 0; i < n; ++i)
		REQUIRE(moveList[i].from() != moveList[i].to());

	const Move null_move, killer0, killer1;
	int32_t history[2][64][64] = {};

	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, null_move, killer0, killer1, history, scored_idx);

	for (int i = 0; i < n; ++i)
		REQUIRE(scored_idx[i].first < 1'900'000);
}

TEST_CASE("Sort - Capture scores above 1'000'000 (winning capture category)", "[sort]")
{
	Board board(FEN_SORT);

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());

	// Find Ra1xa2 in the move list
	const auto it =
	    std::find_if(moveList.begin(), moveList.end(), [](const Move& m) { return m.from() == a1 && m.to() == a2; });
	REQUIRE(it != moveList.end());
	const Move capture = *it;

	const Move null_move, killer0, killer1;
	int32_t history[2][64][64] = {};

	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, null_move, killer0, killer1, history, scored_idx);

	const int cap_score = FindScore(scored_idx, moveList, n, capture);
	REQUIRE(cap_score >= 1'000'000); // winning-capture category
}

TEST_CASE("Sort - Killer0 scores 900'000; beats quiet move with no history", "[sort]")
{
	Board board(FEN_SORT);

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());

	// Find a quiet move to designate as killer0 (not the capture a1xa2)
	const auto it = std::find_if(moveList.begin(), moveList.end(), [](const Move& m) {
		return m.from() == a1 && m.to() != a2; // quiet rook move
	});
	REQUIRE(it != moveList.end());
	const Move killer0 = *it;

	// Find a different quiet move (will have history = 0)
	const auto it2 = std::find_if(moveList.begin(), moveList.end(), [&](const Move& m) {
		return m != killer0 && m.from() == e1; // quiet king move
	});
	REQUIRE(it2 != moveList.end());
	const Move quiet_no_history = *it2;

	const Move null_move, killer1;
	int32_t history[2][64][64] = {};

	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, null_move, killer0, killer1, history, scored_idx);

	REQUIRE(FindScore(scored_idx, moveList, n, killer0) == 900'000);
	REQUIRE(FindScore(scored_idx, moveList, n, quiet_no_history) == 0);
	REQUIRE(FindScore(scored_idx, moveList, n, killer0) > FindScore(scored_idx, moveList, n, quiet_no_history));
}

// The whole tier chain in one position, White to move:
//   Ra1xa5 / b4xa5 — the knight is undefended, so both are SEE-winning
//   b4xc5      — the d6 pawn recaptures, so this is SEE-equal (+100 - 100)
//   Qd1xd6     — the c7 pawn recaptures, so this is SEE-losing (+100 - 900)
// The equal capture is here to pin that it shares the top tier with the winning ones rather
// than getting a third tier of its own; a third tier measured 18% more nodes.
// Everything else is quiet.
static constexpr const char* FEN_SEE_TIERS = "6k1/2p5/3p4/n1p5/1P6/8/8/R2QK3 w - - 0 1";

static Move FindBySquares(const MoveList& moveList, eSquare from, eSquare to)
{
	const auto it =
	    std::find_if(moveList.begin(), moveList.end(), [&](const Move& m) { return m.from() == from && m.to() == to; });
	REQUIRE(it != moveList.end());
	return *it;
}

TEST_CASE("Sort - the SEE tier chain, winning capture down to losing capture", "[sort]")
{
	Board board(FEN_SEE_TIERS);

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());

	const Move winning = FindBySquares(moveList, a1, a5); // RxN, undefended
	const Move equal = FindBySquares(moveList, b4, c5);   // PxP, recaptured by a pawn
	const Move losing = FindBySquares(moveList, d1, d6);  // QxP, recaptured by a pawn
	const Move killer0 = FindBySquares(moveList, a1, a4); // quiet rook move
	const Move quiet = FindBySquares(moveList, e1, e2);   // quiet king move, given history below

	const Move null_move, killer1;
	int32_t history[2][64][64] = {};
	history[WHITE][e1][e2] = 500;

	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, null_move, killer0, killer1, history, scored_idx);

	const int winning_score = FindScore(scored_idx, moveList, n, winning);
	const int killer_score = FindScore(scored_idx, moveList, n, killer0);
	const int equal_score = FindScore(scored_idx, moveList, n, equal);
	const int quiet_score = FindScore(scored_idx, moveList, n, quiet);
	const int losing_score = FindScore(scored_idx, moveList, n, losing);

	REQUIRE(winning_score > killer_score);
	REQUIRE(equal_score > killer_score);
	REQUIRE(killer_score > losing_score);
	REQUIRE(losing_score > quiet_score);

	// The tier bases, so a change that reorders the chain by accident is distinguishable from one
	// that reorders it on purpose.
	REQUIRE(winning_score >= 1'000'000);
	REQUIRE(equal_score >= 1'000'000);
	REQUIRE(killer_score == 900'000);
	REQUIRE(losing_score >= 700'000);
	REQUIRE(losing_score < 800'000);
	REQUIRE(quiet_score == 500);
}

TEST_CASE("Sort - two SEE-winning captures order by MVV-LVA within the tier", "[sort]")
{
	// The contract a boolean SEE would silently drop: see_ge cannot separate two captures that
	// share a tier, so MoveHelper::Value() has to. Both take the same undefended knight on a5;
	// the pawn is the cheaper attacker and must come first.
	Board board(FEN_SEE_TIERS);

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());

	const Move by_pawn = FindBySquares(moveList, b4, a5);
	const Move by_rook = FindBySquares(moveList, a1, a5);

	const Move null_move, killer0, killer1;
	int32_t history[2][64][64] = {};

	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, null_move, killer0, killer1, history, scored_idx);

	REQUIRE(FindScore(scored_idx, moveList, n, by_pawn) >= 1'000'000);
	REQUIRE(FindScore(scored_idx, moveList, n, by_rook) >= 1'000'000);
	REQUIRE(FindScore(scored_idx, moveList, n, by_pawn) > FindScore(scored_idx, moveList, n, by_rook));
}

TEST_CASE("Sort - a non-capturing promotion is tactical, not a quiet", "[sort]")
{
	// a7-a8 promotions are not captures, so without a tier of their own they fall through to
	// history and rank against ordinary quiets with no credit for the queen they make. SEE is
	// deliberately not consulted: it would score a promotion onto a defended square below them.
	Board board("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());

	const auto find_promotion = [&](MoveType type) {
		const auto it = std::find_if(moveList.begin(), moveList.end(), [&](const Move& m) {
			return m.from() == a7 && m.to() == a8 && MoveHelper::AsType(m) == type;
		});
		REQUIRE(it != moveList.end());
		return *it;
	};
	const Move queen_promotion = find_promotion(MoveType::PROMOTION_QUEEN);
	const Move knight_promotion = find_promotion(MoveType::PROMOTION_KNIGHT);

	const Move null_move, killer0, killer1;
	int32_t history[2][64][64] = {};

	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, null_move, killer0, killer1, history, scored_idx);

	const int queen_score = FindScore(scored_idx, moveList, n, queen_promotion);
	const int knight_score = FindScore(scored_idx, moveList, n, knight_promotion);

	REQUIRE(queen_score >= 1'000'000);
	REQUIRE(knight_score >= 1'000'000);
	REQUIRE(queen_score > knight_score); // Value() ranks them by promotion gain, with no second rule
	REQUIRE(moveList[scored_idx[0].second] == queen_promotion);
}

TEST_CASE("Sort - Quiet move with positive history scores exactly that history value", "[sort]")
{
	Board board(FEN_SORT);

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(board, moveList);
	const int n = static_cast<int>(moveList.size());

	// Find a quiet king move
	const auto it =
	    std::find_if(moveList.begin(), moveList.end(), [](const Move& m) { return m.from() == e1 && m.to() == d1; });
	REQUIRE(it != moveList.end());
	const Move quiet_move = *it;

	const Move null_move, killer0, killer1;
	int32_t history[2][64][64] = {};
	history[WHITE][e1][d1] = 42; // inject a history score

	std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
	MoveSorter::ScoreMoves(moveList, n, board, WHITE, null_move, killer0, killer1, history, scored_idx);

	REQUIRE(FindScore(scored_idx, moveList, n, quiet_move) == 42);
}
