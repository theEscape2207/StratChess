#pragma once

// RepetitionTests.h – Tests for the threefold repetition rule (Board::is_repetition)
//
// Historical bugs (all now fixed):
//
//   BUG-1 [Hash ordering] ✅ FIXED
//     push_position() was called before ChangePlayer() in DoMove(). Pushed hashes
//     carried the mover's side-to-move bit instead of the next player's, so the
//     stored hash could never match the hash present at is_repetition() call time.
//     Fix: push_position() now runs after ChangePlayer() in both DoMove() branches.
//
//   BUG-2 [Loop start offset] ✅ FIXED
//     is_repetition() scanned from history_size-4, giving the wrong same-side parity.
//     Fix: loop start changed to history_size-3.
//
//   BUG-3 [Twofold-in-search never fires] ✅ FIXED
//     `history_ply >= root_ply` was always false (history_ply counted down from
//     history_size-3 while root_ply == history_size-1). Three dead variables removed.
//     Fix: replaced with `i >= history_size - static_cast<size_t>(ply)`.
//
//   BUG-4 [Castling rights not in Zobrist hash] ✅ FIXED
//     update_zobrist_castling() and update_zobrist_ep() were commented out in DoMove().
//     Fix: both calls are now active; castling and EP changes are properly hashed.
//
// All tests are expected to PASS against the current implementation.

#include "TestFramework.h"
#include "../Board.h"
#include "../MoveFactory.h"
#include "../defines.h"

using namespace Testing;

// ────────────────────────────────────────────────────────────────────────────
// FEN constants
// ────────────────────────────────────────────────────────────────────────────

// Rook endgame with no pawns, no castling, no en-passant.
// White: Kd3, Ra1 | Black: Kd6
// One oscillation cycle (4 half-moves): Ra1-h1, Kd6-e6, Rh1-a1, Ke6-d6
static constexpr const char* FEN_ROOK = "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

// Same position but with a white pawn on a2 (used to test irreversibility reset).
static constexpr const char* FEN_ROOK_PAWN = "8/8/3k4/8/8/3K4/P7/R7 w - - 0 1";

// Rook endgame with a capturable black pawn on a5.
// White: Kd3, Ra1 | Black: Kf6, pa5
static constexpr const char* FEN_ROOK_CAPTURE = "8/8/5k2/p7/8/3K4/8/R7 w - - 0 1";

// Full starting-rook layout for castling rights tests.
// White: Ke1, Ra1, Rh1 | Black: Ke8, Ra8, Rh8
static constexpr const char* FEN_CASTLING = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

static void apply_quiet(Board& board, eSquare from, eSquare to, ePiece piece)
{
    auto m = MoveFactory::MakeQuiet(from, to, piece);
    if (!board.DoMove(m))
        std::cerr << "SETUP ERROR: DoMove rejected quiet move from "
                  << from << " to " << to << "\n";
}

// One full oscillation cycle for FEN_ROOK — starts with WHITE's move.
// Rook: a1↔h1, Black king: d6↔e6. Returns board to the starting position.
// Precondition: white to move.
static void oscillate_cycle(Board& board)
{
    apply_quiet(board, a1, h1, WHITE_ROOK);
    apply_quiet(board, d6, e6, BLACK_KING);
    apply_quiet(board, h1, a1, WHITE_ROOK);
    apply_quiet(board, e6, d6, BLACK_KING);
}

// One full oscillation cycle for FEN_ROOK — starts with BLACK's move.
// Used after an irreversible white move (pawn advance, capture) so that
// the very next move is correctly black's.
// Precondition: black to move.
static void oscillate_cycle_black_first(Board& board)
{
    apply_quiet(board, d6, e6, BLACK_KING);
    apply_quiet(board, a1, h1, WHITE_ROOK);
    apply_quiet(board, e6, d6, BLACK_KING);
    apply_quiet(board, h1, a1, WHITE_ROOK);
}

// ────────────────────────────────────────────────────────────────────────────
// TC1 – Early-exit: history_size < 4 always returns false          [PASS]
// ────────────────────────────────────────────────────────────────────────────
void test_rep_small_history()
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    apply_quiet(board, a1, h1, WHITE_ROOK);
    TEST_ASSERT(!board.is_repetition(1), "1 half-move: history_size==1, must return false");

    apply_quiet(board, d6, e6, BLACK_KING);
    TEST_ASSERT(!board.is_repetition(1), "2 half-moves: history_size==2, must return false");

    apply_quiet(board, h1, a1, WHITE_ROOK);
    TEST_ASSERT(!board.is_repetition(1), "3 half-moves: history_size==3, must return false");
}

// ────────────────────────────────────────────────────────────────────────────
// TC2 – Twofold from game history is not a draw                     [PASS]
//
// After one oscillation cycle (4 half-moves) the initial position has occurred
// twice. Because both occurrences are in game history (not the search tree),
// threefold is required and is_repetition must return false.
// ────────────────────────────────────────────────────────────────────────────
void test_rep_twofold_not_draw()
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    oscillate_cycle(board); // position seen twice: initial + after cycle

    TEST_ASSERT(!board.is_repetition(1),
        "twofold from game history is not a draw; must return false");
}

// ────────────────────────────────────────────────────────────────────────────
// TC3 – Threefold repetition is detected                                    [PASS]
//
// The initial position is never pushed to position_history_ — only DoMove()
// pushes. So the first copy of the starting state in history appears after
// cycle 1; the second after cycle 2. Three cycles are therefore required to
// have two prior occurrences in history (indices n and n+4) plus the current
// one (index n+8), making threefold detectable. is_repetition must return true.
// ────────────────────────────────────────────────────────────────────────────
void test_rep_threefold_detected()
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    oscillate_cycle(board); // starting position now in history for the 1st time
    oscillate_cycle(board); // 2nd time in history
    oscillate_cycle(board); // 3rd time — threefold

    TEST_ASSERT(board.is_repetition(1),
        "threefold repetition must be a draw");
}

// ────────────────────────────────────────────────────────────────────────────
// TC4 – Post-pawn oscillation is detected as threefold                      [PASS]
//
// After white advances the a-pawn it is black's turn, so the oscillation must
// start with a black move (oscillate_cycle_black_first is used here).
//
// The post-pawn position (pawn on a3, rook on a1, kings on d3/d6) repeats
// three times across three cycles and must be detected as a draw.
// This also validates that last_irreversible_ply_ is set correctly: the scan
// stops at the pawn move boundary and cannot accidentally match any pre-pawn
// entries (which would have pawn on a2 in the hash anyway).
// ────────────────────────────────────────────────────────────────────────────
void test_rep_reset_by_pawn_move()
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK_PAWN);

    // White plays pawn a2→a3 (irreversible). It is now black's turn.
    apply_quiet(board, a2, a3, WHITE_PAWN);

    // Three black-first cycles. Each returns to (Pa3, Ra1, Kd3, Kd6, black to move).
    oscillate_cycle_black_first(board); // 1st copy of post-pawn base in history
    oscillate_cycle_black_first(board); // 2nd copy
    oscillate_cycle_black_first(board); // 3rd copy — threefold

    TEST_ASSERT(board.is_repetition(1),
        "post-pawn threefold must be detected");
}

// ────────────────────────────────────────────────────────────────────────────
// TC5 – Post-capture oscillation is detected as threefold                   [PASS]
//
// After white captures the pawn on a5 it is black's turn, so the oscillation
// starts with a black move (Kf6-e6) to respect the side-to-move invariant.
//
// The post-capture position (Ra5, Kd3, Kf6) repeats three times and must be
// detected as a draw. last_irreversible_ply_ is correctly set by the capture.
// ────────────────────────────────────────────────────────────────────────────
void test_rep_reset_by_capture()
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK_CAPTURE);

    // White captures Ra1xa5 (irreversible). It is now black's turn.
    auto capture = MoveFactory::MakeCapture(a1, a5, WHITE_ROOK, BLACK_PAWN);
    if (!board.DoMove(capture))
    {
        std::cerr << "SETUP ERROR: capture Ra1xa5 rejected\n";
        return;
    }

    // Three black-first cycles: Kf6↔e6, Ra5↔h5.
    // Each ends with white's rook on a5, black to move — the same post-capture base.
    for (int i = 0; i < 3; ++i)
    {
        apply_quiet(board, f6, e6, BLACK_KING);  // black moves first
        apply_quiet(board, a5, h5, WHITE_ROOK);
        apply_quiet(board, e6, f6, BLACK_KING);
        apply_quiet(board, h5, a5, WHITE_ROOK);
    }

    TEST_ASSERT(board.is_repetition(1),
        "post-capture threefold must be detected");
}

// ────────────────────────────────────────────────────────────────────────────
// TC6 – Castling rights change prevents false positive                      [PASS]
//
// After both a-rooks move off their home squares the initial castling rights
// are permanently lost. The piece configuration returns to its starting layout
// after each cycle, but the game state is different (kq only vs KQkq).
//
// With BUG-4 fixed, castling-rights changes are included in the Zobrist hash,
// so the pre-loss and post-loss positions hash to different values. After two
// cycles only the post-loss position has repeated (twofold, not threefold).
// is_repetition must return false — no false positive.
// ────────────────────────────────────────────────────────────────────────────
void test_rep_castling_rights_no_false_positive()
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CASTLING);

    // Move white's a-rook and black's a-rook: both sides lose Q-side castling
    apply_quiet(board, a1, a2, WHITE_ROOK);
    apply_quiet(board, a8, a7, BLACK_ROOK);
    apply_quiet(board, a2, a1, WHITE_ROOK); // pieces back, but castling rights gone
    apply_quiet(board, a7, a8, BLACK_ROOK);

    // Repeat the same rook oscillation (second occurrence of the post-loss position)
    apply_quiet(board, a1, a2, WHITE_ROOK);
    apply_quiet(board, a8, a7, BLACK_ROOK);
    apply_quiet(board, a2, a1, WHITE_ROOK);
    apply_quiet(board, a7, a8, BLACK_ROOK);

    // The initial position (KQkq rights) has only appeared once; no threefold.
    // The post-rights-loss position has appeared twice; still not threefold.
    TEST_ASSERT(!board.is_repetition(1),
        "castling rights change must produce a different hash; no false positive");
}

// ────────────────────────────────────────────────────────────────────────────
// TC7 – UndoMove fully restores repetition state                    [PASS]
//
// After DoMove+UndoMove for each move in a full oscillation cycle, the board's
// repetition tracking must be identical to a freshly set-up board.
// ────────────────────────────────────────────────────────────────────────────
void test_rep_undo_restores_state()
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    auto m1 = MoveFactory::MakeQuiet(a1, h1, WHITE_ROOK);
    auto m2 = MoveFactory::MakeQuiet(d6, e6, BLACK_KING);
    auto m3 = MoveFactory::MakeQuiet(h1, a1, WHITE_ROOK);
    auto m4 = MoveFactory::MakeQuiet(e6, d6, BLACK_KING);

    board.DoMove(m1); board.DoMove(m2); board.DoMove(m3); board.DoMove(m4);
    board.UndoMove(m4); board.UndoMove(m3); board.UndoMove(m2); board.UndoMove(m1);

    // After a complete undo sequence the board is back at the start position.
    // is_repetition must behave as if no moves have been made.
    TEST_ASSERT(!board.is_repetition(1),
        "after full undo the repetition state must match a freshly set-up board");
}

// ────────────────────────────────────────────────────────────────────────────
// TC9 – Twofold repetition within the search tree is a draw               [PASS]
//
// When both occurrences of a position are entirely within the current search
// (neither comes from game history), twofold is sufficient for a draw.
// The engine can force the repetition by simply repeating the same moves.
//
// Setup: no game-history moves — all 5 moves below are "search moves".
//
//   Ply 1: Ra1-h1  → history[0] = hash(Rh1, Kd3, Kd6, black to move)
//   Ply 2: Kd6-e6
//   Ply 3: Rh1-a1
//   Ply 4: Ke6-d6  → board back to root position
//   Ply 5: Ra1-h1  → history[4] == history[0]  ← twofold within search
//
// is_repetition(5): search_root_index = history_size(5) - ply(5) = 0.
// Entry at index 0 satisfies i >= 0 (in search tree) and repetitions == 1
// → returns true (BUG-3 fixed).
// ────────────────────────────────────────────────────────────────────────────
void test_rep_twofold_in_search()
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    // Five search-depth moves. No game history; all entries are in-search.
    apply_quiet(board, a1, h1, WHITE_ROOK); // ply 1
    apply_quiet(board, d6, e6, BLACK_KING); // ply 2
    apply_quiet(board, h1, a1, WHITE_ROOK); // ply 3
    apply_quiet(board, e6, d6, BLACK_KING); // ply 4 — root position revisited
    apply_quiet(board, a1, h1, WHITE_ROOK); // ply 5 — same as ply 1

    // Passing ply=5 tells is_repetition that all 5 history entries were added
    // during the current search (history_size - ply = 5 - 5 = 0 = search root).
    TEST_ASSERT(board.is_repetition(5),
        "twofold entirely within search tree must be a draw");
}

// ────────────────────────────────────────────────────────────────────────────
// Entry point
// ────────────────────────────────────────────────────────────────────────────
int repetition_tests_entry()
{
    reset_test_counters();

    std::cout << "Running Repetition Rule Tests\n";
    std::cout << "=============================\n\n";

    std::cout << "TC1: Small history early-exit\n";
    test_rep_small_history();

    std::cout << "TC2: Twofold from game history is not a draw\n";
    test_rep_twofold_not_draw();

    std::cout << "TC3: Threefold detected\n";
    test_rep_threefold_detected();

    std::cout << "TC4: Post-pawn threefold detected\n";
    test_rep_reset_by_pawn_move();

    std::cout << "TC5: Post-capture threefold detected\n";
    test_rep_reset_by_capture();

    std::cout << "TC6: Castling rights change, no false positive\n";
    test_rep_castling_rights_no_false_positive();

    std::cout << "TC7: UndoMove fully restores repetition state\n";
    test_rep_undo_restores_state();

    std::cout << "TC9: Twofold repetition within search tree is a draw\n";
    test_rep_twofold_in_search();

    print_test_summary();
    return all_tests_passed() ? 0 : 1;
}
