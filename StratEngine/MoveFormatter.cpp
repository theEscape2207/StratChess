// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "MoveFormatter.h"
#include "Board.h"
#include "MoveHelper.h"
#include "PieceHelper.h"

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
// MoveFormatter::ToCoord
// ---------------------------------------------------------------------------
// No board context required. Coordinate-only pseudo-LAN, no piece prefix.
// Examples: "e2-e4", "c5xe6", "e5-d6ep", "0-0", "b7-b8"
std::string MoveFormatter::ToCoord(const Move& move)
{
    assert(move.to() != NO_SQUARE);

    const std::string strFrom = SquareToCoord(move.from());
    const std::string strTo   = SquareToCoord(move.to());

    switch (MoveHelper::AsType(move))
    {
    case MoveType::QUIET:
    case MoveType::DOUBLE_PAWN_PUSH:
        return std::format("{}-{}", strFrom, strTo);
    case MoveType::CAPTURE:
        return std::format("{}x{}", strFrom, strTo);
    case MoveType::EP_CAPTURE:
        return std::format("{}-{}ep", strFrom, strTo);
    case MoveType::PROMOTION_KNIGHT:
    case MoveType::PROMOTION_BISHOP:
    case MoveType::PROMOTION_ROOK:
    case MoveType::PROMOTION_QUEEN:
    case MoveType::PROMOTION_KNIGHT_CAPTURE:
    case MoveType::PROMOTION_BISHOP_CAPTURE:
    case MoveType::PROMOTION_ROOK_CAPTURE:
    case MoveType::PROMOTION_QUEEN_CAPTURE:
        // Capture bit (bit 2) encodes whether the promotion also captures.
        return std::format("{}{}{}",
            strFrom,
            (move.flags() & MoveFlags::CAPTURE_BIT) ? 'x' : '-',
            strTo);
    case MoveType::KING_CASTLE:
        return "0-0";
    case MoveType::QUEEN_CASTLE:
        return "0-0-0";
    }
    return {};
}

// ---------------------------------------------------------------------------
// MoveFormatter::ToShort (explicit piece)
// ---------------------------------------------------------------------------
// No board context required. Pseudo-LAN with piece prefix, no check annotation.
// Examples: "Pe2-e4", "Rc1xc7", "pb7-b8Q", "0-0"
std::string MoveFormatter::ToShort(const Move& move, ePiece movPiece)
{
    assert(PieceHelper::IsActual(movPiece));
    assert(move.to() != NO_SQUARE);

    const std::string strFrom = SquareToCoord(move.from());
    const std::string strTo   = SquareToCoord(move.to());
    const char        piece   = PieceHelper::ShortName(movPiece);

    switch (MoveHelper::AsType(move))
    {
    case MoveType::QUIET:
    case MoveType::DOUBLE_PAWN_PUSH:
        return std::format("{}{}-{}", piece, strFrom, strTo);
    case MoveType::CAPTURE:
        return std::format("{}{}x{}", piece, strFrom, strTo);
    case MoveType::EP_CAPTURE:
        return std::format("{}{}-{}ep", piece, strFrom, strTo);
    case MoveType::PROMOTION_KNIGHT:
    case MoveType::PROMOTION_BISHOP:
    case MoveType::PROMOTION_ROOK:
    case MoveType::PROMOTION_QUEEN:
    case MoveType::PROMOTION_KNIGHT_CAPTURE:
    case MoveType::PROMOTION_BISHOP_CAPTURE:
    case MoveType::PROMOTION_ROOK_CAPTURE:
    case MoveType::PROMOTION_QUEEN_CAPTURE:
        // Capture bit (bit 2) encodes whether the promotion also captures.
        // Piece prefix is the pawn (lower-case = black, upper-case = white via ShortName)
        return std::format("{}{}{}{}{}",
            g_cPieceNames[PieceHelper::AsPawn(movPiece)],
            strFrom,
            (move.flags() & MoveFlags::CAPTURE_BIT) ? 'x' : '-',
            strTo,
            piece);
    case MoveType::KING_CASTLE:
        return "0-0";
    case MoveType::QUEEN_CASTLE:
        return "0-0-0";
    }
    return {};
}

// ---------------------------------------------------------------------------
// MoveFormatter::ToShort (post-move Board)
// ---------------------------------------------------------------------------
// Post-DoMove. Produces pseudo-LAN (e.g. "Pe2-e4", "Rc1xc7+", "pb7-b8Q").
// Resolves the moving piece from the board, then appends '+' when
// board.InCheck() reports the moving side gave check.
std::string MoveFormatter::ToShort(const Move& move, const Board& board)
{
    assert(!move.is_null());
    std::string result = ToShort(move, board.GetPiece(move.to()));
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
    assert(!move.is_null());
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
    assert(!move.is_null());

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
// Returns Move{} (is_null() == true) for malformed input.
Move MoveFormatter::FromUCI(std::string_view uci, const Board& board)
{
    if (uci.size() < 4)
        return Move{};

    // Parse algebraic squares: file 'a'-'h', rank '1'-'8'.
    // Internal layout: rank 8 = row 0, rank 1 = row 7; file a = col 0.
    // The characters are validated, not just the length: this parses whatever a
    // GUI sends. "zzzz" would otherwise compute file 25, rank -66 and index the
    // mailbox at -503 (#200).
    auto parse_sq = [](char file_ch, char rank_ch) -> eSquare
    {
        if (file_ch < 'a' || file_ch > 'h' || rank_ch < '1' || rank_ch > '8')
            return NO_SQUARE;
        const int file = file_ch - 'a';     // 0-7
        const int rank = '8' - rank_ch;     // 0 (rank 8) to 7 (rank 1)
        return static_cast<eSquare>(rank * 8 + file);
    };

    const eSquare from = parse_sq(uci[0], uci[1]);
    const eSquare to   = parse_sq(uci[2], uci[3]);
    if (from == NO_SQUARE || to == NO_SQUARE)
        return Move{};

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
