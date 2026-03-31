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
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_QUIET);

    auto m = MoveFactory::MakeQuiet(a1, h1);
    // Must be called before DoMove — the API contract
    REQUIRE(board.GetCapturedPiece(m) == NO_PIECE);
}

TEST_CASE("Board::GetCapturedPiece returns BLACK_ROOK for white queen captures black rook", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CAPTURE);

    auto m = MoveFactory::MakeCapture(d1, c1);
    REQUIRE(board.GetCapturedPiece(m) == BLACK_ROOK);
}

TEST_CASE("Board::GetCapturedPiece returns BLACK_PAWN for EP capture (pawn is not on destination square)", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP);

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
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_QUIET);

    auto m = MoveFactory::MakeQuiet(a1, h1);
    REQUIRE(board.GetEffectiveMovPiece(m) == WHITE_ROOK);
}

TEST_CASE("Board::GetEffectiveMovPiece returns the promoted piece, not the pawn", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_PROMO);

    // Pawn is still on c7; GetEffectiveMovPiece must return WHITE_QUEEN (the promoted piece)
    auto m = MoveFactory::MakePromotion(c7, c8, WHITE_QUEEN);
    REQUIRE(board.GetEffectiveMovPiece(m) == WHITE_QUEEN);
}

// ── ExtractFEN round-trips ────────────────────────────────────────────────────

TEST_CASE("Board::ExtractFEN round-trips the standard starting position", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_START);
    CHECK(board.ExtractFEN() == std::string(FEN_START));
}

TEST_CASE("Board::ExtractFEN preserves an active EP square", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP_ACTIVE);
    const std::string fen = board.ExtractFEN();
    // The EP field (4th space-delimited token) must contain "d6"
    CHECK(fen.find("d6") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves partial castling rights (Kq)", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_PARTIAL_RIGHTS);
    const std::string fen = board.ExtractFEN();
    // Castling field must be exactly "Kq" — surrounded by spaces
    CHECK(fen.find(" Kq ") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves black to move", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_BLACK_TO_MOVE);
    const std::string fen = board.ExtractFEN();
    // Active color field must be 'b'
    CHECK(fen.find(" b ") != std::string::npos);
}

TEST_CASE("Board::ExtractFEN preserves halfmove clock (17)", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_BLACK_TO_MOVE);
    CHECK(board.ExtractFEN() == std::string(FEN_BLACK_TO_MOVE));
}

TEST_CASE("Board::ExtractFEN preserves fullmove number (34)", "[board_api]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_BLACK_TO_MOVE);
    const std::string fen = board.ExtractFEN();
    // The fullmove number is the last space-delimited token
    CHECK(fen.substr(fen.rfind(' ') + 1) == "34");
}
