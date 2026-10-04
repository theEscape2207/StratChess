#pragma once

#include <array>
#include <cstdint>

// Bitboard index of the union of every piece.
inline constexpr auto ALL_PIECES = 14;

inline constexpr auto ALL_BITBOARDS = 15;
inline constexpr auto ALL_SQUARES = 64;
inline constexpr auto ALL_PIECETYPES = 12;

enum eColor : uint8_t { WHITE = 0, BLACK = 1, NUM_COLORS = 2 };

enum ePieceType : uint8_t { PAWN = 0, KNIGHT = 2, BISHOP = 4, ROOK = 6, QUEEN = 8, KING = 10, ALL_FROM_COLOR = 12 };

// Indexes the per-piece bitboards: piece type plus colour.
enum ePiece : uint8_t {
	WHITE_PAWN = 0,
	BLACK_PAWN,
	WHITE_KNIGHT,
	BLACK_KNIGHT,
	WHITE_BISHOP,
	BLACK_BISHOP,
	WHITE_ROOK,
	BLACK_ROOK,
	WHITE_QUEEN,
	BLACK_QUEEN,
	WHITE_KING,
	BLACK_KING,
	ALL_WHITE_PIECES,
	ALL_BLACK_PIECES, // = 13
	NO_PIECE = 15
};

// Special moves
// Flag bit layout: bit3 = promotion, bit2 = capture
// Values 0-5 and 8-11 are existing types; 12-15 are promotion-captures (bits 3+2 both set).
enum class MoveType : uint8_t {
	QUIET = 0,
	DOUBLE_PAWN_PUSH = 1,
	KING_CASTLE = 2,
	QUEEN_CASTLE = 3,
	CAPTURE = 4,          // bit2 set
	EP_CAPTURE = 5,       // bit2 set
	PROMOTION_KNIGHT = 8, // bit3 set
	PROMOTION_BISHOP = 9,
	PROMOTION_ROOK = 10,
	PROMOTION_QUEEN = 11,
	PROMOTION_KNIGHT_CAPTURE = 12, // bits 3+2 set — promotion that also captures
	PROMOTION_BISHOP_CAPTURE = 13,
	PROMOTION_ROOK_CAPTURE = 14,
	PROMOTION_QUEEN_CAPTURE = 15,
};

// NO_ROW requires a signed representation.
enum eRowNames {
	NO_ROW = -1,
	BLACK_BACK_ROW = 0,
	WHITE_7TH_ROW = 1, // Row indices count from rank 8: White's 7th rank is row 1
	BLACK_7TH_ROW = 6,
	WHITE_BACK_ROW = 7
}; // Cannot easily be converted to enum class

enum eFileNames : uint8_t { LEFT_FILE = 0, RIGHT_FILE = 7 };

// Square names; a8 is 0 and h1 is 63.
// clang-format off
// Laid out as the board itself, eight squares per rank. One enumerator per line
// is technically equivalent and unreadable.
enum eSquare {
	a8 = 0, b8, c8, d8, e8, f8, g8, h8,
	a7, b7, c7, d7, e7, f7, g7, h7,
	a6, b6, c6, d6, e6, f6, g6, h6,
	a5, b5, c5, d5, e5, f5, g5, h5,
	a4, b4, c4, d4, e4, f4, g4, h4,
	a3, b3, c3, d3, e3, f3, g3, h3,
	a2, b2, c2, d2, e2, f2, g2, h2,
	a1, b1, c1, d1, e1, f1, g1, h1 = 63,
	NUM_SQUARES, NO_SQUARE = 255
};
// clang-format on

// Maximum search ply; bounds every ply-indexed array in the search.
constexpr auto MAX_PLY = 256;

// Named constants used inline in arithmetic, never a stored field.
enum GameValues : int {
	Draw = 0,
	Mate_Threshold = 29900, // At or above this magnitude a score is a mate
	Mate = 30000,           // Mate at the root; mate in n plies scores Mate - n
	Search_Init = 50000     // Initial alpha-beta bounds, outside every real score
};

// One bit per square, indexed by eSquare: bit 0 is a8 and bit 63 is h1.
using BITBOARD = std::uint64_t;

inline constexpr auto ONE_ROW = 8;
inline constexpr auto TWO_ROWS = 16;

inline constexpr BITBOARD UNIT = 1;
inline constexpr BITBOARD EMPTY = 0;

#define File(x) ((x) & 7)
#define Rank(x) ((x) >> 3)

inline constexpr BITBOARD MASK_RANK_1 = 0xff00000000000000U;
inline constexpr BITBOARD MASK_RANK_2 = 0x00ff000000000000;
inline constexpr BITBOARD MASK_RANK_3 = 0x0000ff0000000000;
inline constexpr BITBOARD MASK_RANK_4 = 0x000000ff00000000;
inline constexpr BITBOARD MASK_RANK_5 = 0x00000000ff000000U;
inline constexpr BITBOARD MASK_RANK_6 = 0x0000000000ff0000;
inline constexpr BITBOARD MASK_RANK_7 = 0x000000000000ff00;
inline constexpr BITBOARD MASK_RANK_8 = 0x00000000000000ff;

// constexpr, not const: Eval.h's Lazy SMP sharing contract already claims this
// table is compile-time-initialised, and a runtime-initialised array is also a
// memory load the optimiser cannot fold into a file loop over it.
inline constexpr BITBOARD g_bbFileMask[] = {
    0x0101010101010101, 0x0202020202020202, 0x0404040404040404, 0x0808080808080808,
    0x1010101010101010, 0x2020202020202020, 0x4040404040404040, 0x8080808080808080U,
};

// Static piece values, indexed by (piece >> 1).
// Sized to the whole ePiece range rather than to ALL_PIECETYPES: the aggregate
// entries and NO_PIECE also shift down into this table (12 >> 1 == 13 >> 1 == 6,
// 15 >> 1 == 7), so a six-element table leaves those indices out of bounds. They
// score zero — no piece, no material.
// int, not unsigned: callers use these in signed score arithmetic.
// clang-format off
inline constexpr int g_iPieceValues[(ePiece::NO_PIECE >> 1) + 1] = {
	100, 		// Pawn
	300, 		// Knight
	300, 		// Bishop
	500, 		// Rook
	900, 		// Queen
	10000,		// King
	0, 			// ALL_WHITE_PIECES / ALL_BLACK_PIECES
	0			// NO_PIECE
};
// clang-format on

// clang-format off
// Piece-square tables, one 8x8 board per piece, read as the board is drawn: rank 8
// at the top, file A on the left, with the trailing /* n */ markers naming the rank.
// Reflowing these to the column limit destroys the only property that makes them
// checkable by eye.
inline constexpr short g_Eval_Bitboards[][ALL_SQUARES] =
{
	{
		/* Pawn positional values */
		0,  0,  0,  0,  0,  0,  0,  0,  /* 8 */
		24, 28, 32, 35, 35, 32, 28, 24, /* 7 */
		5, 10, 16, 25, 25, 16, 10,  5,  /* 6 */
		4,  8, 14, 19, 19, 14,  8,  4,  /* 5 */
		1,  3,  7, 14, 14,  7,  3,  1,  /* 4 */
		1,  2,  5,  7,  7,  5,  2,  1,  /* 3 */
		1,  1,  2, -8, -8,  2,  1,  1,  /* 2 */
		0,  0,  0,  0,  0,  0,  0,  0 },/* 1 */
		/* A   B   C   D   E   F   G   H  */

	/* Knight positional values */
   {
	   -14,-10, -7, -5, -5, -7,-10,-14,
	   -10, -5,  2,  3,  3,  2, -5,-10,
	   -4,  5,  7,  7,  7,  7,  5, -4,
	   -5,  6,  8, 10, 10,  8,  6, -5,
	   -6,  3,  8, 10, 10,  8,  3, -6,
	   -8,  0,  5,  8,  8,  5,  0, -8,
	   -10, -8,  0,  1,  1,  0, -8,-10,
	   -16,-10,-10, -8, -8,-10,-10,-16
   },

	/* Bishop positional values */
	{
		2,  7,  7,  7,  7,  7,  7,  2,
		2,  4,  4,  4,  4,  4,  4,  2,
		4,  6,  7,  7,  7,  7,  6,  4,
		4,  6,  8,  8,  8,  8,  6,  4,
		0,  4,  6,  8,  8,  6,  4,  0,
		0,  4,  5,  6,  6,  5,  4,  0,
		0,  6,  5,  5,  5,  5,  6,  0,
		-8, -8, -6, -6, -6, -6, -8, -8
	},

	/* Rook positional values */
	{
		0,  1,  2,  3,  3,  2,  1,  0,
		2,  4,  6,  6,  6,  6,  4,  2,
		3,  4,  5,  6,  6,  5,  4,  3,
		-2,  0,  4,  5,  5,  4,  0, -2,
		-4, -2,  3,  5,  5,  3, -2, -4,
		-6, -2,  3,  5,  5,  3, -2, -6,
		-6, -2,  4,  5,  5,  4, -2, -6,
		-0,  0,  0,  0,  0,  0,  0,  0
	},

	/* Queen positional values */
	{
		0,  0,  0,  0,  0,  0,  0,  0,
		0,  2,  3,  4,  4,  3,  2,  0,
		0,  2,  4,  5,  5,  4,  2,  0,
		0,  2,  4,  8,  8,  4,  2,  0,
		-1,  0,  3,  6,  6,  3,  0, -1,
		-1,  0,  2,  3,  3,  2,  0, -1,
		-2, -1,  0,  0,  0,  0, -1, -2,
		-3, -2, -1,  0,  0, -1, -2, -3 },

		/* King positional values - opening and middlegame.
		   Flat: middlegame king placement is priced by the king-safety terms
		   (shelter, storm, file openness, attack pressure), which read the pawns
		   actually in front of the king rather than its rank alone. */

	{
		0,   0,   0,   0,   0,   0,   0,   0,
		0,   0,   0,   0,   0,   0,   0,   0,
		0,   0,   0,   0,   0,   0,   0,   0,
		0,   0,   0,   0,   0,   0,   0,   0,
		0,   0,   0,   0,   0,   0,   0,   0,
		0,   0,   0,   0,   0,   0,   0,   0,
		0,   0,   0,   0,   0,   0,   0,   0,
		0,   0,   0,   0,   0,   0,   0,   0 },


		/* King positional values - endgame*/
		 {
			 0,   10,  20,  30,  30,  20,  10,   0,
			 10,  20,  30,  40,  40,  30,  20,  10,
			 20,  30,  40,  50,  50,  40,  30,  20,
			 30,  40,  50,  60,  60,  50,  40,  30,
			 30,  40,  50,  60,  60,  50,  40,  30,
			 20,  30,  40,  50,  50,  40,  30,  20,
			 10,  20,  30,  40,  40,  30,  20,  10,
			 0,   10,  20,  30,  30,  20,  10,   0
		 }
};
// clang-format on

// Setup short piece names
inline constexpr auto initPieceNames()
{
	std::array<char, ALL_PIECETYPES + 1> names{};
	names[WHITE_PAWN] = 'P';
	names[BLACK_PAWN] = 'p';
	names[WHITE_KNIGHT] = 'N';
	names[BLACK_KNIGHT] = 'n';
	names[WHITE_BISHOP] = 'B';
	names[BLACK_BISHOP] = 'b';
	names[WHITE_ROOK] = 'R';
	names[BLACK_ROOK] = 'r';
	names[WHITE_QUEEN] = 'Q';
	names[BLACK_QUEEN] = 'q';
	names[WHITE_KING] = 'K';
	names[BLACK_KING] = 'k';
	names[ALL_PIECETYPES] = '.';
	return names;
}

inline constexpr auto g_cPieceNames = initPieceNames();

// Setup verbose piece names
inline constexpr auto initLongPieceNames()
{
	std::array<const char*, ALL_PIECETYPES> names{};
	names[WHITE_PAWN] = "White pawn";
	names[BLACK_PAWN] = "Black pawn";
	names[WHITE_KNIGHT] = "White knight";
	names[BLACK_KNIGHT] = "Black knight";
	names[WHITE_BISHOP] = "White bishop";
	names[BLACK_BISHOP] = "Black bishop";
	names[WHITE_ROOK] = "White rook";
	names[BLACK_ROOK] = "Black rook";
	names[WHITE_QUEEN] = "White queen";
	names[BLACK_QUEEN] = "Black queen";
	names[WHITE_KING] = "White king";
	names[BLACK_KING] = "Black king";
	return names;
}

inline constexpr auto g_cPieceNamesVerbose = initLongPieceNames();

// Compile-time move and mask tables. The generators follow The Beowulf Project's.

// Set mask for each square
constexpr std::array<BITBOARD, ALL_SQUARES> makeMask()
{
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = (1ULL << i); // UNIT << i
	}
	return result;
}

// Squares on the same file, towards rank 8 (Up) or rank 1 (Down).
constexpr std::array<BITBOARD, ALL_SQUARES> makeFileUpMask()
{
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = 0;
		for (int j = i - 8; j >= 0; j -= 8) {
			result[i] += (1ULL << j); // UNIT << j
		}
	}
	return result;
}

constexpr std::array<BITBOARD, ALL_SQUARES> makeFileDownMask()
{
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = 0;
		for (int j = i + 8; j < ALL_SQUARES; j += 8) {
			result[i] += (1ULL << j); // UNIT << j
		}
	}
	return result;
}

// Passed-pawn span: the pawn's own file plus both adjacent files, on every rank
// AHEAD of the square. A pawn is passed when no enemy pawn stands anywhere in
// this span.
//
// Square 0 is a8 and 63 is h1, so "ahead" for White means DECREASING index and
// for Black increasing -- the same direction convention as the FileUp/FileDown
// masks above. The file bounds are tested explicitly rather than by shifting a
// file mask, which is what keeps an a-file pawn's span off the h-file: a shift
// wraps around the board edge, and that wraparound is the classic bug in this
// kind of generator.
//
// A pawn on the promotion rank has an empty span, which is correct rather than
// degenerate: there is nothing ahead of it.
constexpr std::array<BITBOARD, ALL_SQUARES> makePassedMask(bool forWhite)
{
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (int i = 0; i < ALL_SQUARES; ++i) {
		const int file = i % 8;
		const int row = i / 8; // 0 = rank 8, 7 = rank 1
		BITBOARD span = 0;
		const int step = forWhite ? -1 : 1;
		for (int r = row + step; r >= 0 && r < 8; r += step) {
			for (int df = -1; df <= 1; ++df) {
				const int f = file + df;
				if (f < 0 || f > 7)
					continue;
				span |= (1ULL << (r * 8 + f));
			}
		}
		result[i] = span;
	}
	return result;
}

// All initialised at compile time.
inline constexpr auto g_bbMask = makeMask();
inline constexpr auto g_bbFileUpMask = makeFileUpMask();
inline constexpr auto g_bbFileDownMask = makeFileDownMask();
inline constexpr auto g_bbPassedMaskWhite = makePassedMask(true);
inline constexpr auto g_bbPassedMaskBlack = makePassedMask(false);

// ============= Knight Moves =============
inline constexpr std::array<std::array<int, 2>, 8> knightOffsets{
    {{{2, 1}}, {{1, 2}}, {{-1, 2}}, {{-2, 1}}, {{-2, -1}}, {{-1, -2}}, {{1, -2}}, {{2, -1}}}};

// Knight destinations from each square, indexed by the knight's square.
constexpr std::array<BITBOARD, ALL_SQUARES> makeKnightMoves()
{
	std::array<BITBOARD, ALL_SQUARES> result{};

	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		const auto rank = static_cast<int>(Rank(i));
		const auto file = static_cast<int>(File(i));
		BITBOARD moves = 0;
		for (const auto& offset : knightOffsets) {
			const int r = rank + offset[0];
			const int f = file + offset[1];
			if (r >= 0 && r < 8 && f >= 0 && f < 8)
				moves |= (UNIT << static_cast<unsigned int>(r * 8 + f));
		}
		result[i] = moves;
	}
	return result;
}

// ============= King Moves =============
constexpr std::array<BITBOARD, ALL_SQUARES> makeKingMoves()
{
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = 0;
		if (Rank(i) > 0) {
			if (File(i) > 0)
				result[i] += (UNIT << (i - 9));
			if (File(i) < 7)
				result[i] += (UNIT << (i - 7));
			result[i] += (UNIT << (i - 8));
		}
		if (Rank(i) < 7) {
			if (File(i) > 0)
				result[i] += (UNIT << (i + 7));
			if (File(i) < 7)
				result[i] += (UNIT << (i + 9));
			result[i] += (UNIT << (i + 8));
		}
		if (File(i) > 0)
			result[i] += (UNIT << (i - 1));
		if (File(i) < 7)
			result[i] += (UNIT << (i + 1));
	}
	return result;
}

// ============= Inline Global Variables =============
inline constexpr auto g_bbKnightMoves = makeKnightMoves();
inline constexpr auto g_bbKingMoves = makeKingMoves();
