// SortTests.cpp — Catch2 tests for MoveSorter::ScoreMoves() priority ordering
//
// Tests that moves are scored in the expected priority order:
//   hash move > winning captures > killer0 > killer1 > equal captures
//   > quiet history > losing captures
//
// The last two tiers are unreachable in every position, not just this one: MoveHelper::Value()
// weights the attacker at 1/16, so a capture's victim contributes at least 100 while the attacker
// deducts at most 56, and no capture can score <= 0. SEE (#86) is what populates them.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "Sort.h"
#include "MoveFactory.h"
#include "MoveGenerator.h"
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
