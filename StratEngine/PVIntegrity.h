#pragma once
#include "Move.h"
#include <span>

class Board;

// True when `line` can be played out from `root`, move by move, each move legal in the position
// the one before it produced. An empty line replays trivially.
//
// This is what a reported "info ... pv" claims, and the only property a GUI can check us on: the
// search asserts it at the emission choke point (AIPerplex::emit_iteration_info) rather than
// trusting that every path through the PV table produced a coherent row.
//
// Both halves of legality are needed and they are not interchangeable.
// MoveGenerator::ComputeLegalMoves is pseudo-legal — it does not test check — while Board::DoMove
// returns false only when the move leaves its own king in check, and otherwise executes whatever
// from/to/flags triple it is handed, including a geometrically impossible one. So membership in
// the generated list is tested first, and only then the move is played. Membership compares flags
// explicitly, because Move equality ignores them and would accept a PV move naming a different
// promotion piece.
//
// `root` is taken by const reference and copied internally: replaying mutates a board, and the
// caller's is the live search position.
[[nodiscard]] bool pv_replays_legally(const Board& root, std::span<const Move> line);
