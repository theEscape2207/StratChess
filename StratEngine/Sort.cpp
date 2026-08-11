// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Sort.h"

#include "Board.h"
#include "MoveHelper.h"

// Her foretages en findes traekket fra PVLine og rykkes forrest
void MoveSorter::SortMovesIter(MoveList& moveList,   // traeklisten
                               const Move& lastMove, // modstanderens traek foer dette
                               const Move* pIterMove, const Board& board)
{
	size_t foundMove = 0;
	if (pIterMove != nullptr) // Har vi et traek at kigge paa?
	{
		const size_t numMoves = moveList.size();
		// Vi loeber traekkene igennem og sorterer
		for (size_t i = 0; i < numMoves; ++i) {
			// Iterative deepening - bedste traek paa samme dybde fra sidste traek
			if (*pIterMove == moveList[i]) {
				// Bytter om paa de 2 moves
				SwapMoves(moveList, i, foundMove++);
				break; // only one move
			}
		}
	}

	// Sort the rest of the moves
	SortMoves(moveList, lastMove, board, foundMove);
}

// TODO: Convert to use standard value based sorting - it's way faster -
// additionally we are now sorting captures before Promotions even if they are usually a lot faster
// Her foretages en sortering af de lovlige traek
void MoveSorter::SortMoves(MoveList& moveList,   // traeklisten
                           const Move& lastMove, // modstanderens traek foer dette
                           const Board& board,
                           size_t curIndex // hvorfra sorteringen skal starte
)
{
	size_t loop = 0;

	const size_t numMoves = moveList.size();
	// Slag af sidst flyttet brik hvis den eksisterer
	if (!lastMove.is_null()) {
		// Vi loeber traekkene igennem og sorterer videre
		const eSquare lastTo = lastMove.to();
		for (loop = curIndex; loop < numMoves; ++loop) {
			// Slag af sidst flyttet brik
			if (lastTo == moveList[loop].to()) // Det sidste af modstanderens traek!!
			{
				assert(MoveHelper::AsType(moveList[loop]) == MoveType::CAPTURE ||
				       MoveHelper::IsPromote(moveList[loop]));
				// Flytter det fundne traek frem
				SwapMoves(moveList, loop, curIndex++);
			}
		}
	}

	const size_t lastMoveEndIndex =
	    curIndex; // TODO: Right now we are not sorting the last move captures as just found
	              // Test whether we should do it anyway.

	// Vi koerer videre fra hvor vi slap
	for (loop = curIndex; loop < numMoves; ++loop) {
		// Other captures (regular Capture, PromoteCapture and En passant moves) or Promotes?
		if (MoveHelper::IsCapture(moveList[loop]) || MoveHelper::IsPromote(moveList[loop]))
			// Flytter det fundne traek frem
			SwapMoves(moveList, loop, curIndex++);
	}

	// Sorterer moves - diff is number of found interesting moves
	SortMovesByValue(moveList, curIndex - lastMoveEndIndex, board, lastMoveEndIndex);

	// Resten af traekkene er saaledes usorterede, da de ikke falder ind under
	// nogen regel endnu
	// Her kunne der f.eks. vaere skak og andre
}

// Her sorteres de gode slag frem for de mindre gode
// Dvs ikke at ofre sin dronning for at faa den #%&!! bonde ;-)
// Bemaerk: start er default 0
// TODO: Why dont the callers supply iterators instead?
void MoveSorter::SortMovesByValue(MoveList& moveList, size_t captures, const Board& board,
                                  size_t start)
{
	// Sort captures by MVV-LVA: captured piece value minus (moving piece value / 16).
	// board supplies the moving piece (Phase 3) and captured piece (Phase 4) for each move.
	if (captures >= 2) // Mindst 2 for at sortere
		std::sort(moveList.begin() + static_cast<int>(start),
		          moveList.begin() + static_cast<int>(start + captures),
		          [&board](const Move& a, const Move& b) {
			          return MoveHelper::Value(a, board.GetEffectiveMovPiece(a),
			                                   board.GetCapturedPiece(a)) >
			                 MoveHelper::Value(b, board.GetEffectiveMovPiece(b),
			                                   board.GetCapturedPiece(b));
		          });
}

void MoveSorter::ScoreMoves(const MoveList& moveList, int n, const Board& board, eColor side,
                            const Move& pv_move, const Move& hash_move, const Move& killer0,
                            const Move& killer1, const int32_t (&history)[2][64][64],
                            std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx)
{
	assert(n >= 0 && n <= static_cast<int>(MoveList::MAX_MOVES));

	for (int i = 0; i < n; ++i) {
		const Move& mv = moveList[i];
		int s = 0;

		if (mv == pv_move) {
			s = 2'000'000;
		} else if (mv == hash_move) {
			s = 1'900'000;
		} else {
			const bool isCapture = MoveHelper::IsCapture(mv);
			const bool isKiller0 = !isCapture && (mv == killer0);
			const bool isKiller1 = !isKiller0 && !isCapture && (mv == killer1);
			const int mvv_lva =
			    MoveHelper::Value(mv, board.GetEffectiveMovPiece(mv), board.GetCapturedPiece(mv));

			if (isKiller0)
				s = 900'000;
			else if (isKiller1)
				s = 800'000;
			else if (isCapture && mvv_lva > 0)
				s = 1'000'000 + mvv_lva;
			else if (isCapture && mvv_lva == 0)
				s = 700'000 + mvv_lva;
			else if (isCapture)
				s = -100'000 + mvv_lva;
			else {
				assert(static_cast<int>(side) >= 0 && static_cast<int>(side) < 2);
				s = history[static_cast<int>(side)][mv.from()][mv.to()];
			}
		}
		out_scored_idx[i] = {s, i};
	}

	std::sort(out_scored_idx.begin(), out_scored_idx.begin() + n,
	          [](const auto& a, const auto& b) { return a.first > b.first; });
}
