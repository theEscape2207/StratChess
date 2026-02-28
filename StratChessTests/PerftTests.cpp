// PerftTests.cpp — Catch2 lightweight perft regression tests
//
// Validates move generation against known node counts using hardcoded expected
// values — no JSON loading required. Depths are bounded so the full suite runs
// in well under a second (Release build).
//
// Deep perft (depth >= 5) remains in StratChessEvolved.exe:
//     cd Tests && ../x64/Release/StratChessEvolved.exe perft test

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "Tests/Perft.h"

using Testing::Perft;

// ── Starting position ─────────────────────────────────────────────────────────

static constexpr const char* FEN_START =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

TEST_CASE("Perft - Starting position depth 1 = 20", "[perft]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_START);
    REQUIRE(Perft::run(board, 1).nodes == 20ULL);
}

TEST_CASE("Perft - Starting position depth 2 = 400", "[perft]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_START);
    REQUIRE(Perft::run(board, 2).nodes == 400ULL);
}

TEST_CASE("Perft - Starting position depth 3 = 8902", "[perft]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_START);
    REQUIRE(Perft::run(board, 3).nodes == 8902ULL);
}

TEST_CASE("Perft - Starting position depth 4 = 197281", "[perft]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_START);
    REQUIRE(Perft::run(board, 4).nodes == 197281ULL);
}

// ── Kiwipete (complex middlegame — exercises castling, promotions, en passant) ─

static constexpr const char* FEN_KIWIPETE =
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

TEST_CASE("Perft - Kiwipete depth 1 = 48", "[perft]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_KIWIPETE);
    REQUIRE(Perft::run(board, 1).nodes == 48ULL);
}

TEST_CASE("Perft - Kiwipete depth 2 = 2039", "[perft]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_KIWIPETE);
    REQUIRE(Perft::run(board, 2).nodes == 2039ULL);
}

TEST_CASE("Perft - Kiwipete depth 3 = 97862", "[perft]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_KIWIPETE);
    REQUIRE(Perft::run(board, 3).nodes == 97862ULL);
}
