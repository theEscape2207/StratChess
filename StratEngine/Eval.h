#pragma once

#include "PieceHelper.h"
#include <memory>
#include "defines.h"

class Board;

// Lazy SMP sharing contract: EvalManager and its
// derived classes (EvalSimple, EvalComplex) hold no mutable state of any
// kind — no data members beyond compile-time-constant enums/statics, and
// Evaluate() is `const`, reading only its `const Board&` argument plus the
// read-only global tables in defines.h (g_Eval_Bitboards, g_bbFileMask,
// g_bbFileUpMask, g_bbFileDownMask — all `constexpr`/compile-time-initialized,
// no lazy/runtime init). A single EvalManager instance is therefore safe to
// share, unsynchronized, across every Lazy SMP helper thread's concurrent
// Evaluate() calls — no per-thread clone is needed.
class EvalManager
{
public:

	enum class EvalTypes {
		NONE,			// No eval engine, i.e. human
		SIMPLE,			// Simple engine
		COMPLEX,		// Complex engine
	};
	enum class PlayState { MIDDLEGAME, ENDGAME, FINALGAME };

	virtual int Evaluate(const Board& board) const = 0;
	virtual const char* GetType() const = 0;
	// Factory constructor!
	static std::unique_ptr<EvalManager> Create(EvalTypes type);

	EvalManager() = default;
	virtual ~EvalManager() = default;
	EvalManager(const EvalManager&) = delete;

	EvalManager& operator=(const EvalManager&) = delete;
	EvalManager(EvalManager&&) = delete;
	EvalManager& operator=(EvalManager&&) = delete;

protected:
	static inline int GetPositionalScore(eSquare squareType, ePiece piece) noexcept
	{
		return g_Eval_Bitboards[piece >> 1][getEvalBoard(piece, squareType)];
	}
	// Maps a piece's board square to its PST lookup index. White uses the
	// square directly; Black needs a vertical mirror (rank r <-> rank 9 - r,
	// file unchanged) since every PST in defines.h is written from White's
	// point of view. `square ^ 56` is that vertical flip: given the board
	// layout in defines.h (a8 = 0 ... h1 = 63), XOR-ing with 56 (0b111000)
	// flips the three rank bits and leaves the three file bits untouched.
	//
	// The previous implementation used `63 - square` (equivalently
	// `square ^ 63`), a 180-degree rotation: it flips the file bits too, not
	// just the rank bits. That distinction is invisible for a file-symmetric
	// PST (rotation and vertical flip agree there) but wrong the moment a
	// table is file-asymmetric. See the plan file
	// `.claude/plans/eval-color-symmetry-and-queen-pst-fix.md` for the queen-PST
	// regression this caused and the test coverage added to guard it
	// (`StratChessTests/EvalTests.cpp`).
	static constexpr inline int getEvalBoard(ePiece piece, eSquare square) noexcept
	{
		return (PieceHelper::Color(piece) == eColor::BLACK) ? (square ^ 56) : square;
	}
};

class EvalSimple final
	: public EvalManager
{
public:
	int Evaluate(const Board& board) const noexcept override;
	const char* GetType() const noexcept override		{ return "Simple";	}

	// Force use of factory by
	// preventing constructor, copy-construction & operator=
	EvalSimple(const EvalSimple&) = delete;
	EvalSimple& operator=(const EvalSimple&) = delete;

	// Not to be used directly - only needed for make_unique
	EvalSimple() = default;
};

class EvalComplex final
	: public EvalManager
{
	// Bonuses and penalties for eval
	static const short DOUBLED_PAWN_PENALTY		= 10;
	static const short ISOLATED_PAWN_PENALTY	= 20;
	static const short BACKWARDS_PAWN_PENALTY	= 5;
	static const short PASSED_PAWN_BONUS		= 20;
	static const short ROOK_ON_7TH_BONUS		= 20;
	static const short HALF_OPEN_FILE			= 10;
	static const short OPEN_FILE				= 15;

	// Mop-up evaluation (won pawnless endgames) — see issue #70 / epic #110.
	// Gated on: pawnless + decisive material lead. Rewards pushing the losing
	// king to the edge/corner and closing the distance between the two kings.
	static const short MOPUP_MATERIAL_THRESHOLD	= 400;	// min material lead (cp) before mop-up applies
	static const short MOPUP_CMD_WEIGHT		= 10;	// weight on losing king's center-manhattan-distance
	static const short MOPUP_KINGDIST_WEIGHT	= 4;	// weight on (MOPUP_MAX_KING_DISTANCE - king-to-king distance)
	static const short MOPUP_MAX_KING_DISTANCE	= 7;	// max Chebyshev distance on an 8x8 board

	// Distance helpers for mop-up scoring — plain grid math, orientation-independent
	// (works the same whether the square belongs to White or Black).
	static constexpr int CenterAxisDistance(int coord) noexcept
	{
		return (coord <= 3) ? (3 - coord) : (coord - 4);
	}
	static constexpr int CenterManhattanDistance(eSquare square) noexcept
	{
		return CenterAxisDistance(File(square)) + CenterAxisDistance(Rank(square));
	}
	static constexpr int AbsDiff(int a, int b) noexcept
	{
		return (a > b) ? (a - b) : (b - a);
	}
	static constexpr int KingDistance(eSquare a, eSquare b) noexcept
	{
		const int fileDiff = AbsDiff(File(a), File(b));
		const int rankDiff = AbsDiff(Rank(a), Rank(b));
		return (fileDiff > rankDiff) ? fileDiff : rankDiff;
	}

public:
	int Evaluate(const Board& board) const noexcept override;
	const char* GetType() const	noexcept override	{ return "Complex";	}

	// Force use of factory by
	// preventing constructor, copy-construction & operator=
	EvalComplex(const EvalComplex&) = delete;
	EvalComplex& operator=(const EvalComplex&) = delete;
	EvalComplex& operator=(const EvalComplex&&) = delete;

	// NOT to be used directly - only to allow make_unique to access the Factory creator
	EvalComplex() = default;
};
