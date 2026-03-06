// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Move.h"
#include <sstream>
#include <cassert>
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
	const std::string strTo = GetBoardCoord(to());

	std::stringstream output;

	MoveType type = static_cast<MoveType>(flags());
	switch (type) {
	case MoveType::QUIET:
	case MoveType::DOUBLE_PAWN_PUSH:
		output << strFrom << "-" << strTo;
		break;
	case MoveType::CAPTURE:
		output << strFrom << "x" << strTo;
		break;
	case MoveType::EP_CAPTURE:
		output << strFrom << "-" << strTo << "ep";
		break;
	case MoveType::PROMOTION_KNIGHT:
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_QUEEN:
	case MoveType::PROMOTION_KNIGHT_CAPTURE:
	case MoveType::PROMOTION_BISHOP_CAPTURE:
	case MoveType::PROMOTION_ROOK_CAPTURE:
	case MoveType::PROMOTION_QUEEN_CAPTURE:
	{
		// Capture bit (bit 2) encodes whether the promotion also captures.
		char isCapture = (flags() & MoveFlags::CAPTURE_BIT) ? 'x' : '-';
		output << strFrom << isCapture << strTo;
	}
		break;
	case MoveType::KING_CASTLE:
		output << "0-0";
		break;
	case MoveType::QUEEN_CASTLE:
		output << "0-0-0";
		break;
	}
	return output.str();
}

// Piece-prefixed output (pseudo-LAN). Requires the moving piece explicitly.
// Examples: "Pe2-e4", "Rc1xc7", "0-0", "pb7-b8Q"
std::string Move::Output(ePiece movPiece) const
{
	assert(PieceHelper::IsActual(movPiece));
	assert(to() != NO_SQUARE);

	const std::string strFrom = GetBoardCoord(from());
	const std::string strTo = GetBoardCoord(to());

	std::stringstream output;

	MoveType type = static_cast<MoveType>(flags());
	switch (type) {
	case MoveType::QUIET:
	case MoveType::DOUBLE_PAWN_PUSH:
		output << PieceHelper::ShortName(movPiece) << strFrom << "-" << strTo;
		break;
	case MoveType::CAPTURE:
		output << PieceHelper::ShortName(movPiece) << strFrom << "x" << strTo;
		break;
	case MoveType::EP_CAPTURE:
		output << PieceHelper::ShortName(movPiece) << strFrom << "-" << strTo << "ep";
		break;
	case MoveType::PROMOTION_KNIGHT:
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_QUEEN:
	case MoveType::PROMOTION_KNIGHT_CAPTURE:
	case MoveType::PROMOTION_BISHOP_CAPTURE:
	case MoveType::PROMOTION_ROOK_CAPTURE:
	case MoveType::PROMOTION_QUEEN_CAPTURE:
	{
		// Capture bit (bit 2) encodes whether the promotion also captures.
		char isCapture = (flags() & MoveFlags::CAPTURE_BIT) ? 'x' : '-';
		// Piece prefix is the pawn (lower-case = black, upper-case = white via ShortName)
		output << g_cPieceNames[PieceHelper::AsPawn(movPiece)] << strFrom <<
			isCapture << strTo << PieceHelper::ShortName(movPiece);
	}
		break;
	case MoveType::KING_CASTLE:
		output << "0-0";
		break;
	case MoveType::QUEEN_CASTLE:
		output << "0-0-0";
		break;
	}
	return output.str();
}
