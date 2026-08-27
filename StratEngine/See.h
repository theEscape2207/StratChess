#pragma once

#include "Move.h"

class Board;

namespace See {
	// True if the static exchange evaluation of `move` is at least `threshold` centipawns — the
	// material the mover keeps once both sides have played out the exchange on move.to().
	//
	// Boolean, not a centipawn value: both consumers want the same predicate, see_ge(m, 0) —
	// ordering to pick the capture tier, pruning to reject SEE < 0. The boolean form is also what
	// lets the swap loop stop the moment the answer is decided instead of unwinding the whole
	// list. An int-valued see() can be added beside this if a consumer for it ever appears.
	//
	// `move` must be a capture or a promotion, and must not yet have been applied to the board.
	//
	// A *non-capturing* promotion can return false here — a queen promotion onto a defended square
	// is see_ge(m, 0) == false — so a caller that must not prune or demote promotions has to
	// exclude them itself. MoveSorter::ScoreMoves does, via a !IsCapture() short-circuit.
	[[nodiscard]] bool see_ge(const Board& board, const Move& move, int threshold) noexcept;
} // namespace See
