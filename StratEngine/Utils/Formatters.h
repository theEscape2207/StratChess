#pragma once
#include <spdlog/fmt/bundled/base.h>
#include "defines.h"		// eColor, eSquare

// eColor formatter
template<> struct fmt::formatter<eColor> {
    constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    auto format(eColor c, fmt::format_context& ctx) const {
        return fmt::format_to(ctx.out(), "{}", static_cast<const char*>(
            c == WHITE ? "White" : "Black"
            ));
    }
};

// eSquare formatter
template<> struct fmt::formatter<eSquare> {
    constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    auto format(eSquare s, fmt::format_context& ctx) const {
        static constexpr const char* names[] = {
            "A8","B8","C8","D8","E8","F8","G8","H8",
            "A7","B7","C7","D7","E7","F7","G7","H7",
            "A6","B6","C6","D6","E6","F6","G6","H6",
            "A5","B5","C5","D5","E5","F5","G5","H5",
            "A4","B4","C4","D4","E4","F4","G4","H4",
            "A3","B3","C3","D3","E3","F3","G3","H3",
            "A2","B2","C2","D2","E2","F2","G2","H2",
            "A1","B1","C1","D1","E1","F1","G1","H1"
        };
        if (s >= a1 && s <= h8)
            return fmt::format_to(ctx.out(), "{}", names[s]);
        return fmt::format_to(ctx.out(), "??");
    }
};

// ePiece formatter
template<> struct fmt::formatter<ePiece> {
    constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    auto format(ePiece p, fmt::format_context& ctx) const {
        static constexpr const char* names[] = {
            "White Pawn",    "Black Pawn",
            "White Knight",  "Black Knight",
            "White Bishop",  "Black Bishop",
            "White Rook",    "Black Rook",
            "White Queen",   "Black Queen",
            "White King",    "Black King",
            "All White",     "All Black",
            "??",            "No Piece"      // index 14 is unused, 15 is NO_PIECE
        };
        return fmt::format_to(ctx.out(), "{}", names[p]);
    }
};

// MoveType formatter
template<> struct fmt::formatter<MoveType> {
    constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
    auto format(MoveType m, fmt::format_context& ctx) const {
        const char* name = [](MoveType t) -> const char* {
            switch (t) {
            case MoveType::QUIET:            return "Quiet";
            case MoveType::DOUBLE_PAWN_PUSH: return "Double Pawn Push";
            case MoveType::KING_CASTLE:      return "King-side Castle";
            case MoveType::QUEEN_CASTLE:     return "Queen-side Castle";
            case MoveType::CAPTURE:          return "Capture";
            case MoveType::EP_CAPTURE:       return "En Passant";
            case MoveType::PROMOTION_KNIGHT: return "Promotion (Knight)";
            case MoveType::PROMOTION_BISHOP: return "Promotion (Bishop)";
            case MoveType::PROMOTION_ROOK:   return "Promotion (Rook)";
            case MoveType::PROMOTION_QUEEN:  return "Promotion (Queen)";
            default:                         return "??";
            }
            }(m);
        return fmt::format_to(ctx.out(), "{}", name);
    }
};