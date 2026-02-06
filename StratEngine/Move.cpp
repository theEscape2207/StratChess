// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Move.h"
#include <sstream>
#include <cassert>
#include "PieceHelper.h"
#include "MoveHelper.h"

static std::string GetBoardCoord(_In_ eSquare square) {
	std::string tmp(1, static_cast<char>((File(square)) + 'a'));				// File: i.e. 'g'
	return tmp.append(1, static_cast<char>((8 - (Rank(square))) + '0'));	// Rank: i.e. '8' => "g8"
}

// Printer indholdet af move til streamen
std::ostream& operator<<(std::ostream& os, _In_ const Move& m)
{
	// FIXME: Test shouldn't be needed
	if (!m.IsEmpty())	// Empty moves are allowed, but ignored
	{
		assert(PieceHelper::IsActual(m.MovPiece));

		// Print out in short notation
		os << "Last move: " << m.Output().c_str() << '\n';

		const std::string strFrom = GetBoardCoord(m.from());
		const std::string strTo = GetBoardCoord(m.to());

		const auto type = static_cast<MoveType>(m.flags());
		switch (type) {
		case MoveType::QUIET:
		case MoveType::DOUBLE_PAWN_PUSH:
			os << "Verbose  : " << PieceHelper::FullName(m.MovPiece) << " moves "
				<< strFrom.c_str() << "-" << strTo.c_str();
			break;
		case MoveType::CAPTURE:
			assert(PieceHelper::Color(m.MovPiece) != PieceHelper::Color(m.Content));
			os << "Verbose  : " << PieceHelper::FullName(m.MovPiece) << " moves "
				<< strFrom.c_str() << "-" << strTo.c_str() << " and takes a "
				<< PieceHelper::FullName(m.Content);
			break;
		case MoveType::PROMOTION_KNIGHT:
		case MoveType::PROMOTION_BISHOP:
		case MoveType::PROMOTION_ROOK:
		case MoveType::PROMOTION_QUEEN:
			// MovPiece is the new piece, but we know it previously was a Pawn
			os << "Verbose  : " << PieceHelper::FullPawnName(m.MovPiece) << " moves "
				<< strFrom.c_str() << "-" << strTo.c_str() << " and gets promoted to a "
				<< PieceHelper::FullName(m.MovPiece);
			if (MoveHelper::IsCapture(m)) {
				assert(PieceHelper::Color(m.MovPiece) != PieceHelper::Color(m.Content));
				os << " while taking a " << PieceHelper::FullName(m.Content);
			}
			break;
		case MoveType::EP_CAPTURE:
			os << "Verbose  : " << PieceHelper::FullName(m.MovPiece) << " moves "
				<< strFrom.c_str() << "-" << strTo.c_str() << " and en passants "
				<< PieceHelper::FullName(m.Content);
			break;
		case MoveType::KING_CASTLE:
			os << "Verbose  : " << ((PieceHelper::Color(m.MovPiece) == BLACK) ? "Black" : "White") << " makes a short castling";
			break;
		case MoveType::QUEEN_CASTLE:
			os << "Verbose  : " << ((PieceHelper::Color(m.MovPiece) == BLACK) ? "Black" : "White") << " makes a long castling";
			break;
		}
		// is this a Checking move?
		if (m.IsCheck)
			os << " and checks!";
		os << '\n';
	}
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
		os << move.Output().c_str();		// write out short notation string

		if (move != *(line.rbegin()))	// last real Move
			os << ", ";					// Add seperation marker
	}
	return os;
}

/////////////////////////////////
//
//	Move members 
//
std::string Move::Output() const
{
	assert(PieceHelper::IsActual(MovPiece));
	assert(to() != NO_SQUARE);

	const std::string strFrom = GetBoardCoord(from());
	const std::string strTo = GetBoardCoord(to());

	std::stringstream output;

	MoveType type = static_cast<MoveType>(flags());
	switch (type) {
	case MoveType::QUIET:
	case MoveType::DOUBLE_PAWN_PUSH:
		output << PieceHelper::ShortName(MovPiece) << strFrom << "-" << strTo;
		break;
	case MoveType::CAPTURE:
		output << PieceHelper::ShortName(MovPiece) << strFrom << "x" << strTo;
		break;
	case MoveType::EP_CAPTURE:
		output << PieceHelper::ShortName(MovPiece) << strFrom << "-" << strTo << "ep";
		break;
	case MoveType::PROMOTION_KNIGHT:	//pB7-B8q
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_QUEEN:
	{
		char isCapture = PieceHelper::IsActual(Content) ? 'x' : '-';
		output << g_cPieceNames[PieceHelper::AsPawn(MovPiece)] << strFrom <<
			isCapture << strTo << PieceHelper::ShortName(MovPiece);
	}
		break;
	case MoveType::KING_CASTLE:
		output << "0-0";
		break;
	case MoveType::QUEEN_CASTLE:
		output << "0-0-0";
		break;
	}
	// is this a Checking move?
	if (IsCheck)
		output << "+";
	return output.str();
}
