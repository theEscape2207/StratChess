#pragma once
#include "Move.h"
#include <cstdint>

class Board;

// Continuation history rows and columns share one index, a moving piece and its destination: 12 pieces
// by 64 squares.
inline constexpr int kPieceSquares = 12 * 64;
constexpr int continuation_index(ePiece piece, int to) noexcept { return static_cast<int>(piece) * 64 + to; }

// The continuation-history rows a pvs() node reads and updates, nullptr when absent.
struct ContinuationRows {
	int16_t* one_ply = nullptr;
	int16_t* two_ply = nullptr;
	bool empty() const noexcept { return one_ply == nullptr && two_ply == nullptr; }
};

class MoveSorter final

{
  public:
	// Sorts [start, start + count) by MVV-LVA. The caller must have partitioned that range to
	// contain only captures and promotions — MoveHelper::Value() scores a quiet move as
	// -piece/16, so a quiet in the range sorts below every capture and the heaviest quiet
	// sorts last. Asserted, because passing a mixed list is silent in Release (#320).
	static void SortMovesByValue(MoveList& moveList, size_t count, const Board& board, size_t start = 0);
	// The lowest tier above the quiets: a losing capture scores this plus its MVV-LVA value.
	static constexpr int kLosingCaptureTier = 700'000;

	// Score all moves in [0, n) into out_scored_idx as (score, original_index) pairs,
	// sorted descending by score. Priority: hash move -> SEE >= 0 captures and all promotions
	// -> killer0 -> killer1 -> SEE < 0 captures -> quiets, with generation order as the final
	// tie-break. A quiet scores its history entry plus its entry in each continuation row present;
	// quiescence passes none.
	static void ScoreMoves(const MoveList& moveList, int n, const Board& board, eColor side, const Move& hash_move,
	                       const Move& killer0, const Move& killer1, const int32_t (&history)[2][64][64],
	                       std::array<std::pair<int, int>, MoveList::MAX_MOVES>& out_scored_idx,
	                       ContinuationRows cont = {});
	// A quiet move's continuation-history column. board holds the position it is played from.
	static int QuietContinuationColumn(const Board& board, const Move& quiet) noexcept;
	~MoveSorter() = default;

  private:
	MoveSorter() = default; // Enforce static method calls
};
