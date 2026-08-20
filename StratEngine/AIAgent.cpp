// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "AIAgent.h"
#include "Sort.h"
#include "MoveGenerator.h"
#include "MoveFormatter.h"

// Fetches the next move
SearchResult AIAgent::GetMove(const SearchLimits& limits)
{
	InitMoveVariables();

	// Aspiration Window Search - size +/- Half a pawn
	const int windowSize = g_iPieceValues[PAWN] >> 1; // 50

	// Set alpha and beta boundaries around last best score
	// TODO: this solution could be a minor deficiency playing with humans,
	// where m_iBestScore is not getting updated
	int alpha = GetBestScore() - windowSize;
	int beta = GetBestScore() + windowSize;

	ApplyLimits(limits);

	// iterativ search
	for (depth_ = 1; depth_ <= effective_depth_;) // depth_ maa ikke opdateres her med aspiration search
	{
		if (StopRequested()) {
			break;
		}
		//Kald vores iterative soegerutine
		const int score = Search(0, alpha, beta, m_Line);

		if (IsOutsideWindow(score, alpha, beta)) {
			// Saa er der sket en stoerre aendring af balancen paa mere end +/- ½ bonde
			// Vi udvider soegningen i den noedvendige retning til det halve fulde vindue
			// og proever igen ved samme dybde

			// Er vaerdien over eller under vinduet ?
			if (score <= alpha)
				alpha = -GameValues::Search_Init; // reset search boundary
			else
				beta = GameValues::Search_Init;
			continue;
		}

		// Vi ramte indenfor, saa soeg dybere med nyt vindue omkring
		alpha = score - windowSize;
		beta = score + windowSize;

		// Spillet er slut (mat, remis) - ingen grund til at soege videre!!
		if (m_Line.size() != depth_)
			break;

		// We've gotten a new move in the PVLine - send it to listeners
		ENewPVLineMove.fire(this, m_Line);

		++depth_; // _Skal_ opdateres her, da "continue" bliver brugt ovenover
	}

	StopTimerAndAdjustVars(m_SearchCount);

	return MakeResult();
}

// En iterativ alpha-beta with PVL and PVS
int AIAgent::Search(size_t ply, int alpha, int beta, PVLine& pline)
{
	// Check time and stop signal
	if (StopRequested()) {
		return GameValues::Draw;
	}

	// Test for 50 moves rule
	if (checkDraws(static_cast<int>(ply)))
		return GameValues::Draw;

	// Er vi naaet til bunden af traeet - leaf nodes?
	if (ply == depth_) {
		// herfra skal vi begynde at lave vores PV Line
		return Quiescent(ply, alpha, beta);
	}

	// Inkrementerer counter - we are not returning early
	m_SearchCount++;

	// Henter de lovlige traek
	MoveList moveList;
	MoveGenerator::ComputeLegalMoves(m_Board, moveList);

	// Sorterer traekkene
	MoveSorter::SortMovesIter(moveList, GetParentMove(), // Last move
	                          GetIterMove(ply), m_Board);
	int score = 0;

	// Tjek om der er lovlige brugbare traek her
	bool moveFound = false;

	// Ny PVL
	PVLine line;
	size_t counter = 0;

	// vi loeber dem allesammen igennem
	for (const auto& curMove : moveList) {
		counter++;

		// Foretag traekket hvis det er lovligt - ellers proever vi naeste traek
		if (m_Board.DoMove(curMove)) {
			// Proev foerste traek for at se om det er godt PVS - det burde vaere det bedste
			if (curMove == moveList.front()) {
				// rekursivt kald til Alpha-Beta
				score = -Search(ply + 1, -beta, -alpha, line);
			} else // alle andre traek
			{
				// Vi regner med at alle andre vaerdier er daarligere!! Dvs sorteringen skal vaere rigtig god
				// rekursivt kald til Alpha-Beta PVS - minimalt vindue for hurtig cutoff eller daarligt traek
				score = -Search(ply + 1, -alpha - 1, -alpha, line);
				//Hvis scoren er imellem, saa maa vi soege endnu en gang
				if (score > alpha && score < beta)
					score = -Search(ply + 1, -beta, -alpha, line);
			}

			// Vi er tilbage igen. Undo traekket igen
			m_Board.UndoMove(curMove);

			moveFound = true;

			if (ply == 0 && depth_ == effective_depth_)
				spdlog::default_logger()->debug("Root move {}/{}: {} score={}", counter, moveList.size(),
				                                MoveFormatter::ToCoord(curMove), score);

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
			if (score > alpha) // Er det en bedre vaerdi?
			{
				// Er den _for_ stor ?
				if (score >= beta) // Cutoff !
					return beta;

				// ellers saa er det en ny bedste vaerdi
				alpha = score;

				// den nye bedste vaerdi gemmes i PVL
				// size is 0 (or 1 if we are at Ply 0) and so using m_Line
				// her erases pline content og current move kopieres ind
				pline.assign(1, curMove);
				// Den nye bedste linje kopieres ind bagefter
				if (!line.empty()) // copy all moves from line to pline
					pline.insert(pline.begin() + 1, line.begin(), line.end());

				// Er vi i roden af traeet ?
				if (ply == 0)
					// Saa saet scoren
					this->_bestScore = score;
			}
		} else {
			if (ply == 0 && depth_ == effective_depth_)
				spdlog::default_logger()->debug("Root move {}/{}: {} ILLEGAL", counter, moveList.size(),
				                                MoveFormatter::ToCoord(curMove));
		}
	}

	// Fandt vi nogen lovlige traek?
	if (!moveFound) {
		//Nope - i denne gren er vi enten mat eller der er remis!!
		pline.clear(); // Denne gren indeholder ingen traek til PVLine
		// FIXME: Er ovenstaaende rigtigt!! Er det meningen at vi skal toemme linjen her?
		if (m_Board.InCheck()) {
			// Ups...Vi er vist mat her!!
			UpdateGameState(ply, m_Board.GetCurrentColor() == WHITE ? GameStates::BLACK_WON : GameStates::WHITE_WON);
			if (ply == 0) // Er vi i roden af traeet ?
			{
				//Ja, saa saet den nye score
				this->_bestScore = -GameValues::Mate;
			}
			return -GameValues::Mate + static_cast<int>(ply); // returner mate value minus distance to it
		}
		// Remis: Godt hvis vi er bagud, men skidt hvis vi er foran
		UpdateGameState(ply, GameStates::DRAW_PAT);
		return GameValues::Draw;
	}

	UpdateGameState(ply, GameStates::STILL_PLAYING);
	// Vi har et normalt traek, returner den bedste alpha-vaerdi
	return alpha;
}
