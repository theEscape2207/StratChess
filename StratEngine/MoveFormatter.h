#pragma once

#include <string>
#include <string_view>
#include "Move.h"

// Forward declaration — MoveFormatter only needs Board by reference.
class Board;

// Stateless move presentation helper. All static methods.
//
// Board state contracts:
//   ToShort, ToVerbose  — expect the Board AFTER DoMove. Piece lookup uses
//                         board.GetPiece(move.to()), which returns the moved
//                         (or promoted) piece at its destination.
//   FromUCI             — expects the Board BEFORE DoMove. Piece lookup uses
//                         board.GetPiece(move.from()) and board.GetPiece(move.to())
//                         to infer MoveType (capture, castling, EP, promotion).
//   ToUCI               — no board context required; derives output from move
//                         flags alone.
class MoveFormatter {
  public:
	// Coordinate-only pseudo-LAN: no piece prefix, no check annotation, no board context.
	// Examples: "e2-e4", "c5xe6", "e5-d6ep", "0-0", "b7-b8"
	// Intended for PV lines and search diagnostics, which have no post-move Board to hand.
	[[nodiscard]] static std::string ToCoord(const Move& move);

	// Pseudo-LAN with piece prefix but no check annotation, for callers that know the moving
	// piece but have no post-move Board.
	// Examples: "Pe2-e4", "Rc1xc7", "pb7-b8Q", "0-0"
	[[nodiscard]] static std::string ToShort(const Move& move, ePiece movPiece);

	// Pseudo-LAN with check annotation.
	// Examples: "Pe2-e4", "Rc1xc7+", "pb7-b8Q", "0-0"
	// Appends '+' when board.InCheck() is true after the move.
	[[nodiscard]] static std::string ToShort(const Move& move, const Board& board);

	// Verbose English description.
	// Examples: "White pawn moves e2 to e4", "White rook captures on c7 and checks!"
	//           "White king castles kingside", "White pawn promotes to queen on b8"
	// Appends " and checks!" when board.InCheck() is true after the move.
	[[nodiscard]] static std::string ToVerbose(const Move& move, const Board& board);

	// UCI wire format. No board context required.
	// Examples: "e2e4", "c5d6", "b7b8q", "b7a8n", "e1g1"
	// Lowercase promotion suffix; no separator between from and to squares.
	[[nodiscard]] static std::string ToUCI(const Move& move);

	// Parse a UCI string to a Move. Board context is used to detect
	// capture, en-passant, castling, and double-pawn-push flags.
	// Returns Move{} (is_null() == true) for malformed input.
	[[nodiscard]] static Move FromUCI(std::string_view uci, const Board& board);
};
