// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

// Basic Iterative Alpha-Beta search algoritm
// Also with Quiscent (anti-horisont) extended search and move sorting
// Not using Aspiration windows
//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "ABIterative.h"
#include "Sort.h"
#include "MoveGenerator.h"


// ************************************
// Method:      GetMove
// Description: Returnerer det bedste traek efter soegningen
// FullName:    public ABIterative::GetMove 
// Returns:     Move - The best move found
// Parameter:   _Inout_ BoardInfo& info - 
// ************************************
Move ABIterative::GetMove(_Inout_ GameInfo& info)
{
	InitMoveVariables(info);

	constexpr int alpha = -GameValues::Search_Init;
	constexpr int beta = GameValues::Search_Init;

	StartTimer();

	// almindelig iterativ soegning
	for (m_Depth = 1; m_Depth <= max_depth_; ++m_Depth)
	{
		if (ShouldStopSearch())
			break;

		//Kald vores iterative soegerutine (resetting alpha and beta)
		/*const int score =*/ Search(0, alpha, beta, m_Line);

		/*if (score != GetBestScore())
			spdlog::default_logger()->debug("ABIterative: iScore != GetBestScore - when does this happen?");*/

		// Spillet er slut (mat, remis) - ingen grund til at soege videre!!
		if (m_Line.size() != m_Depth)
			break;

		/*if (m_Line.front() != m_BestMove)
			spdlog::default_logger()->debug("ABIterative: iScore != GetBestScore - when does this happen?");*/

		// We've gotten a new move in the PVLine
		ENewPVLineMove.fire(this, m_Line);
	}

	StopTimerAndAdjustVars();

	CheckGameOver(info);

	return GetBestMove(info);
}

// ************************************
// Method:     Search
// Description:En iterativ alpha-beta med PVL, Quiescent og sorterede traek
// FullName:   public ABIterative::Search 
// Returns:    int - 
// Parameter:  unsigned iPly - 
///			 :  int iAlpha - 
///			 :  int iBeta - 
// Parameter:  PVLine& pline - 
// ************************************
int ABIterative::Search(int ply, _In_ int alpha, _In_ int beta, _Inout_ PVLine& pline)
{
	// Check time and stop signal
	if (ShouldStopSearch()) {
		return GameValues::Draw;
	}
	const GameInfo& info = GetLastBoardInfo(ply);

	// Test for 50 moves rule
	if (checkDraws(info, ply))
		return GameValues::Draw;

	// Er vi naaet til bunden af traeet - leaf nodes?
	if (ply == m_Depth)
	{
		assert(pline.size() <= m_Depth);
		// Quiescent seach
		return Quiescent(ply, alpha, beta);
	}

	// Inkrementerer counter - we are not returning early
	m_SearchCount++;

	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(info, moveList);
	// Sorterer traekkene - iterativt
	MoveSorter::SortMovesIter(moveList, GetParentMove(ply), GetIterMove(ply), m_Board);

	bool moveFound = false;

	PVLine line;
	size_t counter = 0;

	for (const auto &curMove : moveList)
	{
		counter++;
		// Foretag traekket hvis det er lovligt - ellers proever vi naeste traek
		if (m_Board.DoMove(curMove))
		{
			// Tilfoejer dette traek til nuvaerende traekfoelge - og opdaterer resten
			AddMoveToSeq(curMove, ply);

			// rekursivt kald til Alpha-Beta
			const int value = -Search(ply + 1, -beta, -alpha, line);

			// Vi er tilbage igen. Undo traekket igen
			m_Board.UndoMove(curMove);

			moveFound = true;

			if (ply == 0 && m_Depth == m_MaxDepth)
				spdlog::default_logger()->debug("Root move {}/{}: {} score={}",
					counter, moveList.size(), curMove.Output(), value);

			//	We got a value back.  We unmade the move.  We're not dead.  Let's
			//	see how good this move was.  If it was >= "beta", it was so good
			//	that we don't need to search for anything better, so we'll leave.
			//
			//	If it was not >= "beta", but it was > "alpha", this is better than
			//	anything else we've found before, but not so good we have to
			//	leave.  These kinds of moves are actually quite rare.  If I find
			//	one of these, I have to store it in the PV line that I'm
			//	constructing.  This might end up being the main-line for the whole
			//	search, if it gets backed up all the way to the root.
			//
			if (value > alpha)		// Er det en bedre vaerdi?
			{
				if (value >= beta)
				{
					//	This move failed high (i.e. too good a move), so we are going to return beta
					assert(pline.size() <= m_Depth);
					return beta;
				}

				//	This move is between alpha and beta, which is actually pretty
				//	rare.  If this happens I have to add a PV move, and append the
				//	returned PV to it, and if I'm at the root I'll send the PV to
				//	the interface so it can display it.
				//
				alpha = value;

				// Hurry and save it in the PVL
				// size is 0 or 1 if we are at Ply 0 and so using m_Line
				pline.assign(1, curMove);
				// flyt resten nedad
				if (!line.empty())		// copy all moves from line to pline
					pline.insert(pline.begin() + 1, line.begin(), line.end());

				assert(pline.size() <= m_Depth);

				if (ply == 0) {		// Er vi i roden af traeet ?
					//Ja, saa saet den nye score
					this->_bestScore = value;
					m_BestMove = curMove;
				}
			}
		}
		else
		{
			if (ply == 0 && m_Depth == m_MaxDepth)
				spdlog::default_logger()->debug("Root move {}/{}: {} ILLEGAL",
					counter, moveList.size(), curMove.Output());
		}
	}
	// Fandt vi nogen lovlige traek?
	if (!moveFound)
	{
		//Nope - i denne gren er vi enten mat eller der er remis!!
		pline.clear();		//TODO: SVN20141229 Why are we clearing the pline here? And why are the assert()s below still here, then?!?
		if (m_Board.InCheck())
		{
			UpdateGameState(ply, m_Board.GetCurrentColor() == eColor::WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
			// Ups...Vi er vist mat her!!
			if (ply == 0)		// Er vi i roden af traeet ?
			{
				//Ja, saa saet den nye score
				_bestScore = -GameValues::Mate;
			}
			assert(pline.size() <= m_Depth);
			return -GameValues::Mate + static_cast<int>(ply);		// returner mate value minus distance to it
		}
		assert(pline.size() <= m_Depth);
		// Remis: Godt hvis vi er bagud, men skidt hvis vi er foran
		UpdateGameState(ply, GameStates::DRAW_PAT);
		return GameValues::Draw;
	}
	UpdateGameState(ply, GameStates::STILL_PLAYING);
	assert(pline.size() <= m_Depth);
	// Vi har et normalt traek, returner den bedste alpha-vaerdi
	return alpha;
}
