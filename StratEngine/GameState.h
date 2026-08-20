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
	STILL_PLAYING, // 0
	WHITE_WON,     // 1
	BLACK_WON,     // 2
	DRAW_PAT,      // 3
	DRAW_50_MOVES, // 4
	HUMAN_EXITED   // 5
};

// The rule is fifty moves by each side, and Board::halfmove_clock() counts halfmoves.
inline constexpr int HALFMOVE_CLOCK_LIMIT = 100;

// Bounds on what a caller may supply, taken from the limits of the game rather than from the
// storage type. The longest possible chess game is 5898.5 moves
// (https://wismuth.com/chess/longest-game.html); the 75-move rule caps the halfmove clock at 150.
// A value past these describes a game that cannot be played, so it is rejected, not repaired.
// These bound the input only -- playing on from a position loaded at the limit is legal.
inline constexpr int MAX_FEN_HALFMOVE_CLOCK = 150;
inline constexpr int MAX_FEN_FULLMOVE_COUNT = 5899;
inline constexpr size_t MAX_UCI_REPLAY_PLIES = 11797;
