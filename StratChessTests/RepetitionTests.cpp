// RepetitionTests.cpp — Catch2 test suite for Board::is_repetition()
//
// Migrated from StratEngine/Tests/RepetitionTests.h.
// Each TEST_CASE calls SetupFromFEN() for Board isolation.
//
// Historical bugs verified fixed (see RepetitionTests.h header for details):
//   BUG-1: Hash ordering — push_position() after change_player()
//   BUG-2: Loop start offset — history_size-3, not history_size-4
//   BUG-3: Twofold-in-search — fixed index comparison
//   BUG-4: Castling rights and en-passant included in Zobrist hash

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "MoveFactory.h"
#include "defines.h"

// ── FEN constants ─────────────────────────────────────────────────────────────

static constexpr const char* FEN_ROOK =
    "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

static constexpr const char* FEN_ROOK_PAWN =
    "8/8/3k4/8/8/3K4/P7/R7 w - - 0 1";

static constexpr const char* FEN_ROOK_CAPTURE =
    "8/8/5k2/p7/8/3K4/8/R7 w - - 0 1";

static constexpr const char* FEN_CASTLING =
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// ── Local helpers ─────────────────────────────────────────────────────────────

namespace {

// Phase 3: MakeQuiet no longer stores the moving piece; just pass from/to.
void apply_quiet(Board& board, eSquare from, eSquare to)
{
    auto m = MoveFactory::MakeQuiet(from, to);
    REQUIRE(board.DoMove(m)); // fail fast on bad setup move
}

// One oscillation cycle starting with WHITE. Ra1<->h1, Kd6<->e6.
void oscillate_cycle(Board& board)
{
    apply_quiet(board, a1, h1);
    apply_quiet(board, d6, e6);
    apply_quiet(board, h1, a1);
    apply_quiet(board, e6, d6);
}

// One oscillation cycle starting with BLACK (used after an irreversible white move).
void oscillate_cycle_black_first(Board& board)
{
    apply_quiet(board, d6, e6);
    apply_quiet(board, a1, h1);
    apply_quiet(board, e6, d6);
    apply_quiet(board, h1, a1);
}

} // anonymous namespace

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("TC1 - Small history: history_size < 4 always returns false", "[repetition]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    apply_quiet(board, a1, h1);
    REQUIRE_FALSE(board.is_repetition(1));

    apply_quiet(board, d6, e6);
    REQUIRE_FALSE(board.is_repetition(1));

    apply_quiet(board, h1, a1);
    REQUIRE_FALSE(board.is_repetition(1));
}

TEST_CASE("TC2 - Twofold from game history is not a draw", "[repetition]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    oscillate_cycle(board); // position seen twice: initial + after cycle

    REQUIRE_FALSE(board.is_repetition(1));
}

TEST_CASE("TC3 - Threefold repetition is detected", "[repetition]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    oscillate_cycle(board); // 1st copy of starting state in history
    oscillate_cycle(board); // 2nd copy
    oscillate_cycle(board); // 3rd copy — threefold

    REQUIRE(board.is_repetition(1));
}

TEST_CASE("TC4 - Post-pawn threefold is detected; pawn move resets scan boundary", "[repetition]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK_PAWN);

    apply_quiet(board, a2, a3); // irreversible — black to move next

    oscillate_cycle_black_first(board); // 1st copy of post-pawn position in history
    oscillate_cycle_black_first(board); // 2nd copy
    oscillate_cycle_black_first(board); // 3rd copy — threefold

    REQUIRE(board.is_repetition(1));
}

TEST_CASE("TC5 - Post-capture threefold is detected; capture resets scan boundary", "[repetition]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK_CAPTURE);

    auto capture = MoveFactory::MakeCapture(a1, a5);
    REQUIRE(board.DoMove(capture)); // irreversible — black to move next

    for (int i = 0; i < 3; ++i)
    {
        apply_quiet(board, f6, e6);
        apply_quiet(board, a5, h5);
        apply_quiet(board, e6, f6);
        apply_quiet(board, h5, a5);
    }

    REQUIRE(board.is_repetition(1));
}

TEST_CASE("TC6 - Castling rights change prevents false positive", "[repetition]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CASTLING);

    // Move a-rooks off home squares — both sides lose queenside castling rights.
    apply_quiet(board, a1, a2);
    apply_quiet(board, a8, a7);
    apply_quiet(board, a2, a1); // pieces return, rights permanently gone
    apply_quiet(board, a7, a8);

    // Second cycle: post-rights-loss position repeats (twofold only, not threefold).
    apply_quiet(board, a1, a2);
    apply_quiet(board, a8, a7);
    apply_quiet(board, a2, a1);
    apply_quiet(board, a7, a8);

    REQUIRE_FALSE(board.is_repetition(1)); // no false positive
}

TEST_CASE("TC7 - UndoMove fully restores repetition state", "[repetition]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    auto m1 = MoveFactory::MakeQuiet(a1, h1);
    auto m2 = MoveFactory::MakeQuiet(d6, e6);
    auto m3 = MoveFactory::MakeQuiet(h1, a1);
    auto m4 = MoveFactory::MakeQuiet(e6, d6);

    board.DoMove(m1); board.DoMove(m2); board.DoMove(m3); board.DoMove(m4);
    board.UndoMove(m4); board.UndoMove(m3); board.UndoMove(m2); board.UndoMove(m1);

    REQUIRE_FALSE(board.is_repetition(1)); // must match freshly set-up board
}

TEST_CASE("TC9 - Twofold repetition within search tree is a draw", "[repetition]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK);

    // All 5 moves are search moves (no game history).
    apply_quiet(board, a1, h1); // ply 1 — hash stored at index 0
    apply_quiet(board, d6, e6); // ply 2
    apply_quiet(board, h1, a1); // ply 3
    apply_quiet(board, e6, d6); // ply 4 — back to root position
    apply_quiet(board, a1, h1); // ply 5 — same hash as ply 1

    // ply=5: search_root_index = history_size(5) - ply(5) = 0.
    // Index 0 satisfies i >= 0 (in-search), repetitions==1 → returns true.
    REQUIRE(board.is_repetition(5));
}
