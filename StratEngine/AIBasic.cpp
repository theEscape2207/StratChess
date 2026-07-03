// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

// Basic Alpha-Beta search algoritm
// Atm also with Quiscent (anti-horisont) extended search and a simple move sorting
//
//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "AIBasic.h"
#include "Sort.h"
#include "MoveGenerator.h"


// Fetches the next move
Move AIBasic::GetMove(_Inout_ GameInfo& info)
{
	InitMoveVariables(info);

	StartTimer();

	//Kalder den Almindelige rekursive alphabeta soegning
	Search(0, -GameValues::Search_Init, GameValues::Search_Init);

	StopTimerAndAdjustVars(m_SearchCount);

	CheckGameOver(info);

	return GetBestMove(info);
}

// En basic alpha-beta
// p.t. ogsaa med anti-horisont-effekt og simpel sortering
int AIBasic::Search(_In_ size_t ply, _In_ int alpha, _In_ int beta)
{
	// Check time and stop signal
	if (ShouldStopSearch()) {
		return GameValues::Draw;
	}
	const GameInfo& info = GetLastBoardInfo(ply);

	// Test for 50 moves rule
	if (checkDraws(info, static_cast<int>(ply)))
		return GameValues::Draw;

	// Er vi naaet til bunden af traeet ?
	// Modvirkning af Horisont effekt: singular extensions
	// hvis der i bunden af traeet er slaaet en brik, saa fortsaettes soegningen 
	// indtil der ikke bliver slaaet en brik
	if (ply == max_depth_)
	{
		// Quiescent seach
		return Quiescent(ply, alpha, beta);
	}

	// Inkrementerer counter - we are not returning early
	m_SearchCount++;

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(m_Board, info, moveList);

	// Sorterer traekkene
	MoveSorter::SortMoves(moveList, GetParentMove(ply), m_Board);

	bool moveFound = false;
	size_t counter = 0;

	for (const auto &curMove : moveList)
	{
		counter++;

		// Foretag traekket hvis det er lovligt(Check for skak) - ellers proever vi naeste traek
		if (m_Board.DoMove(curMove))
		{
			// Tilfoejer dette traek til nuvaerende traekfoelge
			AddMoveToSeq(curMove, ply);

			// rekursivt kald til Alpha-Beta
			const int value = -Search(ply + 1, -beta, -alpha);

			// Vi er tilbage igen. Undo traekket igen
			m_Board.UndoMove(curMove);

			moveFound = true;	// Har fundet et gyldigt traek

			if (ply == 0)
				spdlog::default_logger()->debug("Root move {}/{}: {} score={}",
					counter, moveList.size(), curMove.Output(), value);

			//	We got a value back.  We unmade the move.  We're not dead.  Let's
			//	see how good this move was.  If it was >= "beta", it was so good
			//	that we don't need to search for anything better, so we'll leave.
			//
			//	If it was not >= "beta", but it was > "alpha", this is better than
			//	anything else we've found before, but not so good we have to
			//	leave.  These kinds of moves are actually quite rare.  
			//
			if (value > alpha)		// Er det en bedre vaerdi?
			{
				if (value >= beta)	// Too good a move? Cutoff!
					return beta;

				// ellers saa er det en ny bedste vaerdi
				alpha = value;

				if (ply == 0)		// Er vi i roden af traeet ?
				{
					// Saa er traekket BEST Move indtil videre 
					m_BestMove = curMove;
					_bestScore = value;
				}
			}
		}
		else
		{
			if (ply == 0)
				spdlog::default_logger()->debug("Root move {}/{}: {} ILLEGAL",
					counter, moveList.size(), curMove.Output());
		}
	}
	if (moveFound) {
		UpdateGameState(ply, GameStates::STILL_PLAYING);
		// Vi har et normalt traek, returner den bedste alpha-vaerdi
		return alpha;
	}
	// Ingen lovlige traek !
	if (m_Board.InCheck())
	{
		UpdateGameState(ply, m_Board.GetCurrentColor() == WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
		// Ups...Vi er vist mat her!!
		if (ply == 0)		// Er vi i roden af traeet ?
		{
			this->_bestScore = -GameValues::Mate;		//Ja, saa saet den nye score
		}
		return -GameValues::Mate + static_cast<int>(ply);		// returner mat-vaerdi minus afstanden til matten
	}
	// Remis: Godt hvis vi er bagud, men skidt hvis vi er foran
	UpdateGameState(ply, GameStates::DRAW_PAT);
	return GameValues::Draw;
}
