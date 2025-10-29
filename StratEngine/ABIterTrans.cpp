// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "ABIterTrans.h"
#include "Sort.h"
#include "MoveGenerator.h"

// TODO: For debugging
//#include "MoveHelper.h"

extern std::ofstream outLegalMoves;

// Aspirational Iterative transpositional alpha beta search
// Transposition tables
// FIXME: Transposition tables earlier did not generate the same moves as the other algorithms - i.e. we have a bug!
Move ABIterTrans::GetMove(_Inout_ GameInfo& info)
{
	InitMoveVariables(info);

	// Note: It is needed to Clear the Hashtable due to the possibility that we are getting hits on 
	// positions recorded by the opposition player - using his eval engine which might give incorrect position scoring back
	// So while this is only needed in cases where the AI is playing against himself with two different configurations we skip 
	// this OPTIMIZATION for now - we can likely readd this either 
	// 1) by making the hashtable per player or 
	// 2) just testing on both players using transposition AIs and different Eval Engine versions and only clear then
	// NOTE2: I earlier wrote another comment about why and vaguely refer to something with the PVLine. No idea what I was talking about
	// NOTE3: But I also wrote about 'verify that the moves are valid and not stale'. There might be something about this in the chess litterature
	Board::Instance().ClearHashTable();

	// Aspiration Window Search - size +/- Half a pawn
	const int windowSize = g_iPieceValues[PAWN] >> 1;	// 50

	// Set alpha and beta boundaries around last best score
	// TODO: this solution could be a minor deficiency playing with humans, 
	// where m_iBestScore will not get updated
	int alpha = GetBestScore() - windowSize;
	int beta = GetBestScore() + windowSize;

	StartTimer();

	// iterative search - aspiration edition
	for (m_Depth = 1; m_Depth <= m_MaxDepth; )	// m_Depth maa ikke opdateres her med aspiration search
	{
		//if (TimedOut())
		//	break;

		//Kald vores iterative soegerutine
		const int score = Search(0, alpha, beta, m_Line);

		// Ramte vi udenfor vores vindue ?
		if (IsOutsideWindow(score, alpha, beta))
		{
			// Saa er der sket en stoerre aendring af balancen paa mere end +/- ½ bonde 
			// Vi udvider soegningen i den noedvendige retning til det halve fulde vindue 
			// og proever igen ved samme dybde

			// What direction was it moved ?
			if (score <= alpha)
				alpha = -GameValues::Search_Init;		// reset search boundary
			else
				beta = GameValues::Search_Init;
			continue;
		}

		// Check if game is (almost) over - then no reason to keep searching 
		// FIXME: Argh - the reason that this triggers way more often than in 
		// ABIterative is that for longer sequences we start getting hash table hits 
		// obviously causing the PVLine not to be filled out as expected - we are not searching, so not creating it
		// in order for this to work, we need to store the chain of moves in the table!
		if ((abs(score) > GameValues::Mate - 10) || (info.gameState == GameStates::DRAW_50_MOVES))
			break;
		assert(m_Line.size() <= m_Depth); // FIXME: We are getting many hits with m_Line being shorter than m_Depth, probably due to hashtable hits

		// We've gotten a new move in the PVLine - send it to listeners
		ENewPVLineMove.fire(this, m_Line);

		// Ok, vi skal soege videre. Vi ramte indenfor, saa soeg dybere med nyt, tilpasset vindue omkring
		alpha = score - windowSize;
		beta = score + windowSize;

		++m_Depth;	// _Skal_ opdateres her, da "continue" bliver brugt ovenover
	}

	StopTimer();

	CheckGameOver(info);

	return GetBestMove(info);
}

// An iterative alpha-beta with PVL and transposition table
// FIXME: Transposition tables still doesn't work correctly 
// - ABIterTrans doesn't assert, but does not generate the same moves as the non-trans search algos
int ABIterTrans::Search(_In_ size_t ply, _In_ int alpha, _In_ int beta, _Inout_ PVLine& pline)
{
	const GameInfo& info = GetLastBoardInfo(ply);

	// Test for 50 moves rule
	if (IsFiftyMoves(info))
		return GameValues::Draw;

	if (ply)
	{
		// FIXME: This is crap! GetIterMove() returns NULL if theres no current best move
		// Because we know the internals of GetIterMove and RecordHash, we know that currentBestMove will only be 
		// used inside RecordHash() when GetIterMove() returns non-NULL
		//const Move HashMove = Move::EmptyMove(); // info.lastMove; //GetIterMove( iPly );

		// FIXME: We have a problem with the iterLine

		// Check Transposition table if we know this move
		const auto& hashScore = m_Board.ProbeHash(ply, alpha, beta);
		if (hashScore.first != GameValues::Unknown_Hash) // we found something
		{
			assert(m_Board.IsLegalMove(hashScore.second));
			assert(pline.size() <= m_Depth);
			return hashScore.first;

			//	We got a value back from the Hash table.  Let's
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

			//if (iValue > iAlpha)		// Er det en bedre vaerdi?
			//{
			//	// Er den _for_ stor ?
			//	if (iValue >= iBeta)		// Cutoff !
			//	{
			//		return iBeta;
			//	}
			//	// ellers saa er det en ny bedste vaerdi
			//	goodMove = curMove;		// Set the new best move
			//
			//	// den nye bedste vaerdi gemmes i PVL
			//	// size is 0 or 1 if we are at Ply 0 and so using m_Line
			//	pline.assign(1, curMove);
			//	// flyt resten nedad
			//	if (!line.empty())		// copy all moves from line to pline
			//		pline.insert(pline.begin()+1, line.begin(), line.end());
			//
			//	if (iPly == 0)		// Er vi i roden af traeet?
			//		_bestScore = iValue;
			//}

		}

		// Er vi naaet til bunden af traeet - evaluering?
		//	See if static eval will cause a cutoff or raise alpha.
		if (ply == m_Depth)
		{
			assert(pline.size() <= m_Depth);

			return Quiescent(ply, alpha, beta);
		}
	}

	int value = 0;

	// Increment counter - we are not returning early
	m_SearchCount++;

	MoveList moveList;
	moveList.reserve(MAX_PLY*8);

	// Henter de lovlige traek
	MoveGenerator::ComputeLegalMoves(info, moveList);

	// Sorterer traekkene
	MoveSorter::SortMovesIter(moveList, GetParentMove(ply), // Last move
		GetIterMove(ply));

	// Ny PVL
	PVLine line;

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
			value = -Search(ply + 1, -beta, -alpha, line);

			// Vi er tilbage igen. Undo traekket igen
			m_Board.UndoMove(curMove);

			moveFound = true;

#ifdef PRINT_MOVES
			// Udskriver traekkene til fil
			if (ply == 0 && m_Depth == m_MaxDepth)	// for iterativ udskriv kun i roden af traeet
				PrintMovesAndScore(outLegalMoves, counter, moveList.size(), curMove, value);
#endif // PRINT_MOVES

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

				// den nye bedste vaerdi gemmes i PVL
				// size is 0 or 1 if we are at Ply 0 and so using m_Line
				// assert(pline.size() <= m_Depth); // Denne assert tror jeg faktisk er direkte forkert
				// - den rammer pline.size() =3 og m_Depth=1 - kan jo godt ske her naar der bliver fundet en ny bedre variant
				pline.assign(1, curMove);
				// flyt resten nedad
				if (!line.empty())		// copy all moves from line to pline
					pline.insert(pline.begin() + 1, line.begin(), line.end());

				assert(pline.size() <= m_Depth);
				if (ply == 0)		// Er vi i roden af traeet?
					_bestScore = value;
			}
		}
#ifdef PRINT_MOVES
		else
		{
			// Udskriver traekkene til fil
			if (ply == 0 && m_Depth == m_MaxDepth)	// for iterativ kun udskriv nederste dybde
				// Ulovligt traek!!														// Magic value: illegal move
				PrintMovesAndScore(outLegalMoves, counter, moveList.size(), curMove, -GameValues::Search_Init - 1);
		}
#endif // PRINT_MOVES
	}
	// Any legal moves found?
	if (!moveFound)
	{
		// Nope - so we are either mate or remis here !
		// Denne gren indeholder ingen traek til PVLine
		pline.clear();
		if (m_Board.InCheck())
		{
			// Oops - we are mate!!
			UpdateGameState(ply, m_Board.GetCurrentColor() == WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
			if (ply == 0)		// Are we at root?
			{
				_bestScore = -GameValues::Mate;
			}
			// We are not saving anything in the hash as we have no move here

			return -GameValues::Mate + static_cast<int>(ply);		// returner mate value minus distance to it
		}
		// Else No move and not in check - Pat!

		// We are not saving anything in the hash as we have no move here
		UpdateGameState(ply, GameStates::DRAW_PAT);
		return GameValues::Draw;
	}

	UpdateGameState(ply, GameStates::STILL_PLAYING);
	// Vi gemmer liige vaerdien i vores transposition table foerst
	//assert(m_Board.IsLegalMove(goodMove));		// <-- That one insta-asserts!! It's no better than alpha, so no move is persisted
	m_Board.RecordHash(ply, alpha, hashf, goodMove);

	// Vi har et normalt traek, returner den bedste alpha-vaerdi
	return alpha;
}
