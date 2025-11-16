#include "TranspositionTable.h"

#include "Move.h"

//std::ostream& operator<<(std::ostream& os, _In_ const PVTable& line)
//{
//	//assert(!line.empty());
//
//	os << "Depth " << line.get_length(0) << ": ";		// TODO: Ekstra check her ? //-V128
//
//	for (int i = 0; i < line.get_length(0) && i < 10; ++i)
//	{
//		auto move = line.get_line(0)[i];
//		os << move.Output().c_str();		// write out short notation string
//
//		if (move != *(line.rbegin()))	// last real Move
//			os << ", ";					// Add seperation marker
//	}
//	return os;
//}
