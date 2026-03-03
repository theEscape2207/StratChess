// BoardTests.cpp — Catch2 test suite for Board DoMove/UndoMove completeness
//
// Phase 1 board tests covering en passant, castling, promotion, and Zobrist
// hash invariants. These verify Move struct Phase 3 correctness (MovPiece
// removed from Move; piece information flows through Board::GetEffectiveMovPiece).
//
// Added as part of Phase 3 of the Move layout refactoring (March 2026).

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "MoveFactory.h"
#include "MoveHelper.h"
#include "MoveGenerator.h"
#include "PieceHelper.h"
#include "defines.h"
#include <algorithm>

// ── FEN constants ─────────────────────────────────────────────────────────────

// White pawn on d5; black pawn just moved e7-e5 (EP square e6)
static constexpr const char* FEN_EP =
    "8/8/8/3Pp3/8/8/8/4K2k w - e6 0 1";

// Kings and rooks with full castling rights
static constexpr const char* FEN_CASTLING =
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

// White pawn on c7 (quiet promo to c8); black rook on b8 (capture-promo c7xb8)
static constexpr const char* FEN_PROMOTION =
    "1r6/2P5/8/8/8/8/8/4K2k w - - 0 1";

// Simple rook + king position for Zobrist hash invariant test
static constexpr const char* FEN_ROOK_HASH =
    "8/8/3k4/8/8/3K4/8/R7 w - - 0 1";

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("Board - En passant DoMove removes captured pawn; UndoMove restores it", "[board]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_EP);

    // Verify initial state
    REQUIRE(board.GetPiece(d5) == WHITE_PAWN);
    REQUIRE(board.GetPiece(e5) == BLACK_PAWN);
    REQUIRE(board.GetPiece(e6) == NO_PIECE);

    // DoMove: white d5 captures en passant to e6
    auto ep = MoveFactory::MakeEnPassant(d5, e6);
    REQUIRE(board.DoMove(ep));

    // White pawn on e6, origin and captured-pawn square empty
    REQUIRE(board.GetPiece(e6) == WHITE_PAWN);
    REQUIRE(board.GetPiece(d5) == NO_PIECE);
    REQUIRE(board.GetPiece(e5) == NO_PIECE); // captured pawn must be gone

    // UndoMove: full restoration
    board.UndoMove(ep);
    REQUIRE(board.GetPiece(d5) == WHITE_PAWN);
    REQUIRE(board.GetPiece(e5) == BLACK_PAWN); // captured pawn restored
    REQUIRE(board.GetPiece(e6) == NO_PIECE);
}

TEST_CASE("Board - Kingside castling DoMove moves king and rook; UndoMove restores both", "[board]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_CASTLING);

    // Verify initial state
    REQUIRE(board.GetPiece(e1) == WHITE_KING);
    REQUIRE(board.GetPiece(h1) == WHITE_ROOK);
    REQUIRE(board.GetPiece(f1) == NO_PIECE);
    REQUIRE(board.GetPiece(g1) == NO_PIECE);

    // DoMove: white castles kingside (e1 → g1, rook h1 → f1)
    auto castle = MoveFactory::MakeMove(e1, g1, MoveType::KING_CASTLE);
    REQUIRE(board.DoMove(castle));

    // King on g1, rook on f1; original squares empty
    REQUIRE(board.GetPiece(g1) == WHITE_KING);
    REQUIRE(board.GetPiece(f1) == WHITE_ROOK);
    REQUIRE(board.GetPiece(e1) == NO_PIECE);
    REQUIRE(board.GetPiece(h1) == NO_PIECE);

    // UndoMove: king and rook back on original squares
    board.UndoMove(castle);
    REQUIRE(board.GetPiece(e1) == WHITE_KING);
    REQUIRE(board.GetPiece(h1) == WHITE_ROOK);
    REQUIRE(board.GetPiece(f1) == NO_PIECE);
    REQUIRE(board.GetPiece(g1) == NO_PIECE);
}

TEST_CASE("Board - Promotion move generation: quiet promotion yields PROMOTION_QUEEN", "[board]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_PROMOTION);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);

    // Quiet promotion: c7 → c8, must find at least the queen promotion
    const auto it = std::find_if(moveList.begin(), moveList.end(), [](const Move& m) {
        return m.from() == c7 && m.to() == c8
            && MoveHelper::AsType(m) == MoveType::PROMOTION_QUEEN;
    });
    REQUIRE(it != moveList.end());
}

TEST_CASE("Board - Capture-promotion yields PROMOTION_QUEEN_CAPTURE type", "[board]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_PROMOTION);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);

    // Capture-promotion: c7 captures rook on b8, promotes to queen.
    // Phase 4: encoded as PROMOTION_QUEEN_CAPTURE (bits 3+2 both set); no Content field.
    const auto it = std::find_if(moveList.begin(), moveList.end(), [](const Move& m) {
        return m.from() == c7 && m.to() == b8
            && MoveHelper::AsType(m) == MoveType::PROMOTION_QUEEN_CAPTURE;
    });
    REQUIRE(it != moveList.end());
}

TEST_CASE("Board - Promotion DoMove replaces pawn with queen; UndoMove restores pawn", "[board]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_PROMOTION);

    REQUIRE(board.GetPiece(c7) == WHITE_PAWN);
    REQUIRE(board.GetPiece(c8) == NO_PIECE);

    // DoMove: promote pawn c7 → c8 (queen)
    auto promo = MoveFactory::MakePromotion(c7, c8, WHITE_QUEEN);
    REQUIRE(board.DoMove(promo));

    REQUIRE(board.GetPiece(c8) == WHITE_QUEEN);
    REQUIRE(board.GetPiece(c7) == NO_PIECE);

    // UndoMove: pawn back on c7, c8 empty
    board.UndoMove(promo);
    REQUIRE(board.GetPiece(c7) == WHITE_PAWN);
    REQUIRE(board.GetPiece(c8) == NO_PIECE);
}

TEST_CASE("Board - Zobrist hash is identical before and after a DoMove/UndoMove cycle", "[board]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_ROOK_HASH);

    const uint64_t hashBefore = board.get_zobrist_hash();

    // Make a quiet rook move and immediately undo it
    auto m = MoveFactory::MakeQuiet(a1, h1);
    board.DoMove(m);
    board.UndoMove(m);

    REQUIRE(board.get_zobrist_hash() == hashBefore);
}
