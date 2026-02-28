// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Sort.h"

#include <cassert>
#include <algorithm>
#include <functional>      // For greater<Move>( )
#include "MoveHelper.h"

// Her foretages en findes traekket fra PVLine og rykkes forrest
void MoveSorter::SortMovesIter(MoveList& moveList,			// traeklisten
							   const Move& lastMove,	// modstanderens traek foer dette
							   const Move* pIterMove
							   )
{
	size_t foundMove = 0;
	if (pIterMove != nullptr)	// Har vi et traek at kigge paa?
	{
		const size_t numMoves = moveList.size();
		//Vi loeber traekkene igennem og sorterer
		for (size_t i=0; i< numMoves; ++i)
		{
			// Iterative deepening - bedste traek paa samme dybde fra sidste traek
			if (*pIterMove == moveList[i])
			{
				// Bytter om paa de 2 moves
				SwapMoves(moveList, i, foundMove++);
				break; // only one move
			}
		}
	}

	// Sort the rest of the moves
	SortMoves(moveList, lastMove, foundMove);
}

// TODO: Convert to use standard value based sorting - it's way faster - 
// additionally we are now sorting captures before Promotions even if they are usually a lot faster
// Her foretages en sortering af de lovlige traek 
void MoveSorter::SortMoves(MoveList& moveList,		// traeklisten
						   const Move& lastMove,	// modstanderens traek foer dette
						   size_t curIndex			// hvorfra sorteringen skal starte
						   )
{
	size_t loop = 0;

	const size_t numMoves = moveList.size();
	// Slag af sidst flyttet brik hvis den eksisterer
	if (!lastMove.IsEmpty()) {
		//Vi loeber traekkene igennem og sorterer videre
		const eSquare lastTo = lastMove.to();
		for (loop = curIndex; loop < numMoves; ++loop)
		{
			// Slag af sidst flyttet brik
			if (lastTo == moveList[loop].to())	// Det sidste af modstanderens traek!!
			{
				assert(MoveHelper::AsType(moveList[loop]) == MoveType::CAPTURE || MoveHelper::IsPromote(moveList[loop]));
				// Flytter det fundne traek frem
				SwapMoves(moveList, loop, curIndex++);
			}
		}
	}

	const size_t lastMoveEndIndex = curIndex;	// TODO: Right now we are not sorting the last move captures as just found
											// Test whether we should do it anyway.

	// Vi koerer videre fra hvor vi slap
	for ( loop=curIndex; loop<numMoves; ++loop )
	{
		// Other captures (regular Capture, PromoteCapture and En passant moves) or Promotes?
		if (MoveHelper::IsCapture(moveList[loop]) || MoveHelper::IsPromote(moveList[loop]))
			// Flytter det fundne traek frem
			SwapMoves(moveList, loop, curIndex++);
	}

	// Sorterer moves - diff is number of found interesting moves
	SortMovesByValue(moveList, curIndex-lastMoveEndIndex, lastMoveEndIndex);

	// Resten af traekkene er saaledes usorterede, da de ikke falder ind under 
	// nogen regel endnu
	// Her kunne der f.eks. vaere skak og andre
}

// Her sorteres de gode slag frem for de mindre gode 
// Dvs ikke at ofre sin dronning for at faa den #%&!! bonde ;-)
// Bemaerk: start er default 0
// TODO: Why dont the callers supply iterators instead?
void MoveSorter::SortMovesByValue(MoveList& moveList, size_t captures, size_t start)
{
	// Sorter slagene efter vaerdien af forskellen mellem slagne brik og brikken der slaar
	// Benytter sig af den generiske quicksort og class Move's operator>
	if (captures >= 2)	// Mindst 2 for at sortere
		std::sort(moveList.begin()+ static_cast<int>(start), moveList.begin() + static_cast<int>(start+captures), std::greater<>());
}
