// BoardApiTests.cpp — Catch2 test suite for Board public query APIs and
// FEN round-trip serialization.
//
// Covers GetCapturedPiece, GetEffectiveMovPiece (both must be called BEFORE
// DoMove — the API contract documented in Board.h), and ExtractFEN round-trips
// for all FEN fields (side-to-move, castling, EP square, halfmove clock,
// fullmove number).

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "MoveFactory.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "defines.h"
#include <array>
#include <string>

// ── FEN constants ─────────────────────────────────────────────────────────────

// Quiet rook position — no captures possible
static constexpr const char* FEN_QUIET =
    "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

// White queen d1, black rook c1, kings on e1/e8
static constexpr const char* FEN_CAPTURE =
    "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1";

// White pawn d5, black pawn e5, EP square e6 (black just pushed e7-e5)
static constexpr const char* FEN_EP =
    "8/8/8/3Pp3/8/8/8/4K2k w - e6 0 1";

// White pawn c7, kings on e1/e8 — promotion API test
static constexpr const char* FEN_PROMO =
    "4k3/2P5/8/8/8/8/8/4K3 w - - 0 1";

// Standard starting position
static constexpr const char* FEN_START =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Active EP square — black pushed d7-d5; white pawn on e5, black pawn on d5.
// EP square d6 available for white to capture. White to move.
static constexpr const char* FEN_EP_ACTIVE =
    "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1";

// Partial castling rights: white can castle kingside (K), black queenside (q)
static constexpr const char* FEN_PARTIAL_RIGHTS =
    "r3k3/8/8/8/8/8/8/4K2R w Kq - 0 1";

// Black to move, halfmove=17, fullmove=34 — tests all numeric/side fields
static constexpr const char* FEN_BLACK_TO_MOVE =
    "4k3/8/8/8/8/8/8/R3K3 b - - 17 34";

// ── GetCapturedPiece ──────────────────────────────────────────────────────────

TEST_CASE("Board::GetCapturedPiece returns NO_PIECE for a quiet move", "[board_api]")
{
    Board board(FEN_QUIET);

    auto m = MoveFactory::MakeQuiet(a1, h1);
    // Must be called before DoMove — the API contract
    REQUIRE(board.GetCapturedPiece(m) == NO_PIECE);
}

TEST_CASE("Board::GetCapturedPiece returns BLACK_ROOK for white queen captures black rook", "[board_api]")
{
    Board board(FEN_CAPTURE);

    auto m = MoveFactory::MakeCapture(d1, c1);
    REQUIRE(board.GetCapturedPiece(m) == BLACK_ROOK);
}

TEST_CASE("Board::GetCapturedPiece returns BLACK_PAWN for EP capture (pawn is not on destination square)", "[board_api]")
{
    Board board(FEN_EP);

    // EP: white d5 captures to e6; the black pawn is on e5 (not e6)
    auto m = MoveFactory::MakeEnPassant(d5, e6);

    // Confirm the destination is empty (the API must NOT look at m.to())
    REQUIRE(board.GetPiece(e6) == NO_PIECE);
    REQUIRE(board.GetPiece(e5) == BLACK_PAWN);

    CHECK(board.GetCapturedPiece(m) == BLACK_PAWN);
}

// ── GetEffectiveMovPiece ──────────────────────────────────────────────────────

TEST_CASE("Board::GetEffectiveMovPiece returns the piece on from-square for a quiet move", "[board_api]")
{
    Board board(FEN_QUIET);

    auto m = MoveFactory::MakeQuiet(a1, h1);
    REQUIRE(board.GetEffectiveMovPiece(m) == WHITE_ROOK);
}

TEST_CASE("Board::GetEffectiveMovPiece returns the promoted piece, not the pawn", "[board_api]")
{
    Board board(FEN_PROMO);

    // Pawn is still on c7; GetEffectiveMovPiece must return WHITE_QUEEN (the promoted piece)
    auto m = MoveFactory::MakePromotion(c7, c8, WHITE_QUEEN);
    REQUIRE(board.GetEffectiveMovPiece(m) == WHITE_QUEEN);
}

// ── ExtractFEN round-trips ────────────────────────────────────────────────────

TEST_CASE("Board::ExtractFEN round-trips the standard starting position", "[board_api]")
{
    Board board(FEN_START);
    CHECK(board.ExtractFEN() == std::string(FEN_START));
}

TEST_CASE("Board::ExtractFEN preserves an active EP square", "[board_api]")
{
    Board board(FEN_EP_ACTIVE);
    const std::string fen = board.ExtractFEN();
    // The EP field (4th space-delimited token) must contain "d6"
    CHECK(fen.find("d6") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves partial castling rights (Kq)", "[board_api]")
{
    Board board(FEN_PARTIAL_RIGHTS);
    const std::string fen = board.ExtractFEN();
    // Castling field must be exactly "Kq" — surrounded by spaces
    CHECK(fen.find(" Kq ") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves black to move", "[board_api]")
{
    Board board(FEN_BLACK_TO_MOVE);
    const std::string fen = board.ExtractFEN();
    // Active color field must be 'b'
    CHECK(fen.find(" b ") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves halfmove clock (17)", "[board_api]")
{
    Board board(FEN_BLACK_TO_MOVE);
    CHECK(board.ExtractFEN() == std::string(FEN_BLACK_TO_MOVE));
}

TEST_CASE("Board::ExtractFEN preserves fullmove number (34)", "[board_api]")
{
    Board board(FEN_BLACK_TO_MOVE);
    const std::string fen = board.ExtractFEN();
    // The fullmove number is the last space-delimited token
    CHECK(fen.substr(fen.rfind(' ') + 1) == "34");
}

// ── ResetSearchDepth (issue #53: currentPly_ overflow across long games) ──────

TEST_CASE("Board::ResetSearchDepth zeroes undo-stack depth after each committed move, regardless of total moves played", "[board_api]")
{
    Board board(FEN_QUIET);

    // White rook shuffles a1<->a2; black king shuffles d6<->d7. Neither move
    // ever threatens either king, so the 4-ply cycle stays legal indefinitely.
    constexpr int kCycles = 80; // 80 * 4 = 320 committed real moves — past the old MAX_PLY=256 ceiling
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        REQUIRE(board.DoMove(MoveFactory::MakeQuiet(a1, a2)));
        // Depth reflects only this single commit, never the cumulative move
        // count — proves currentPly_ does not accumulate across the game.
        REQUIRE(board.GetSearchDepth() == 1);
        board.ResetSearchDepth();

        REQUIRE(board.DoMove(MoveFactory::MakeQuiet(d6, d7)));
        REQUIRE(board.GetSearchDepth() == 1);
        board.ResetSearchDepth();

        REQUIRE(board.DoMove(MoveFactory::MakeQuiet(a2, a1)));
        REQUIRE(board.GetSearchDepth() == 1);
        board.ResetSearchDepth();

        REQUIRE(board.DoMove(MoveFactory::MakeQuiet(d7, d6)));
        REQUIRE(board.GetSearchDepth() == 1);
        board.ResetSearchDepth();
    }
}

TEST_CASE("Board::ResetSearchDepth preserves full search-recursion headroom after a game longer than the old MAX_PLY ceiling", "[board_api]")
{
    Board board(FEN_QUIET);

    // Commit 260 real moves (already past the old MAX_PLY=256 ceiling on its own),
    // resetting the undo-stack depth after each — mirrors Game::Run's real commit pattern.
    for (int i = 0; i < 130; ++i)
    {
        REQUIRE(board.DoMove(MoveFactory::MakeQuiet(a1, a2)));
        board.ResetSearchDepth();
        REQUIRE(board.DoMove(MoveFactory::MakeQuiet(a2, a1)));
        board.ResetSearchDepth();
    }

    // Now simulate search recursion on top of that (reset) baseline: 50 nested
    // DoMove pushes without intervening undo. In the old scheme, currentPly_
    // would already sit at ~260 here, so this would overflow MAX_PLY=256.
    const std::array<Move, 4> cycle = {
        MoveFactory::MakeQuiet(a1, a2),
        MoveFactory::MakeQuiet(d6, d7),
        MoveFactory::MakeQuiet(a2, a1),
        MoveFactory::MakeQuiet(d7, d6)
    };

    constexpr int kRecursionDepth = 50;
    for (int depth = 0; depth < kRecursionDepth; ++depth)
    {
        REQUIRE(board.DoMove(cycle[depth % 4]));
        REQUIRE(board.GetSearchDepth() == static_cast<size_t>(depth + 1));
    }
    for (int depth = kRecursionDepth; depth > 0; --depth)
    {
        board.UndoMove(cycle[(depth - 1) % 4]);
        REQUIRE(board.GetSearchDepth() == static_cast<size_t>(depth - 1));
    }
}
