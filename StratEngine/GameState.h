#pragma once

#include "MoveHelper.h"
#include "Move.h"
#include <cassert>
#include <cstddef>
#include <cstdint>

// Castling rights bit flags
namespace CastlingRights {
	constexpr uint8_t NONE = 0;
	constexpr uint8_t WHITE_KINGSIDE = 1 << 0;
	constexpr uint8_t WHITE_QUEENSIDE = 1 << 1;
	constexpr uint8_t BLACK_KINGSIDE = 1 << 2;
	constexpr uint8_t BLACK_QUEENSIDE = 1 << 3;
	constexpr uint8_t WHITE_BOTH = WHITE_KINGSIDE | WHITE_QUEENSIDE;
	constexpr uint8_t BLACK_BOTH = BLACK_KINGSIDE | BLACK_QUEENSIDE;
	constexpr uint8_t ALL = WHITE_BOTH | BLACK_BOTH;
} // namespace CastlingRights

// Possible game states
enum class GameStates {
	STILL_PLAYING,  // 0
	WHITE_WON,      // 1
	BLACK_WON,      // 2
	DRAW_PAT,       // 3
	DRAW_50_MOVES,  // 4
	WHITE_RESIGNED, // 5
	BLACK_RESIGNED  // 6
};

// The rule is fifty moves by each side, and Board::halfmove_clock() counts halfmoves.
inline constexpr int HALFMOVE_CLOCK_LIMIT = 100;

// Bounds on what a caller may supply, taken from the limits of the game rather than from the
// storage type: the longest possible chess game is 5898.5 moves, i.e. 11797 halfmoves
// (https://wismuth.com/chess/longest-game.html). A value past these describes a game that cannot be
// played, so it is rejected rather than repaired. They bound the input only -- playing on from a
// position loaded at a bound is legal, and the increments assert against the field maximum instead.
//
// The halfmove clock is bounded by game length, NOT by the 50- or 75-move rule. Those are
// adjudication thresholds, and this parser does not referee: it repairs rule-inconsistent castling
// and en-passant metadata rather than rejecting it. A tighter bound also refuses synthetic test
// positions -- the perft corpus carries clocks up to 253.
inline constexpr int MAX_FEN_HALFMOVE_CLOCK = 11797;
inline constexpr int MAX_FEN_FULLMOVE_COUNT = 5899;
inline constexpr size_t MAX_UCI_REPLAY_PLIES = 11797;
