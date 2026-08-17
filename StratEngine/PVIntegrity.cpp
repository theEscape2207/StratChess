#include "StdAfx.h"
#include "PVIntegrity.h"

#include "Board.h"
#include "MoveGenerator.h"

namespace {

	// Move equality compares from/to only, so the PV's promotion piece — and the difference
	// between a quiet move and a capture on the same squares — has to be compared here.
	bool same_move_exactly(const Move& a, const Move& b) noexcept
	{
		return a.from() == b.from() && a.to() == b.to() && a.flags() == b.flags();
	}

} // namespace

bool pv_replays_legally(const Board& root, std::span<const Move> line)
{
	Board board = root;

	for (const Move& move : line) {
		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, board.GetGameInfo(), moves);

		const bool generated = std::any_of(
		    moves.begin(), moves.end(), [&move](const Move& candidate) { return same_move_exactly(candidate, move); });
		if (!generated)
			return false;

		// The generated list is pseudo-legal, so this is where a move that leaves its own king
		// in check is rejected.
		if (!board.DoMove(move))
			return false;

		// Every replayed move is permanent here — nothing is undone — so the ply-history arrays
		// only ever need to span one move at a time.
		board.ResetSearchDepth();
	}

	return true;
}
