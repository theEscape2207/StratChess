#include "TranspositionTable.h"

#include "Move.h"

//std::ostream& operator<<(std::ostream& os, const PVTable& line)
//{
//	//assert(!line.empty());
//
//	os << "Depth " << line.get_length(0) << ": ";		// TODO: Ekstra check her ? //-V128
//
//	for (int i = 0; i < line.get_length(0) && i < 10; ++i)
//	{
//		auto move = line.get_line(0)[i];
//		os << MoveFormatter::ToCoord(move).c_str();		// write out coordinate notation
//
//		if (move != *(line.rbegin()))	// last real Move
//			os << ", ";					// Add seperation marker
//	}
//	return os;
//}
