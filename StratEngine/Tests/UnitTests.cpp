#include "TestFramework.h"
#include "Perft.h"
#include "../Board.h"
#include "../MoveGenerator.h"
#include "../Move.h"

using namespace Testing;

// Test that starting position has correct number of moves
void test_starting_position_moves() {
    Board& board = Board::Instance();
    board.SetDefaultBoard();
    
    GameInfo info = board.GetGameInfo();
    MoveList moves;
    MoveGenerator::ComputeLegalMoves(info, moves);

    TEST_ASSERT_EQUAL(20u, moves.size());
}

// Test perft depth 1 on starting position
void test_perft_depth_1() {
    Board& board = Board::Instance();
    board.SetDefaultBoard();
    
    auto result = Perft::run(board, 1, false);
    TEST_ASSERT_EQUAL(20ull, result.nodes);
}

// Test perft depth 2 on starting position
void test_perft_depth_2() {
    Board& board = Board::Instance();
    board.SetDefaultBoard();
    
    auto result = Perft::run(board, 2, false);
    TEST_ASSERT_EQUAL(400ull, result.nodes);
}

// Test perft depth 3 on starting position
void test_perft_depth_3() {
    Board& board = Board::Instance();
    board.SetDefaultBoard();
    
    auto result = Perft::run(board, 3, false);
    TEST_ASSERT_EQUAL(8902ull, result.nodes);
}

// Test perft depth 4 on starting position (slower - optional)
void test_perft_depth_4() {
    Board& board = Board::Instance();
    board.SetDefaultBoard();
    
    auto result = Perft::run(board, 4, false);
    TEST_ASSERT_EQUAL(197281ull, result.nodes);
}

// Test that move/unmove leaves board in same state
void test_move_unmove_consistency() {
    Board& board = Board::Instance();
    board.SetDefaultBoard();

    GameInfo info = board.GetGameInfo();
    MoveList moves;
    MoveGenerator::ComputeLegalMoves(info, moves);

    for (const auto& move : moves) {
        // Capture initial state
        auto initial_hash = board.GetCurBoardHKey();

        if (board.DoMove(move)) {
            board.UndoMove(move);

            // Check hash is restored
            auto final_hash = board.GetCurBoardHKey();
            TEST_ASSERT_EQUAL(initial_hash, final_hash);
        }
    }
}

int main() {
    reset_test_counters();

    std::cout << "Running Unit Tests\n";
    std::cout << "==================\n\n";

    std::cout << "Test: Starting position has 20 legal moves\n";
    test_starting_position_moves();

    std::cout << "Test: Perft depth 1\n";
    test_perft_depth_1();

    std::cout << "Test: Perft depth 2\n";
    test_perft_depth_2();

    std::cout << "Test: Perft depth 3\n";
    test_perft_depth_3();

    std::cout << "Test: Perft depth 4 (this may take a few seconds)\n";
    test_perft_depth_4();

    std::cout << "Test: Move/Unmove consistency\n";
    test_move_unmove_consistency();

    print_test_summary();

    return all_tests_passed() ? 0 : 1;
}
