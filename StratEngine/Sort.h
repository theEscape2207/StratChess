#pragma once
#include "Move.h"
#include <cstdint>

class Board;

class MoveSorter final

{
  public:
	static void SortMoves(MoveList& moveList, const Move& lastMove, const Board& board, size_t startIndex = 0);
	static void SortMovesIter(MoveList& moveList, const Move& lastMove, const Move* pIterMove, const Board& board);
	// Sorts [start, start + count) by MVV-LVA. The caller must have partitioned that range to
	// contain only captures and promotions — MoveHelper::Value() scores a quiet move as
	// -piece/16, so a quiet in the range sorts below every capture and the heaviest quiet
	// sorts last. Asserted, because passing a mixed list is silent in Release (#320).
	static void SortMovesByValue(MoveList& moveList, size_t count, const Board& board, size_t start = 0);
	// Score all moves in [0, n) into out_scored_idx as (score, original_index) pairs,
	// sorted descending by score. Priority: hash move -> winning captures -> killer0 ->
	// killer1 -> equal captures -> history (quiet) -> losing captures.
	static void ScoreMoves(const MoveList& moveList, int n, const Board& board, eColor side, const Move& hash_move,
	                       const Move& killer0, const Move& killer1, const int32_t (&history)[2][64][64],
	                       std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx);
	~MoveSorter() = default;

  private:
	MoveSorter() = default; // Enforce static method calls
	// TODO: This is duplicated in MoveGenerator
	static void
	SwapMoves(MoveList& moveList, size_t first,
	          size_t second) noexcept // TODO: This function is an extension method (friend) and should be moved out
	{
		if (first == second) {
			return;
		}
		std::swap(moveList[first], moveList[second]);
	}
};
