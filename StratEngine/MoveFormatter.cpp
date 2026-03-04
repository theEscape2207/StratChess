// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "MoveFormatter.h"
#include "Board.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include <array>
#include <cassert>

namespace {

    // Returns the algebraic coordinate for a square (e.g., e2, a8, h1).
    // Mirrors the internal GetBoardCoord helper in Move.cpp.
    [[nodiscard]] std::string SquareToCoord(eSquare sq)
    {
        std::string s(1, static_cast<char>(File(sq) + 'a'));
        s += static_cast<char>((8 - Rank(sq)) + '0');
        return s;
    }

    // Returns lowercase piece type name ("pawn", "knight", ..., "king").
    // Index is derived from the ePiece value: static_cast<size_t>(piece) >> 1
    // gives 0=pawn, 1=knight, 2=bishop, 3=rook, 4=queen, 5=king.
    [[nodiscard]] const char* PieceTypeName(ePiece piece) noexcept
    {
        static constexpr std::array<const char*, 6> names = {
            "pawn", "knight", "bishop", "rook", "queen", "king"
        };
        const size_t index = static_cast<size_t>(piece) >> 1;
        assert(index < names.size());
        return names[index];
    }

} // namespace

// ---------------------------------------------------------------------------
// MoveFormatter::ToShort
// ---------------------------------------------------------------------------
// Post-DoMove. Produces pseudo-LAN (e.g. "Pe2-e4", "Rc1xc7+", "pb7-b8Q").
// Delegates to Move::Output(ePiece) for the base string, then appends '+'
// when board.InCheck() reports the moving side gave check.
std::string MoveFormatter::ToShort(const Move& move, const Board& board)
{
    assert(!move.IsEmpty());
    const ePiece movPiece = board.GetPiece(move.to());
    std::string result = move.Output(movPiece);
    if (board.InCheck())
        result += '+';
    return result;
}

// ---------------------------------------------------------------------------
// MoveFormatter::ToVerbose
// ---------------------------------------------------------------------------
// Post-DoMove. Produces a verbose English description of the move.
// Examples:
//   "White pawn moves e2 to e4"
//   "White rook captures on c7 and checks!"
//   "White king castles kingside"
//   "White pawn promotes to queen on b8"
//   "Black pawn captures and promotes to rook on a1"
std::string MoveFormatter::ToVerbose(const Move& move, const Board& board)
{
    assert(!move.IsEmpty());
    const ePiece movPiece = board.GetPiece(move.to());
    const MoveType type   = MoveHelper::AsType(move);

    const std::string from = SquareToCoord(move.from());
    const std::string to   = SquareToCoord(move.to());

    std::string result;

    switch (type)
    {
    case MoveType::QUIET:
    case MoveType::DOUBLE_PAWN_PUSH:
        result = std::string(PieceHelper::FullName(movPiece))
               + " moves " + from + " to " + to;
        break;

    case MoveType::CAPTURE:
        result = std::string(PieceHelper::FullName(movPiece))
               + " captures on " + to;
        break;

    case MoveType::EP_CAPTURE:
        result = std::string(PieceHelper::FullName(movPiece))
               + " captures en passant on " + to;
        break;

    case MoveType::KING_CASTLE:
        result = std::string(PieceHelper::FullName(movPiece))
               + " castles kingside";
        break;

    case MoveType::QUEEN_CASTLE:
        result = std::string(PieceHelper::FullName(movPiece))
               + " castles queenside";
        break;

    case MoveType::PROMOTION_KNIGHT:
    case MoveType::PROMOTION_BISHOP:
    case MoveType::PROMOTION_ROOK:
    case MoveType::PROMOTION_QUEEN:
    {
        // movPiece is the promoted piece; recover the pawn color for the subject
        const eColor color = PieceHelper::Color(movPiece);
        const ePiece pawn  = PieceHelper::AsPawn(color);
        result = std::string(PieceHelper::FullName(pawn))
               + " promotes to " + PieceTypeName(movPiece) + " on " + to;
        break;
    }

    case MoveType::PROMOTION_KNIGHT_CAPTURE:
    case MoveType::PROMOTION_BISHOP_CAPTURE:
    case MoveType::PROMOTION_ROOK_CAPTURE:
    case MoveType::PROMOTION_QUEEN_CAPTURE:
    {
        const eColor color = PieceHelper::Color(movPiece);
        const ePiece pawn  = PieceHelper::AsPawn(color);
        result = std::string(PieceHelper::FullName(pawn))
               + " captures and promotes to " + PieceTypeName(movPiece) + " on " + to;
        break;
    }

    default:
        result = std::string(PieceHelper::FullName(movPiece))
               + " moves " + from + " to " + to;
        break;
    }

    if (board.InCheck())
        result += " and checks!";

    return result;
}

// ---------------------------------------------------------------------------
// MoveFormatter::ToUCI
// ---------------------------------------------------------------------------
// No board context required. Produces standard UCI coordinate notation.
// Examples: "e2e4", "c5d6", "b7b8q", "b7a8n", "e1g1", "e1c1"
// Lowercase promotion suffix for all 8 promotion MoveType variants (including
// capture-promotions that the old Perft::move_to_string omitted).
std::string MoveFormatter::ToUCI(const Move& move)
{
    assert(!move.IsEmpty());

    std::string result = SquareToCoord(move.from()) + SquareToCoord(move.to());

    switch (MoveHelper::AsType(move))
    {
    case MoveType::PROMOTION_QUEEN:
    case MoveType::PROMOTION_QUEEN_CAPTURE:   result += 'q'; break;
    case MoveType::PROMOTION_ROOK:
    case MoveType::PROMOTION_ROOK_CAPTURE:    result += 'r'; break;
    case MoveType::PROMOTION_BISHOP:
    case MoveType::PROMOTION_BISHOP_CAPTURE:  result += 'b'; break;
    case MoveType::PROMOTION_KNIGHT:
    case MoveType::PROMOTION_KNIGHT_CAPTURE:  result += 'n'; break;
    default: break;
    }

    return result;
}

// ---------------------------------------------------------------------------
// MoveFormatter::FromUCI
// ---------------------------------------------------------------------------
// Parses a 4- or 5-character UCI string and infers MoveType from board state.
// Board must be in the state BEFORE DoMove (piece on move.from() is the moving
// piece; move.to() may or may not have an opponent piece).
// Returns Move{} (IsEmpty() == true) for malformed input.
Move MoveFormatter::FromUCI(std::string_view uci, const Board& board)
{
    if (uci.size() < 4)
        return Move{};

    // Parse algebraic squares: file 'a'-'h', rank '1'-'8'.
    // Internal layout: rank 8 = row 0, rank 1 = row 7; file a = col 0.
    auto parse_sq = [](char file_ch, char rank_ch) -> eSquare
    {
        const int file = file_ch - 'a';     // 0-7
        const int rank = '8' - rank_ch;     // 0 (rank 8) to 7 (rank 1)
        return static_cast<eSquare>(rank * 8 + file);
    };

    const eSquare from = parse_sq(uci[0], uci[1]);
    const eSquare to   = parse_sq(uci[2], uci[3]);

    const ePiece piece = board.GetPiece(from);

    // --- Promotion (5-character string) ---
    if (uci.size() == 5)
    {
        const bool isCapture = PieceHelper::IsActual(board.GetPiece(to));
        switch (uci[4])
        {
        case 'q': return Move(from, to, isCapture ? MoveType::PROMOTION_QUEEN_CAPTURE  : MoveType::PROMOTION_QUEEN);
        case 'r': return Move(from, to, isCapture ? MoveType::PROMOTION_ROOK_CAPTURE   : MoveType::PROMOTION_ROOK);
        case 'b': return Move(from, to, isCapture ? MoveType::PROMOTION_BISHOP_CAPTURE : MoveType::PROMOTION_BISHOP);
        case 'n': return Move(from, to, isCapture ? MoveType::PROMOTION_KNIGHT_CAPTURE : MoveType::PROMOTION_KNIGHT);
        default:  return Move{};  // unknown suffix — reject rather than silently defaulting to queen
        }
    }

    // --- Castling (king moves two files) ---
    if (PieceHelper::IsKing(piece))
    {
        const int fileDiff = static_cast<int>(File(to)) - static_cast<int>(File(from));
        if (fileDiff == 2)  return Move(from, to, MoveType::KING_CASTLE);
        if (fileDiff == -2) return Move(from, to, MoveType::QUEEN_CASTLE);
    }

    // --- En passant (pawn moves diagonally to an empty square) ---
    if (PieceHelper::IsPawn(piece)
        && File(from) != File(to)
        && !PieceHelper::IsActual(board.GetPiece(to)))
    {
        return Move(from, to, MoveType::EP_CAPTURE);
    }

    // --- Double pawn push (pawn advances two ranks) ---
    if (PieceHelper::IsPawn(piece))
    {
        const int rankDelta = static_cast<int>(Rank(to)) - static_cast<int>(Rank(from));
        if (rankDelta == 2 || rankDelta == -2)
            return Move(from, to, MoveType::DOUBLE_PAWN_PUSH);
    }

    // --- Regular capture ---
    if (PieceHelper::IsActual(board.GetPiece(to)))
        return Move(from, to, MoveType::CAPTURE);

    // --- Quiet move ---
    return Move(from, to, MoveType::QUIET);
}
