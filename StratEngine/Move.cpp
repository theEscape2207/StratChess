// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Move.h"

#include <sstream>

#include <cassert>

#include "PieceHelper.h"

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

		const std::string strFrom = GetBoardCoord(m.From);
		const std::string strTo	 = GetBoardCoord(m.To);

		switch(m.Type) {
		case MoveType::Normal:
		case MoveType::PawnTwoForward:
			os << "Verbose  : " << PieceHelper::FullName(m.MovPiece) << " moves "
				<< strFrom.c_str() << "-" << strTo.c_str();
			break;
		case MoveType::Capture:
			assert(PieceHelper::Color(m.MovPiece) != PieceHelper::Color(m.Content));
			os << "Verbose  : " << PieceHelper::FullName(m.MovPiece) << " moves " 
				<< strFrom.c_str() << "-" << strTo.c_str() << " and takes a " 
				<< PieceHelper::FullName(m.Content);
			break;
		case MoveType::Promote:
			// MovPiece is the new piece, but we know it previously was a Pawn
			os << "Verbose  : " << PieceHelper::FullPawnName(m.MovPiece) << " moves " 
				<< strFrom.c_str() << "-" << strTo.c_str() << " and gets promoted to a " 
				<< PieceHelper::FullName(m.MovPiece);
			break;
		case MoveType::PromoteCapture:
			// MovPiece is the new piece, but we know it previously was a Pawn
			os << "Verbose  : " << PieceHelper::FullPawnName(m.MovPiece) << " moves "
				<< strFrom.c_str() << "-" << strTo.c_str() << " and gets promoted to a "
				<< PieceHelper::FullName(m.MovPiece) << " while killing a " << PieceHelper::FullName(m.Content);
			break;
		case MoveType::En_Passant:
			os << "Verbose  : " << PieceHelper::FullName(m.MovPiece) << " moves "
				<< strFrom.c_str() << "-" << strTo.c_str() << " and en passants " 
				<< PieceHelper::FullName(m.Content);
			break;
		case MoveType::Castling:
			os << "Verbose  : " << ((PieceHelper::Color(m.MovPiece) == BLACK) ? "Black" : "White") << " makes a " 
				<< ( (File( m.To ) == 6 ) ? "short" : "long") << " castling";
			break;
		}
	}
	// is this a Checking move?
		if (m.IsCheck)
		os << " and checks!";
	os << '\n';
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
	assert(To != NO_SQUARE);

	const std::string strFrom = GetBoardCoord(From);
	const std::string strTo = GetBoardCoord(To);

	std::stringstream output;

	switch(Type) {
		case MoveType::Normal:
		case MoveType::PawnTwoForward:
			output << PieceHelper::ShortName(MovPiece) << strFrom << "-" << strTo;
			break;
		case MoveType::Capture:
			output << PieceHelper::ShortName(MovPiece) << strFrom << "x" << strTo;
			break;
		case MoveType::En_Passant:
			output << PieceHelper::ShortName(MovPiece) << strFrom << "-" << strTo << "ep";
			break;
		case MoveType::Promote:	//pB7-B8q
			output << g_cPieceNames[PieceHelper::AsPawn(MovPiece)] << strFrom << 
				"-" << strTo << PieceHelper::ShortName(MovPiece);
			break;
		case MoveType::PromoteCapture:	//pB7xC8q
			output << g_cPieceNames[PieceHelper::AsPawn(MovPiece)] << strFrom <<
				"x" << strTo << PieceHelper::ShortName(MovPiece);
			break;
		case MoveType::Castling:
			// Output is either 0-0 or 0-0-0
			if ( File( To ) == 6 )	// G-column => Short castling
				output << "0-0";
			else if ( File( To ) == 2 )	// C-column => Long castling
				output << "0-0-0";
			else
				assert(!"Sveinn suck at math...");
			break;
	}
	// is this a Checking move?
	if ( IsCheck )
		output << "+";
	return output.str();
}
