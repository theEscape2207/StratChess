// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Move.h"
#include "MoveFormatter.h"

// Prints a Move to a stream in coordinate-only form (e.g. "Last move: e2-e4").
// The moving piece is not available here; use MoveFormatter::ToShort for piece-prefixed notation.
std::ostream& operator<<(std::ostream& os, const Move& m)
{
	if (!m.is_null()) // Empty moves are allowed, but ignored
		os << "Move: " << MoveFormatter::ToCoord(m).c_str() << '\n';
	return os;
}

// Denne funktion printer den bedste linje ud for hver iterativ fordybning
// Invariant: Must be called only with moves in the line
std::ostream& operator<<(std::ostream& os, const PVLine& line)
{
	assert(!line.empty());

	os << "Depth " << line.size() << ": "; // TODO: Ekstra check her ? //-V128

	for (const auto& move : line) {
		os << MoveFormatter::ToCoord(move).c_str(); // write out coordinate notation

		if (move != *(line.rbegin())) // last real Move
			os << ", ";               // Add separation marker
	}
	return os;
}
