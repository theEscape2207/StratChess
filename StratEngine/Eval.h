#pragma once

#include "PieceHelper.h"
#include <memory>
#include <span>
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
// Evaluate() calls — no per-thread clone is needed. EvalContext below (used
// by EvalComplex) does not change this: it is always a per-call stack local,
// never a member of EvalManager or EvalComplex. The same holds for
// EvalComplex::Breakdown() and its EvalBreakdown result (issue #129 phase 2):
// also `const`, also per-call stack locals — though it is a debug path that no
// search thread calls.
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

// EvalContext — shared, per-call intermediates for EvalComplex::Evaluate().
// Built once by EvalComplex::BuildContext() as a plain stack local and passed
// by const reference into each term function; nothing in it is ever stored on
// EvalManager/EvalComplex (see the Lazy SMP sharing-contract comment above).
// Everything here is already computed or trivially available from Board — this
// names shared work, it does not add any. See
// `.claude/plans/eval-context-restructure.md` for the restructure this supports.
struct EvalContext
{
	std::span<const BITBOARD>  boards;             // Board::GetBitBoards(), indexed by ePiece
	BITBOARD                   all_pieces;          // boards[ALL_PIECES]
	BITBOARD                   pawns[NUM_COLORS];   // boards[WHITE_PAWN] / boards[BLACK_PAWN]
	// boards[ALL_WHITE_PIECES] / boards[ALL_BLACK_PIECES]. Unread by any term
	// today (same for all_pieces above) — both are populated because issue #98
	// (Mobility) needs per-color occupancy to mask off blocked squares. Do not
	// remove as dead code; see D3 in eval-context-restructure.md.
	BITBOARD                   occupied[NUM_COLORS];
	// NO_SQUARE for a color with no king on the board. That only happens for a
	// default-constructed or failed-parse Board — MoveGenerator.cpp asserts
	// both kings exist before any search runs, so every position the search
	// evaluates has both. Consumers must check for NO_SQUARE before using this
	// square: Board::GetFirstPiece's assert(mask != 0) precondition is a
	// Release no-op, so calling it on an empty king bitboard silently reads
	// past the end of g_Eval_Bitboards rather than trapping. See eval_pst and
	// eval_mopup, and the kingless-board regression test in EvalTests.cpp.
	eSquare                    king_sq[NUM_COLORS];
	// Board::GetMaterialScore(color) — includes the king at 10000 cp
	// (g_iPieceValues, defines.h). That inclusion cancels in Evaluate()'s
	// final white-minus-black difference, so it is left as-is rather than
	// "fixed" here.
	int                        material[NUM_COLORS];
	// MIDDLEGAME vs ENDGAME, gated on min(material[WHITE], material[BLACK])
	// <= 11500. That threshold is king-value-inclusive (a king alone is 10000
	// of it) and takes the min() over both sides rather than each side's own
	// material — both are known-imprecise (issue #99) and deliberately left
	// unchanged here: fixing either would move scores, which is out of scope
	// for a pure restructure (D4, eval-context-restructure.md).
	EvalManager::PlayState     stage;
};

// EvalBreakdown — per-term introspection output for the UCI 'eval' command
// (issue #129 phase 2 — see .claude/plans/uci-eval-command-term-breakdown.md).
// Produced by EvalComplex::Breakdown(); read-only, never consulted by search.
//
// Every field is indexed by eColor, so a caller can show which side a term is
// actually acting on rather than only the net effect — the per-color split is
// usually the thing being debugged. The net contribution of a term is always
// white-minus-black, matching how Evaluate() combines them.
struct EvalBreakdown
{
	// Board::GetMaterialScore(color), copied verbatim from EvalContext — so
	// king-inclusive (10000 cp per side; see EvalContext::material above).
	// Left unadjusted deliberately: a king-stripped display figure would be a
	// number no part of the evaluator computes. It cancels in white-minus-black.
	int                    material[NUM_COLORS];
	int                    pawns[NUM_COLORS];    // eval_pawns
	int                    rooks[NUM_COLORS];    // eval_rooks
	int                    pst[NUM_COLORS];      // eval_pst
	int                    mopup[NUM_COLORS];    // eval_mopup
	// Included because it is not derivable from the rows: it selects which
	// king PST eval_pst reads and gates eval_mopup entirely.
	EvalManager::PlayState stage;
	// Side-to-move-relative, exactly as Evaluate() returns it — this field is
	// Evaluate()'s return value, not a re-derivation of it (D8). Material plus
	// the four terms, summed white-minus-black, reproduces it up to the
	// side-to-move sign; that identity is asserted in StratChessTests.
	int                    total;
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

	// Builds the EvalContext Evaluate() and every term function read from —
	// the one construction site, so phase detection and the rest of the
	// context's fields can't drift out of sync between production and the
	// term-level test fixture (StratChessTests/EvalTests.cpp) that also calls
	// this. See .claude/plans/eval-context-restructure.md.
	static EvalContext BuildContext(const Board& board) noexcept;

	// Per-term evaluation functions (issue #127 restructure). Each returns
	// only the named term's contribution for one color; Evaluate() sums the
	// terms itself, so no term touches a shared accumulator and no term reads
	// state another term writes — each is a pure function of EvalContext.
	// Plain int addition, so summation order cannot change the result.
	static int eval_pawns(const EvalContext& ctx, eColor color) noexcept;
	static int eval_rooks(const EvalContext& ctx, eColor color) noexcept;
	static int eval_pst(const EvalContext& ctx, eColor color) noexcept;
	static int eval_mopup(const EvalContext& ctx, eColor color) noexcept;

public:
	int Evaluate(const Board& board) const noexcept override;
	const char* GetType() const	noexcept override	{ return "Complex";	}

	// Per-term introspection for the UCI 'eval' command (issue #129 phase 2 —
	// see .claude/plans/uci-eval-command-term-breakdown.md). Reports what the
	// four private term functions above contribute, per color, for one
	// position; changes nothing and is never called from search.
	//
	// This is the only member made public for the breakdown: BuildContext and
	// the eval_* functions stay private, and EvalManager's abstract interface
	// is left alone rather than growing a debug method EvalSimple could never
	// implement (D7).
	//
	// The rows come from the same BuildContext + eval_* calls Evaluate() makes
	// — never a parallel computation — and `total` is Evaluate()'s own return
	// value rather than a restatement of its side-to-move sign flip (D8). That
	// costs a second BuildContext per call, which is free on a path invoked
	// once per interactive command.
	EvalBreakdown Breakdown(const Board& board) const noexcept;

	// Force use of factory by
	// preventing constructor, copy-construction & operator=
	EvalComplex(const EvalComplex&) = delete;
	EvalComplex& operator=(const EvalComplex&) = delete;
	EvalComplex& operator=(const EvalComplex&&) = delete;

	// NOT to be used directly - only to allow make_unique to access the Factory creator
	EvalComplex() = default;

#ifdef STRAT_ENABLE_TEST_ACCESS
	// Enables term-level unit tests (StratChessTests/EvalTests.cpp) to call
	// the private eval_* functions directly now that they exist as separately
	// callable units. Same mechanism as AIPerplex/UciHandler's test fixtures.
	// Activated only by StratChessTests.vcxproj's preprocessor definitions —
	// never in production.
	friend struct EvalComplexTestFixture;
#endif
};
