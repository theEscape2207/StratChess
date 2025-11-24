// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "AITrans.h"
#include "Sort.h"
#include "MoveGenerator.h"

extern std::ofstream outLegalMoves;

// Transposition tables
// FIXME: Transposition tables do not generate the same moves as the other algorithms - i.e. we have a bug!
Move AITrans::GetMove(_Inout_ GameInfo& info)
{
	InitMoveVariables(info);

	// Note: It is needed to Clear the Hashtable due to the possibility that we are getting hits on 
	// positions recorded by the opposition player - using his eval engine which might give incorrect position scoring back
	// So while this is only needed in cases where the AI is playing against himself with two different configurations we skip 
	// this OPTIMIZATION for now - we can likely readd this either 
	// 1) by making the hashtable per player or 
	// 2) just testing on both players using transposition AIs and different Eval Engine versions and only clear then
	// NOTE2: But I also wrote about 'verify that the moves are valid and not stale'. There might be something about this in the chess litterature
	Board::Instance().ClearHashTable();

	StartTimer();

	//Kalder den Almindelige rekursive alphabeta soegning
	Search(0, -GameValues::Search_Init, GameValues::Search_Init);

	StopTimerAndAdjustVars();

	CheckGameOver(info);

	return GetBestMove(info);
}

// An alpha-beta with transposition table
// FIXME: Transposition tables still doesn't work correctly
// - AITrans simply asserts atm - and not every time at that :( :( Most likely some bug in the hash probing / storing code around mate positions
int AITrans::Search(_In_ size_t ply, _In_ int alpha, _In_ int beta)
{
	// Check time and stop signal
	if (ShouldStopSearch()) {
		return GameValues::Draw;
	}
	const GameInfo& info = GetLastBoardInfo(ply);

	// Test for 50 moves rule
	if (checkDraws(info, ply))
		return GameValues::Draw;

	if (ply)	// Det her behoever naturligvis ikke vaere det bedste traek i denne hoejde, men vi kan altid bruge det i traeksorteringen
	{
		// Check Transposition table if we know this move
		const auto& hashScore = m_Board.ProbeHash(ply, alpha, beta);
		if (hashScore.first != GameValues::Unknown_Hash)
		{
			assert(m_Board.IsLegalMove(hashScore.second));
			return hashScore.first;		// Yay! We found something interesting
		}
	}

	// Er vi naaet til bunden af traeet - evaluering?
	//	See if static eval will cause a cutoff or raise alpha.
	if (ply == m_MaxDepth)
	{
		// Quiescent seach
		return Quiescent(ply, alpha, beta);
	}

	int value = 0;

	// Increment counter - we are not returning early
	m_SearchCount++;

	MoveList moveList;
	moveList.reserve(static_cast<size_t>(MAX_PLY)*8);

	// Henter de lovlige traek
	MoveGenerator::ComputeLegalMoves(info, moveList);

	// Sorterer traekkene
	MoveSorter::SortMoves(moveList, GetParentMove(ply));

	// Set hash-flag
	eHashFlags hashf = eHashFlags::hashfALPHA;

	// Tjek om der er lovlige brugbare traek her
	bool moveFound = false;
	Move goodMove;
	size_t counter = 0;

	for (const auto &curMove : moveList)
	{
		counter++;
		// Foretag traekket hvis det er lovligt - ellers proever vi naeste traek
		if (m_Board.DoMove(curMove))
		{
			// Tilfoejer dette traek til nuvaerende traekfoelge
			AddMoveToSeq(curMove, ply);

			// rekursivt kald til Alpha-Beta
			value = -Search(ply + 1, -beta, -alpha);

			// Vi er tilbage igen. Undo traekket igen
			m_Board.UndoMove(curMove);

			moveFound = true;

#ifdef PRINT_MOVES
			// Udskriver traekkene til fil
			if (ply == 0)
				PrintMovesAndScore(outLegalMoves, counter, moveList.size(), curMove, value);
#endif // PRINT_MOVES

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
				// Er den _for_ stor ?
				if (value >= beta)		// Cutoff !
				{
					// Vi gemmer liige vaerdien i vores transposition table foerst
					m_Board.RecordHash(ply, beta, eHashFlags::hashfBETA, curMove);
					return beta;
				}
				// ellers saa er det en ny bedste vaerdi
				hashf = eHashFlags::hashfEXACT;
				alpha = value;
				goodMove = curMove;		// Set the new best move

				if (ply == 0) {		// Er vi i roden af traeet?
					// Saa er traekket BEST Move indtil videre 
					m_BestMove = curMove;
					this->_bestScore = value;
				}
			}
		}
#ifdef PRINT_MOVES
		else
		{
			// Udskriver traekkene til fil
			if (ply == 0)
				// Ulovligt traek!!														// Magic value: illegal move
				PrintMovesAndScore(outLegalMoves, counter, moveList.size(), curMove, -GameValues::Search_Init - 1);
		}
#endif // PRINT_MOVES
	}
	// Any legal moves found?
	if (!moveFound)
	{
		// Nope - so we are either mate or remis here !
		// We are not saving anything in the hash as we have no move here

		if (m_Board.InCheck())
		{
			// Mate!
			UpdateGameState(ply, m_Board.GetCurrentColor() == WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
			if (ply == 0)		// Are we at the root?
			{
				// Yes, then set the new score
				this->_bestScore = -GameValues::Mate;
			}
			return -GameValues::Mate + static_cast<int>(ply);		// returner mate value minus distance to it
		}
		// Else No move and not in check - Pat!

		UpdateGameState(ply, GameStates::DRAW_PAT);
		return GameValues::Draw;
	}

	UpdateGameState(ply, GameStates::STILL_PLAYING);
	// Vi gemmer liige vaerdien i vores transposition table foerst - baade alpha og exact
	//assert(m_Board.IsLegalMove(goodMove));
	m_Board.RecordHash(ply, alpha, hashf, goodMove);
	// Vi har et normalt traek, returner den bedste alpha-vaerdi
	return alpha;
}
