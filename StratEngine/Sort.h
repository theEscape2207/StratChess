#pragma once
#include "Move.h"

class MoveSorter final

{
public:
	static void SortMoves(MoveList& moveList, const Move& lastMove, size_t startIndex = 0 );
	static void SortMovesIter(MoveList& moveList, const Move& lastMove, const Move* pIterMove);
	static void SortMovesHash(MoveList& moveList, const Move& lastMove, const Move* pHashMove);
	static void SortMovesByValue(MoveList& moveList, size_t captures, size_t start=0);
	~MoveSorter() = default;
private:
	MoveSorter() = default;		// Enforce static method calls
	// TODO: This is duplicated in MoveGenerator
	static void SwapMoves(MoveList& moveList, size_t first, size_t second) noexcept	// TODO: This function is an extension method (friend) and should be moved out
	{
		if (first == second) {
			return;
		}
		std::swap(moveList[first], moveList[second]);
	}
};
