// SortTests.cpp — Catch2 tests for MoveSorter::ScoreMoves() priority ordering
//
// Tests that moves are scored in the expected priority order:
//   PV move > hash move > winning captures > killer0 > killer1 > equal captures
//   > quiet history > losing captures (none in this position)

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "Sort.h"
#include "MoveFactory.h"
#include "MoveGenerator.h"
#include "defines.h"

// Rook endgame: White Ra1, Ke1 vs Black Ra2, Ke8.
// Ra1xa2 is the only capture; all other legal white moves are quiet.
static constexpr const char* FEN_SORT =
    "4k3/8/8/8/8/8/r7/R3K3 w - - 0 1";

// Helper: find the score assigned to a specific move in out_scored_idx.
// Returns INT_MIN if not found.
static int FindScore(
    const std::array<std::pair<int,int>, MoveList::MAX_MOVES>& scored_idx,
    const MoveList& moveList,
    int n,
    const Move& target)
{
    for (int i = 0; i < n; ++i) {
        if (moveList[scored_idx[i].second] == target)
            return scored_idx[i].first;
    }
    return INT_MIN;
}

TEST_CASE("Sort - PV move scores 2'000'000", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());
    REQUIRE(n > 1);

    // Pick any legal move as the PV move (use the first one)
    const Move pv_move  = moveList[0];
    const Move null_move;
    const Move killer0, killer1;                       // null — no killers set
    int32_t history[2][64][64] = {};

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           pv_move, null_move,
                           killer0, killer1,
                           history, scored_idx);

    const int pv_score = FindScore(scored_idx, moveList, n, pv_move);
    REQUIRE(pv_score == 2'000'000);
    // PV move should be first after sorting
    REQUIRE(moveList[scored_idx[0].second] == pv_move);
}

TEST_CASE("Sort - Hash move scores 1'900'000 when not the PV move", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());
    REQUIRE(n > 2);

    const Move pv_move   = moveList[0];
    const Move hash_move = moveList[1]; // different from pv_move
    const Move killer0, killer1;
    int32_t history[2][64][64] = {};

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           pv_move, hash_move,
                           killer0, killer1,
                           history, scored_idx);

    REQUIRE(FindScore(scored_idx, moveList, n, hash_move) == 1'900'000);
}

TEST_CASE("Sort - Capture scores above 1'000'000 (winning capture category)", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());

    // Find Ra1xa2 in the move list
    const auto it = std::find_if(moveList.begin(), moveList.end(), [](const Move& m) {
        return m.from() == a1 && m.to() == a2;
    });
    REQUIRE(it != moveList.end());
    const Move capture = *it;

    const Move null_move, killer0, killer1;
    int32_t history[2][64][64] = {};

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           null_move, null_move,
                           killer0, killer1,
                           history, scored_idx);

    const int cap_score = FindScore(scored_idx, moveList, n, capture);
    REQUIRE(cap_score >= 1'000'000); // winning-capture category
}

TEST_CASE("Sort - Killer0 scores 900'000; beats quiet move with no history", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
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

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           null_move, null_move,
                           killer0, killer1,
                           history, scored_idx);

    REQUIRE(FindScore(scored_idx, moveList, n, killer0)          == 900'000);
    REQUIRE(FindScore(scored_idx, moveList, n, quiet_no_history) == 0);
    REQUIRE(FindScore(scored_idx, moveList, n, killer0) >
            FindScore(scored_idx, moveList, n, quiet_no_history));
}

TEST_CASE("Sort - Quiet move with positive history scores exactly that history value", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());

    // Find a quiet king move
    const auto it = std::find_if(moveList.begin(), moveList.end(), [](const Move& m) {
        return m.from() == e1 && m.to() == d1;
    });
    REQUIRE(it != moveList.end());
    const Move quiet_move = *it;

    const Move null_move, killer0, killer1;
    int32_t history[2][64][64] = {};
    history[WHITE][e1][d1] = 42; // inject a history score

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           null_move, null_move,
                           killer0, killer1,
                           history, scored_idx);

    REQUIRE(FindScore(scored_idx, moveList, n, quiet_move) == 42);
}
