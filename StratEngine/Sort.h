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

// (score, original_index) pairs, one per move in the list.
using ScoredMoves = std::array<std::pair<int, int>, MoveList::MAX_MOVES>;

// A node's move-ordering inputs, built once per node. The hash move is not here: ScoreAppended
// scores without one. Killers and history are references, so a staged frame reads them when it
// scores its quiets, not when it builds the context. Every member must outlive the context. The side
// whose history row is read is the board's side to move.
struct MoveScoringContext {
	const Board& board;
	const Move& killer0;
	const Move& killer1;
	const int32_t (&history)[2][64][64];
	ContinuationRows cont;
	// ThreadData::capture_history, or nullptr for no history term in the capture scores.
	const int16_t (*capture_history)[8] = nullptr;
};

class MoveSorter final {
  public:
	// Sorts the list by MVV-LVA, descending; equal values keep generation order. The list must
	// contain only captures and promotions — MoveHelper::Value() scores a quiet move as
	// -piece/16, so a quiet sorts below every capture and the heaviest quiet sorts last.
	// Asserted, because passing a mixed list is silent in Release.
	static void SortMovesByValue(MoveList& moveList, const Board& board);
	// The hash move's score, above every tier.
	static constexpr int kHashMoveScore = 1'900'000;
	// A capture SEE does not call losing, or any promotion, scores in [this, this + kCaptureTierSpan):
	// above both killer scores, so it outranks everything ComputeQuiets() can add.
	static constexpr int kGoodCaptureTier = 1'000'000;
	static constexpr int kKiller0Score = 900'000;
	static constexpr int kKiller1Score = 800'000;
	// The lowest tier above the quiets: a losing capture scores in [this, this + kCaptureTierSpan).
	static constexpr int kLosingCaptureTier = 700'000;
	static_assert(kGoodCaptureTier > kKiller0Score && kKiller0Score > kKiller1Score &&
	              kKiller1Score > kLosingCaptureTier);
	// Inside a capture tier: tier + kCaptureHistoryOffset + kCaptureMvvLvaScale * mvv_lva + the
	// capture-history entry. The offset is ThreadData::HISTORY_MAX, asserted there.
	static constexpr int kCaptureHistoryOffset = 16'384;
	static constexpr int kCaptureMvvLvaScale = 16;
	// The largest MoveHelper::Value() of a legal capture or promotion: a queen promotion taking a queen.
	static constexpr int kMaxMvvLva =
	    2 * g_iPieceValues[QUEEN >> 1] - g_iPieceValues[PAWN] - (g_iPieceValues[PAWN] >> 4);
	// Bounds every in-tier score: two history bounds (the offset and the entry) plus the scaled MVV-LVA.
	static constexpr int kCaptureTierSpan = 2 * kCaptureHistoryOffset + kCaptureMvvLvaScale * kMaxMvvLva + 1;
	static_assert(kLosingCaptureTier + kCaptureTierSpan <= kKiller1Score &&
	                  kGoodCaptureTier + kCaptureTierSpan <= kHashMoveScore,
	              "capture scores must stay inside their tier");
	// The row and column of a capture or promotion's capture-history entry.
	static int CaptureHistoryRow(const Board& board, const Move& mv) noexcept;
	static int CaptureHistoryColumn(const Board& board, const Move& mv) noexcept;

	// Score all moves in [0, n) into out_scored_idx as (score, original_index) pairs,
	// sorted descending by score. Priority: hash move -> SEE >= 0 captures and all promotions
	// -> killer0 -> killer1 -> SEE < 0 captures -> quiets, with generation order as the final
	// tie-break. A quiet scores its history entry plus its entry in each continuation row present,
	// and a capture or promotion adds its capture-history entry when the context has the table;
	// quiescence passes neither.
	static void ScoreMoves(const MoveList& moveList, int n, const MoveScoringContext& ctx, const Move& hash_move,
	                       ScoredMoves& out_scored_idx);
	// As ScoreMoves, but only out_scored_idx[0] is in order; [1, n) is unordered until OrderRemaining.
	// For n <= 1 the result is already fully ordered.
	static void ScoreMovesBestFirst(const MoveList& moveList, int n, const MoveScoringContext& ctx,
	                                const Move& hash_move, ScoredMoves& out_scored_idx);
	// Scores the moves appended at [first, n) as ScoreMoves would, for a list with no hash move, and
	// leaves [0, first) alone. Unordered, like ScoreMovesBestFirst's tail.
	static void ScoreAppended(const MoveList& moveList, int first, int n, const MoveScoringContext& ctx,
	                          ScoredMoves& scored_idx);
	// Swaps the best of [first, n) to first, leaving the rest unordered.
	static void SelectBest(const MoveList& moveList, ScoredMoves& scored_idx, int first, int n);
	// Puts [first, n) in ScoreMoves' order. Requires 0 <= first and n <= MAX_MOVES; does nothing when
	// fewer than two entries remain (n - first < 2), so OrderRemaining(.., 1, 0) is a no-op.
	// moveList supplies the profile tie-break key.
	static void OrderRemaining(const MoveList& moveList, ScoredMoves& scored_idx, int first, int n);
	// A quiet move's continuation-history column. board holds the position it is played from.
	static int QuietContinuationColumn(const Board& board, const Move& quiet) noexcept;
	~MoveSorter() = default;

  private:
	MoveSorter() = default; // Enforce static method calls

	// The scoring every ScoreMoves entry point shares: (score, original_index) pairs for [first, n), in
	// generation order.
	static void ScoreUnordered(const MoveList& moveList, int first, int n, const MoveScoringContext& ctx,
	                           const Move& hash_move, ScoredMoves& out_scored_idx);
};
