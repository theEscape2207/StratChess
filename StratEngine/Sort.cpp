// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Sort.h"

#include "Board.h"
#include "MoveHelper.h"
#include "See.h"

// Her foretages en findes traekket fra PVLine og rykkes forrest
void MoveSorter::SortMovesIter(MoveList& moveList,   // traeklisten
                               const Move& lastMove, // modstanderens traek foer dette
                               const Move* pIterMove, const Board& board)
{
	size_t foundMove = 0;
	if (pIterMove != nullptr) // Har vi et traek at kigge paa?
	{
		const size_t numMoves = moveList.size();
		//Vi loeber traekkene igennem og sorterer
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
		//Vi loeber traekkene igennem og sorterer videre
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

	const size_t lastMoveEndIndex = curIndex; // TODO: Right now we are not sorting the last move captures as just found
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
void MoveSorter::SortMovesByValue(MoveList& moveList, size_t count, const Board& board, size_t start)
{
	// The range really must be captures and promotions only — see the declaration.
	assert(std::all_of(moveList.begin() + static_cast<int>(start), moveList.begin() + static_cast<int>(start + count),
	                   [](const Move& m) { return MoveHelper::IsCapture(m) || MoveHelper::IsPromote(m); }));

	// Sort captures by MVV-LVA: captured piece value minus (moving piece value / 16).
	// board supplies the moving piece (Phase 3) and captured piece (Phase 4) for each move.
	if (count >= 2) // Mindst 2 for at sortere
		std::sort(moveList.begin() + static_cast<int>(start), moveList.begin() + static_cast<int>(start + count),
		          [&board](const Move& a, const Move& b) {
			          return MoveHelper::Value(a, board.GetEffectiveMovPiece(a), board.GetCapturedPiece(a)) >
			                 MoveHelper::Value(b, board.GetEffectiveMovPiece(b), board.GetCapturedPiece(b));
		          });
}

// The hash move is the top tier. There is deliberately no separate tier above it for the
// previous iteration's principal variation: such a hint exists only along the PV, so it can be
// offered at one node per ply per iteration, and at those nodes it names the move the
// transposition table already names — the entry at a PV node is that node's own store from the
// previous iteration, which is where the hint would come from too. Where the table has nothing
// to offer there, it is because the entry was overwritten, not because the hint knew better
// (#335), and the fix belongs in the table.
//
// This applies to interior PV nodes. Ordering the ROOT's moves by the previous iteration's
// scores is a separate question with a different answer available to it, and nothing here
// forecloses it.
void MoveSorter::ScoreMoves(const MoveList& moveList, int n, const Board& board, eColor side, const Move& hash_move,
                            const Move& killer0, const Move& killer1, const int32_t (&history)[2][64][64],
                            std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx)
{
	assert(n >= 0 && n <= static_cast<int>(MoveList::MAX_MOVES));

	for (int i = 0; i < n; ++i) {
		const Move& mv = moveList[i];
		int s = 0;

		if (mv == hash_move) {
			s = 1'900'000;
		} else if (MoveHelper::IsCapture(mv) || MoveHelper::IsPromote(mv)) {
			// SEE selects the tier; MVV-LVA scores within it. see_ge is boolean and has nothing
			// to say about two captures that land in the same tier, so MoveHelper::Value() stays
			// as the secondary score — it is what ranks PxQ above QxQ.
			//
			// Placing the SEE-equal tier *below* the killers is a chosen policy, not a standard
			// one; the common alternative ranks every SEE >= 0 capture above them. It is what the
			// tier constants here already encoded, so this change moves one thing and not two.
			// #398 is where it gets revisited.
			//
			// A non-capturing promotion is tactical and bypasses SEE entirely. It has no tier at
			// all otherwise: it fails IsCapture(), so it would fall through to history and be
			// ranked against ordinary quiets with no credit for the queen it makes. SEE would be
			// worse than nothing here — it scores a queen promotion onto a defended square as
			// 800 - 900 and files it below the quiets, when the pawn was promoting anyway.
			// MoveHelper::Value() already ranks promotions by promotion gain, so under-promotions
			// stay beneath a queen promotion without a second rule.
			//
			// The losing tier sits below every quiet without a further guard: history is clamped
			// to [0, HISTORY_MAX] and has no penalty path, and every capture's MVV-LVA is at
			// least 44, so this tier tops out below -99'000.
			const int mvv_lva = MoveHelper::Value(mv, board.GetEffectiveMovPiece(mv), board.GetCapturedPiece(mv));

			if (!MoveHelper::IsCapture(mv) || See::see_ge(board, mv, 1))
				s = 1'000'000 + mvv_lva;
			else if (See::see_ge(board, mv, 0))
				s = 700'000 + mvv_lva;
			else
				s = -100'000 + mvv_lva;
		} else if (mv == killer0) {
			s = 900'000;
		} else if (mv == killer1) {
			s = 800'000;
		} else {
			assert(static_cast<int>(side) >= 0 && static_cast<int>(side) < 2);
			s = history[static_cast<int>(side)][mv.from()][mv.to()];
		}
		out_scored_idx[i] = {s, i};
	}

	// Ties break on generation order. std::sort is not stable, and equal scores are common — an
	// in-check quiescence node with a cold history table scores every quiet evasion 0 — so without
	// this the whole tied block is permuted arbitrarily, and differently across stdlib versions.
	std::sort(out_scored_idx.begin(), out_scored_idx.begin() + n, [](const auto& a, const auto& b) {
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	});
}
