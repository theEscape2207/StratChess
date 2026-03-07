// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Move.h"
#include "PieceHelper.h"
#include "MoveHelper.h"

static std::string GetBoardCoord(_In_ eSquare square) {
	std::string tmp(1, static_cast<char>((File(square)) + 'a'));			// File: i.e. 'g'
	return tmp.append(1, static_cast<char>((8 - (Rank(square))) + '0'));	// Rank: i.e. '8' => "g8"
}

// Prints a Move to a stream in coordinate-only form (e.g. "Last move: e2-e4").
// The moving piece is not available here; use Output(ePiece) for piece-prefixed notation.
std::ostream& operator<<(std::ostream& os, _In_ const Move& m)
{
	if (!m.is_null())	// Empty moves are allowed, but ignored
		os << "Move: " << m.Output().c_str() << '\n';
	return os;
}

// Denne funktion printer den bedste linje ud for hver iterativ fordybning
// Invariant: Must be called only with moves in the line
std::ostream& operator<<(std::ostream& os, _In_ const PVLine& line)
{
	assert(!line.empty());

	os << "Depth " << line.size() << ": ";		// TODO: Ekstra check her ? //-V128

	for (const auto& move : line)
	{
		os << move.Output().c_str();		// write out coordinate notation

		if (move != *(line.rbegin()))	// last real Move
			os << ", ";					// Add separation marker
	}
	return os;
}

/////////////////////////////////
//
//	Move members
//

// Coordinate-only output (no piece prefix). Suitable for PV lines and stream consumers
// that do not have board context. Examples: "e2-e4", "c5xe6", "0-0", "b7-b8"
std::string Move::Output() const
{
	assert(to() != NO_SQUARE);

	const std::string strFrom = GetBoardCoord(from());
	const std::string strTo   = GetBoardCoord(to());

	switch (static_cast<MoveType>(flags())) {
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
			(flags() & MoveFlags::CAPTURE_BIT) ? 'x' : '-',
			strTo);
	case MoveType::KING_CASTLE:
		return "0-0";
	case MoveType::QUEEN_CASTLE:
		return "0-0-0";
	}
	return {};
}

// Piece-prefixed output (pseudo-LAN). Requires the moving piece explicitly.
// Examples: "Pe2-e4", "Rc1xc7", "0-0", "pb7-b8Q"
std::string Move::Output(ePiece movPiece) const
{
	assert(PieceHelper::IsActual(movPiece));
	assert(to() != NO_SQUARE);

	const std::string strFrom = GetBoardCoord(from());
	const std::string strTo   = GetBoardCoord(to());
	const char        piece   = PieceHelper::ShortName(movPiece);

	switch (static_cast<MoveType>(flags())) {
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
			(flags() & MoveFlags::CAPTURE_BIT) ? 'x' : '-',
			strTo,
			piece);
	case MoveType::KING_CASTLE:
		return "0-0";
	case MoveType::QUEEN_CASTLE:
		return "0-0-0";
	}
	return {};
}
