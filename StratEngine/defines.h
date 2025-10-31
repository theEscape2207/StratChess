#pragma once

#include <array>

// Index til bitboards
inline constexpr auto ALL_PIECES = 14;
inline constexpr auto ROTATED90 = 15;
inline constexpr auto ROTATED45R = 16;
inline constexpr auto ROTATED45L = 17;

// Andre konstanter
//------------------
inline constexpr auto ALL_BITBOARDS = 18;
inline constexpr auto ALL_SQUARES = 64;
inline constexpr auto ALL_PIECETYPES = 12;

enum eColor { NO_COLOR = -1, WHITE = 0, BLACK = 1 };

enum ePieceType {
	NO_TYPE = -1, PAWN = 0, KNIGHT = 2, BISHOP = 4,
	ROOK = 6, QUEEN = 8, KING = 10, ALL_FROM_COLOR = 12
};

// enum ePiece benyttes til indexere de enkelte brikkers bitboards
enum ePiece {
	NO_PIECE = -1,
	WHITE_PAWN,
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
	ALL_BLACK_PIECES
}; // = 13

// Special moves
enum class MoveType {
	Normal = 0x0,
	Capture = 0x01,
	Promote = 0x02,
	PromoteCapture = 0x03,	// Both a Promote and a capture
	En_Passant = 0x04,
	PawnTwoForward = 0x08,
	Castling = 0x10
};

enum eRowNames {
	NO_ROW = -1, BLACK_BACK_ROW = 0, WHITE_7TH_ROW = 1,	// TODO: Confusing names
	BLACK_7TH_ROW = 6, WHITE_BACK_ROW = 7
};	// Cannot easily be converted to enum class

enum eFileNames { LEFT_FILE = 0, RIGHT_FILE = 7 };

// Feltbetegnelser
enum eSquare {
	NO_SQUARE = -1,
	a8 = 0, b8, c8, d8, e8, f8, g8, h8,
	a7, b7, c7, d7, e7, f7, g7, h7,
	a6, b6, c6, d6, e6, f6, g6, h6,
	a5, b5, c5, d5, e5, f5, g5, h5,
	a4, b4, c4, d4, e4, f4, g4, h4,
	a3, b3, c3, d3, e3, f3, g3, h3,
	a2, b2, c2, d2, e2, f2, g2, h2,
	a1, b1, c1, d1, e1, f1, g1, h1 = 63,
	NUM_SQUARES
};

// Bestemmer den maksimale soegedybde for Quiescent(?)
constexpr auto MAX_PLY = 32;

enum GameValues
{
	Draw = 0,								// 
	Mate = 30000,							// Arbitraer _hoej_ vaerdi for mat
	Search_Init = 50000,					// Start-vaerdier for alpha-beta soegningen
	Unknown_Hash = 65000					// Bruges i Transposition tables
};

// Typer
//
// BitBoard is a key data type.  It is a 64-bit value, in which each
// bit represents a square on the board as defined by "enum Square".
// Thus, bit position 0 represents a fact about square a1, and bit
// position 63 represents a fact about square h8.  For example, the
// bitboard representing "white rook" positions will have a bit set
// for every position occupied on the board by a white rook.
//
using BITBOARD = unsigned __int64;

// Helper
inline constexpr auto ONE_ROW = 8;
inline constexpr auto TWO_ROWS = 16;

// Makroer
inline constexpr BITBOARD UNIT = 1;	// 64 bit 1-tal
inline constexpr BITBOARD EMPTY = 0;	// 64 bit 0

#define File(x)		((x) & 7)
#define Rank(x)		((x) >> 3)

inline constexpr BITBOARD FIRST_TWO_RANKS_MASK = 0x000000000000ffff;
inline constexpr BITBOARD SECOND_TWO_RANKS_MASK = 0x00000000ffff0000U;
inline constexpr BITBOARD THIRD_TWO_RANKS_MASK = 0x0000ffff00000000;

inline constexpr BITBOARD MASK_RANK_1 = 0xff00000000000000U;
inline constexpr BITBOARD MASK_RANK_2 = 0x00ff000000000000;
inline constexpr BITBOARD MASK_RANK_3 = 0x0000ff0000000000;
inline constexpr BITBOARD MASK_RANK_4 = 0x000000ff00000000;
inline constexpr BITBOARD MASK_RANK_5 = 0x00000000ff000000U;
inline constexpr BITBOARD MASK_RANK_6 = 0x0000000000ff0000;
inline constexpr BITBOARD MASK_RANK_7 = 0x000000000000ff00;
inline constexpr BITBOARD MASK_RANK_8 = 0x00000000000000ff;

inline const BITBOARD g_bbFileMask[] =
{
	0x0101010101010101,
	0x0202020202020202,
	0x0404040404040404,
	0x0808080808080808,
	0x1010101010101010,
	0x2020202020202020,
	0x4040404040404040,
	0x8080808080808080U,
};


inline constexpr unsigned int g_iRotateR90[ALL_SQUARES] = {
	a1,a2,a3,a4,a5,a6,a7,a8,
	b1,b2,b3,b4,b5,b6,b7,b8,
	c1,c2,c3,c4,c5,c6,c7,c8,
	d1,d2,d3,d4,d5,d6,d7,d8,
	e1,e2,e3,e4,e5,e6,e7,e8,
	f1,f2,f3,f4,f5,f6,f7,f8,
	g1,g2,g3,g4,g5,g6,g7,g8,
	h1,h2,h3,h4,h5,h6,h7,h8
};



inline constexpr unsigned int g_iRotateR45[ALL_SQUARES] = {
	a8,
	a7,b8,
	a6,b7,c8,
	a5,b6,c7,d8,
	a4,b5,c6,d7,e8,
	a3,b4,c5,d6,e7,f8,
	a2,b3,c4,d5,e6,f7,g8,
	a1,b2,c3,d4,e5,f6,g7,h8,
	b1,c2,d3,e4,f5,g6,h7,
	c1,d2,e3,f4,g5,h6,
	d1,e2,f3,g4,h5,
	e1,f2,g3,h4,
	f1,g2,h3,
	g1,h2,
	h1
};


inline constexpr unsigned int g_iRotateL45[ALL_SQUARES] = {
	h8,
	g8,h7,
	f8,g7,h6,
	e8,f7,g6,h5,
	d8,e7,f6,g5,h4,
	c8,d7,e6,f5,g4,h3,
	b8,c7,d6,e5,f4,g3,h2,
	a8,b7,c6,d5,e4,f3,g2,h1,
	a7,b6,c5,d4,e3,f2,g1,
	a6,b5,c4,d3,e2,f1,
	a5,b4,c3,d2,e1,
	a4,b3,c2,d1,
	a3,b2,c1,
	a2,b1,
	a1
};

inline constexpr unsigned int g_iDiagonalLength_a1h8[ALL_SQUARES] = {
	1,2,3,4,5,6,7,8,
	2,3,4,5,6,7,8,7,
	3,4,5,6,7,8,7,6,
	4,5,6,7,8,7,6,5,
	5,6,7,8,7,6,5,4,
	6,7,8,7,6,5,4,3,
	7,8,7,6,5,4,3,2,
	8,7,6,5,4,3,2,1
};

inline constexpr unsigned int g_iDiagonalLength_a8h1[ALL_SQUARES] = {
	8,7,6,5,4,3,2,1,
	7,8,7,6,5,4,3,2,
	6,7,8,7,6,5,4,3,
	5,6,7,8,7,6,5,4,
	4,5,6,7,8,7,6,5,
	3,4,5,6,7,8,7,6,
	2,3,4,5,6,7,8,7,
	1,2,3,4,5,6,7,8
};

inline constexpr unsigned int g_iDiagonalShifts_a1h8[ALL_SQUARES] = {
	0, 1, 3, 6,10,15,21,28,
	1, 3, 6,10,15,21,28,36,
	3, 6,10,15,21,28,36,43,
	6,10,15,21,28,36,43,49,
	10,15,21,28,36,43,49,54,
	15,21,28,36,43,49,54,58,
	21,28,36,43,49,54,58,61,
	28,36,43,49,54,58,61,63
};

inline constexpr unsigned int g_iDiagonalShifts_a8h1[ALL_SQUARES] = {
	28,21,15,10, 6, 3, 1, 0,
	36,28,21,15,10, 6, 3, 1,
	43,36,28,21,15,10, 6, 3,
	49,43,36,28,21,15,10, 6,
	54,49,43,36,28,21,15,10,
	58,54,49,43,36,28,21,15,
	61,58,54,49,43,36,28,21,
	63,61,58,54,49,43,36,28
};

// Tabel med brikkernes statiske vaerdier
inline constexpr unsigned int g_iPieceValues[ALL_PIECETYPES >> 1] = {
	100, 		// Boender
	300, 		// Springere
	300, 		// Loebere
	500, 		// Taarne
	900, 		// Dronninger
	10000		// Konger
};


/*
constexpr short g_Eval_Bitboards[][ALL_SQUARES] =
{
{0,0,0,0,0,0,0,0,	//For WHITE_PAWN.
7,7,13,23,26,13,7,7,
-2,-2,4,13,15,4,-2,-2,
-3,-3,2,9,11,2,-3,-3,
-4,-4,0,6,8,0,-4,-4,
-4,-4,0,4,6,0,-4,-4,
-1,-1,1,5,6,1,-1,-1,
0,0,0,0,0,0,0,0},

{-2,2,7,9,9,7,2,-2,	//For WHITE_KNIGHT.
1,4,12,13,13,12,4,1,
5,11,18,19,19,18,11,5,
3,10,14,14,14,14,10,3,
0,5,8,9,9,8,5,0,
-3,1,3,4,4,3,1,-3,
-5,-3,-1,0,0,-1,-3,-5,
-7,-5,-4,-2,-2,-4,-5,-7},

{2,3,4,4,4,4,3,2,	//For WHITE_BISHOP.
4,7,7,7,7,7,7,4,
3,5,6,6,6,6,5,3,
3,5,7,7,7,7,5,3,
4,5,6,8,8,6,5,4,
4,5,5,-2,-2,5,5,4,
5,5,5,3,3,5,5,5,
0,0,0,0,0,0,0,0},

{9,9,11,10,11,9,9,9,	//For WHITE_ROOK.
4,6,7,9,9,7,6,4,
9,10,10,11,11,10,10,9,
8,8,8,9,9,8,8,8,
6,6,5,6,6,5,6,6,
4,5,5,5,5,5,5,4,
3,4,4,6,6,4,4,3,
0,0,0,0,0,0,0,0},

{2,3,4,3,4,3,3,2,		//For WHITE_QUEEN.
2,3,4,4,4,4,3,2,
3,4,4,4,4,4,4,3,
3,3,4,4,4,4,3,3,
2,3,3,4,4,3,3,2,
2,2,2,3,3,2,2,2,
2,2,2,2,2,2,2,2,
0,0,0,0,0,0,0,0},

{0,0,0,0,0,0,0,0,		//For WHITE_KING.
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,-2,0,0,0,
0,0,0,0,0,-2,0,0}
};
*/

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
		0,  2,  4,  5,  5,  3,  2,  0,
		0,  2,  4,  8,  8,  4,  2,  0,
		-1,  0,  3,  6,  6,  3,  0, -1,
		-1,  0,  2,  3,  3,  2,  0, -1,
		-2, -1,  0,  0,  0,  0, -1, -2,
		-3, -2, -1,  0,  0, -1, -2, -3 },

		/* King positional values - opening and middlegame*/

	{
		-40, -40, -40, -40, -40, -40, -40, -40,
		-40, -40, -40, -40, -40, -40, -40, -40,
		-40, -40, -40, -40, -40, -40, -40, -40,
		-40, -40, -40, -40, -40, -40, -40, -40,
		-40, -40, -40, -40, -40, -40, -40, -40,
		-40, -40, -40, -40, -40, -40, -40, -40,
		-20, -20, -20, -20, -20, -20, -20, -20,
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

// Setup short piece names
inline constexpr auto initPieceNames() {
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
inline constexpr auto initLongPieceNames() {
	std::array<const char*, ALL_PIECETYPES + 1> names{};
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

/*
----------------------------------------------------------
|                                                          |
|	Laver tabeller over mulige traek - cool                 |
|	Algoritmerne til udregning af disse tabeller er        |
|   laant fra The Beowulf Project                           |
|														   |
----------------------------------------------------------
*/

// Hjaelpefunktioner til at generere hver bitboard-array
// 
// Set mask for each square
constexpr std::array<BITBOARD, ALL_SQUARES> makeMask() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = (1ULL << i);  // UNIT << i
	}
	return result;
}

// Maske for de roterede bitboards
constexpr std::array<BITBOARD, ALL_SQUARES> makeMaskRotated90() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	auto mask = makeMask();
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[g_iRotateR90[i]] = mask[i];
	}
	return result;
}

constexpr std::array<BITBOARD, ALL_SQUARES> makeMaskRotated45R() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	auto mask = makeMask();
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[g_iRotateR45[i]] = mask[i];
	}
	return result;
}

constexpr std::array<BITBOARD, ALL_SQUARES> makeMaskRotated45L() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	auto mask = makeMask();
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[g_iRotateL45[i]] = mask[i];
	}
	return result;
}

// Diagonale masker for loebere og dronning
constexpr std::array<BITBOARD, ALL_SQUARES> makeDiagonalMask_a1h8() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = (1ULL << g_iDiagonalLength_a1h8[i]) - 1;
	}
	return result;
}

constexpr std::array<BITBOARD, ALL_SQUARES> makeDiagonalMask_a8h1() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = (1ULL << g_iDiagonalLength_a8h1[i]) - 1;
	}
	return result;
}

// Masker for raekker og kolonner
constexpr std::array<BITBOARD, ALL_SQUARES> makeFileUpMask() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = 0;
		for (int j = i - 8; j >= 0; j -= 8) {
			result[i] += (1ULL << j);  // UNIT << j
		}
	}
	return result;
}

constexpr std::array<BITBOARD, ALL_SQUARES> makeFileDownMask() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = 0;
		for (int j = i + 8; j < ALL_SQUARES; j += 8) {
			result[i] += (1ULL << j);  // UNIT << j
		}
	}
	return result;
}

// Inline constexpr arrays initialiseres ved kompileringstid
inline constexpr auto g_bbMask = makeMask();
inline constexpr auto g_bbMaskRotated90 = makeMaskRotated90();
inline constexpr auto g_bbMaskRotated45R = makeMaskRotated45R();
inline constexpr auto g_bbMaskRotated45L = makeMaskRotated45L();
inline constexpr auto g_bbDiagonalMask_a1h8 = makeDiagonalMask_a1h8();
inline constexpr auto g_bbDiagonalMask_a8h1 = makeDiagonalMask_a8h1();
inline constexpr auto g_bbFileUpMask = makeFileUpMask();
inline constexpr auto g_bbFileDownMask = makeFileDownMask();

// Type alias for 2D array: [ALL_SQUARES][256]
template <typename T, std::size_t Rows, std::size_t Cols>
using Array2D = std::array<std::array<T, Cols>, Rows>;

// ============= Knight Moves =============
constexpr std::array<BITBOARD, ALL_SQUARES> makeKnightMoves() {
	std::array<BITBOARD, ALL_SQUARES> result{};
	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		result[i] = 0;
		if (Rank(i) > 0) {
			if (Rank(i) > 1) {
				if (File(i) > 0)
					result[i] += (UNIT << (i - 17));
				if (File(i) < 7)
					result[i] += (UNIT << (i - 15));
			}
			if (File(i) > 1)
				result[i] += (UNIT << (i - 10));
			if (File(i) < 6)
				result[i] += (UNIT << (i - 6));
		}
		if (Rank(i) < 7) {
			if (Rank(i) < 6) {
				if (File(i) > 0)
					result[i] += (UNIT << (i + 15));
				if (File(i) < 7)
					result[i] += (UNIT << (i + 17));
			}
			if (File(i) > 1)
				result[i] += (UNIT << (i + 6));
			if (File(i) < 6)
				result[i] += (UNIT << (i + 10));
		}
	}
	return result;
}

// ============= King Moves =============
constexpr std::array<BITBOARD, ALL_SQUARES> makeKingMoves() {
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

// ============= Rank Moves (Horizontal) =============
constexpr Array2D<BITBOARD, ALL_SQUARES, 256> makeMovesRank() {
	Array2D<BITBOARD, ALL_SQUARES, 256> result{};

	for (unsigned int iFile = 0; iFile < 8; ++iFile) {
		for (int j = 0; j < 256; ++j) {
			BITBOARD bbMask = 0;

			for (int x = iFile - 1; x >= 0; --x) {
				bbMask += (UNIT << x);
				if (j & (1 << x))
					break;
			}
			for (int x = iFile + 1; x < 8; ++x) {
				bbMask += (UNIT << x);
				if (j & (1 << x))
					break;
			}

			for (unsigned int iRank = 0; iRank < 8; ++iRank) {
				result[(iRank << 3) + iFile][j] = bbMask << (iRank << 3);
			}
		}
	}
	return result;
}

// ============= File Moves (Vertical) =============
constexpr Array2D<BITBOARD, ALL_SQUARES, 256> makeMovesFile() {
	Array2D<BITBOARD, ALL_SQUARES, 256> result{};

	for (unsigned int iRank = 0; iRank < 8; ++iRank) {
		for (int j = 0; j < 256; ++j) {
			BITBOARD bbMask = 0;

			for (int x = 6 - iRank; x >= 0; --x) {
				bbMask += (UNIT << ((7 - x) << 3));
				if (j & (1 << x))
					break;
			}
			for (int x = 8 - iRank; x < 8; ++x) {
				bbMask += (UNIT << ((7 - x) << 3));
				if (j & (1 << x))
					break;
			}

			for (unsigned int iFile = 0; iFile < 8; ++iFile) {
				result[(iRank << 3) + iFile][j] = bbMask << iFile;
			}
		}
	}
	return result;
}

// ============= Diagonal a1-h8 Moves =============
constexpr Array2D<BITBOARD, ALL_SQUARES, 256> makeMovesa1h8() {
	Array2D<BITBOARD, ALL_SQUARES, 256> result{};

	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		unsigned int iDiagonalStart = 7 * (std::min((File(i)), 7 - (Rank(i)))) + i;
		unsigned int iDiagonalStartFile = File(iDiagonalStart);
		unsigned int iDiagonalLength = g_iDiagonalLength_a1h8[i];
		unsigned int iFile = File(i);

		// Skip invalid diagonals
		if (iDiagonalLength < 1 || iDiagonalLength > 8)
			continue;

		for (int j = 0; j < (1 << iDiagonalLength); ++j) {
			BITBOARD bbMask = 0, bbMask2 = 0;

			for (int x = (iFile - iDiagonalStartFile) - 1; x >= 0; --x) {
				bbMask += (UNIT << x);
				if (j & (1 << x))
					break;
			}
			for (unsigned int x = (iFile - iDiagonalStartFile) + 1; x < iDiagonalLength; ++x) {
				bbMask += (UNIT << x);
				if (j & (1 << x))
					break;
			}

			for (unsigned int x = 0; x < iDiagonalLength; ++x) {
				bbMask2 += (((bbMask >> x) & 1) << (iDiagonalStart - (7 * x)));
			}

			result[i][j] = bbMask2;
		}
	}
	return result;
}

// ============= Diagonal a8-h1 Moves =============
constexpr Array2D<BITBOARD, ALL_SQUARES, 256> makeMovesa8h1() {
	Array2D<BITBOARD, ALL_SQUARES, 256> result{};

	for (unsigned int i = 0; i < ALL_SQUARES; ++i) {
		unsigned int iDiagonalStart = i - 9 * (std::min((File(i)), (Rank(i))));
		unsigned int iDiagonalStartFile = File(iDiagonalStart);
		unsigned int iDiagonalLength = g_iDiagonalLength_a8h1[i];
		unsigned int iFile = File(i);

		// Skip invalid diagonals
		if (iDiagonalLength < 1 || iDiagonalLength > 8)
			continue;

		for (int j = 0; j < (1 << iDiagonalLength); ++j) {
			BITBOARD bbMask = 0, bbMask2 = 0;

			for (int x = (iFile - iDiagonalStartFile) - 1; x >= 0; --x) {
				bbMask += (UNIT << x);
				if (j & (1 << x))
					break;
			}
			for (unsigned int x = (iFile - iDiagonalStartFile) + 1; x < iDiagonalLength; ++x) {
				bbMask += (UNIT << x);
				if (j & (1 << x))
					break;
			}

			for (unsigned int x = 0; x < iDiagonalLength; ++x) {
				bbMask2 += (((bbMask >> x) & 1) << (iDiagonalStart + (9 * x)));
			}

			result[i][j] = bbMask2;
		}
	}
	return result;
}

// ============= Inline Global Variables =============
inline constexpr auto g_bbKnightMoves = makeKnightMoves();
inline constexpr auto g_bbKingMoves = makeKingMoves();
inline constexpr auto g_bbMovesRank = makeMovesRank();
inline constexpr auto g_bbMovesFile = makeMovesFile();
inline constexpr auto g_bbMovesa1h8 = makeMovesa1h8();
inline constexpr auto g_bbMovesa8h1 = makeMovesa8h1();

#define PRINT_STATS							1;

#ifdef PRINT_STATS
#define PRINT_MOVES						1;
#endif
