#pragma once

#include "PieceHelper.h"
#include <cstdint>
#include <memory>
#include <span>
#include "defines.h"

class Board;

// Tapered evaluation (issue #99 — see .claude/plans/tapered-evaluation.md).
//
// Game phase is a single integer in [0, MAX_GAME_PHASE] computed from the
// non-king, non-pawn material on the board: MAX_GAME_PHASE with a full set of
// pieces, 0 in a bare-pawn ending. Phase-sensitive terms produce a ScorePair
// instead of one number and are interpolated between the two endpoints by
// Evaluate(), which blends each term and sums the results (see the comment
// there for why per-term rather than once over the accumulated pair).
//
// Deliberately piece-COUNT based, not material-sum based: a material sum
// conflates "few pieces left" with "one side is winning", which is the same
// confusion as the pre-#99 `min(material) <= 11500` stage threshold it
// replaces. Phase is a property of the position, not of who is ahead.
inline constexpr int MAX_GAME_PHASE = 24;

// A phase-dependent score: `mg` applies at MAX_GAME_PHASE, `eg` at phase 0.
// A term with no phase sensitivity sets both to the same value, which makes it
// invariant under the blend.
struct ScorePair {
	int mg = 0;
	int eg = 0;

	constexpr ScorePair& operator+=(const ScorePair& rhs) noexcept
	{
		mg += rhs.mg;
		eg += rhs.eg;
		return *this;
	}
};

// Interpolate a ScorePair at the given phase. Exact at both endpoints:
// phase == MAX_GAME_PHASE yields mg, phase == 0 yields eg — an off-by-one here
// is the classic tapering bug, so both endpoints are asserted in EvalTests.
//
// Truncation toward zero is odd-symmetric ((-n)/d == -(n/d)), so it introduces
// no sign-dependent bias and does not by itself threaten the #125 mirror
// property — blending the white-minus-black difference would preserve that
// too. Per-color blending is chosen for a different reason: it is what makes
// the per-term rows #129 prints the literal addends of the score, see
// Evaluate().
constexpr int BlendPhase(const ScorePair& s, int phase) noexcept
{
	return (s.mg * phase + s.eg * (MAX_GAME_PHASE - phase)) / MAX_GAME_PHASE;
}

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
class EvalManager {
  public:
	// Never a stored field.
	// NOLINTNEXTLINE(performance-enum-size)
	enum class EvalTypes {
		NONE,    // No eval engine, i.e. human
		SIMPLE,  // Simple engine
		COMPLEX, // Complex engine
	};

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

class EvalSimple final : public EvalManager {
  public:
	int Evaluate(const Board& board) const noexcept override;
	const char* GetType() const noexcept override { return "Simple"; }

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
struct EvalContext {
	std::span<const BITBOARD> boards; // Board::GetBitBoards(), indexed by ePiece
	BITBOARD all_pieces;              // boards[ALL_PIECES]
	BITBOARD pawns[NUM_COLORS];       // boards[WHITE_PAWN] / boards[BLACK_PAWN]
	// boards[ALL_WHITE_PIECES] / boards[ALL_BLACK_PIECES]. Read by eval_mobility
	// to mask off squares occupied by the side's own pieces (issue #98).
	BITBOARD occupied[NUM_COLORS];
	// Squares each color's pawns attack. Derived here rather than read from
	// Board, using the same file-masked shifts MoveGenerator::GeneratePawnCaptures
	// uses, so the two cannot disagree about what an edge-file pawn covers.
	// eval_mobility subtracts the ENEMY set: a square an enemy pawn guards is not
	// one a piece can usefully occupy. Issue #116 (backwards pawns) will want
	// these too, which is why they live in the context rather than in one term.
	BITBOARD pawn_attacks[NUM_COLORS];
	// NO_SQUARE for a color with no king on the board. That only happens for a
	// default-constructed or failed-parse Board — MoveGenerator.cpp asserts
	// both kings exist before any search runs, so every position the search
	// evaluates has both. Consumers must check for NO_SQUARE before using this
	// square: Board::GetFirstPiece's assert(mask != 0) precondition is a
	// Release no-op, so calling it on an empty king bitboard silently reads
	// past the end of g_Eval_Bitboards rather than trapping. See eval_pst and
	// eval_mopup, and the kingless-board regression test in EvalTests.cpp.
	eSquare king_sq[NUM_COLORS];
	// Board::GetMaterialScore(color) — includes the king at 10000 cp
	// (g_iPieceValues, defines.h). That inclusion cancels in Evaluate()'s
	// final white-minus-black difference, so it is left as-is rather than
	// "fixed" here.
	int material[NUM_COLORS];
	// Game phase in [0, MAX_GAME_PHASE] from non-king, non-pawn piece counts
	// (issue #99). Replaced the old MIDDLEGAME/ENDGAME `stage`, which keyed on
	// min(material) <= 11500 — a threshold that was king-value-inclusive (hence
	// the otherwise inexplicable 11500 = 10000 + 1500) and took min() over both
	// sides, so a player still holding a queen switched to endgame king scoring
	// as soon as its OPPONENT was stripped down.
	int phase;
	// True for the side that is mopping up, false for both otherwise —
	// i.e. pawnless, decisive material lead, low phase, both kings present.
	// Computed once in BuildContext so eval_mopup and eval_pst cannot
	// disagree about whether a position is a mop-up: eval_pst suppresses the
	// winner's king PST exactly when eval_mopup is paying for king placement
	// (issue #118 item 4). Two readers of one gate, not two copies of it.
	bool mopup_active[NUM_COLORS];
	// GameInfo::castlingRights, the CastlingRights bit flags. This is a FEN
	// field, so reading it keeps Evaluate() a pure function of the position --
	// which whether a side has actually castled is not, since no FEN records it.
	uint8_t castling_rights;
};

// EvalBreakdown — per-term introspection output for the UCI 'eval' command
// (issue #129 phase 2 — see .claude/plans/uci-eval-command-term-breakdown.md).
// Produced by EvalComplex::Breakdown(); read-only, never consulted by search.
//
// Every field is indexed by eColor, so a caller can show which side a term is
// actually acting on rather than only the net effect — the per-color split is
// usually the thing being debugged. The net contribution of a term is always
// white-minus-black, matching how Evaluate() combines them.
struct EvalBreakdown {
	// Board::GetMaterialScore(color), copied verbatim from EvalContext — so
	// king-inclusive (10000 cp per side; see EvalContext::material above).
	// Left unadjusted deliberately: a king-stripped display figure would be a
	// number no part of the evaluator computes. It cancels in white-minus-black,
	// so a consumer showing net contributions never has to account for it. This
	// is the documented home for that fact — which is why the UCI 'eval' output
	// does not restate it on every call.
	int material[NUM_COLORS];
	int pawns[NUM_COLORS];    // eval_pawns
	int rooks[NUM_COLORS];    // eval_rooks (incl. connected rooks)
	int pst[NUM_COLORS];      // eval_pst
	int mopup[NUM_COLORS];    // eval_mopup
	int bishops[NUM_COLORS];  // eval_bishops
	int castling[NUM_COLORS]; // eval_castling
	int mobility[NUM_COLORS]; // eval_mobility
	// Included because it is not derivable from the rows: it sets where between
	// the mg and eg endpoints every tapered term landed, and gates eval_mopup.
	int phase;
	// Side-to-move-relative, exactly as Evaluate() returns it — this field is
	// Evaluate()'s return value, not a re-derivation of it (D8). Material plus
	// the four terms, summed white-minus-black, reproduces it up to the
	// side-to-move sign; that identity is asserted in StratChessTests.
	int total;
};

class EvalComplex final : public EvalManager {
	// Bonuses and penalties for eval
	static const short DOUBLED_PAWN_PENALTY = 10;
	static const short ISOLATED_PAWN_PENALTY = 20;
	// Backwards pawn (issue #116). BOTH clauses are required: the pawn is behind
	// every friendly pawn on its adjacent files, AND its stop square is attacked
	// by an enemy pawn without being defended by a friendly one. A pawn meeting
	// only one is not backwards -- "backwards pawn" has several incompatible
	// definitions in the literature and an unstated one cannot be tuned.
	//
	// Deliberately small. A mis-specified structural penalty is a rounding error
	// at 5 cp and a strategic distortion at 30.
	static const short BACKWARDS_PAWN_PENALTY = 5;
	// Passed pawn (issue #116): no enemy pawn on its own or either adjacent file
	// ahead of it (g_bbPassedMask*, defines.h). The base bonus is scaled by rank
	// and by phase -- see PASSED_PAWN_RANK_SCALE and the eg endpoint below.
	static const short PASSED_PAWN_BONUS = 20;
	// Passers are worth more as the endgame approaches: fewer pieces to blockade
	// or round them up, and the king can escort. The source TODO this term
	// replaces asked for exactly this phase dependence.
	static const short PASSED_PAWN_BONUS_EG = 45;
	static const short ROOK_ON_7TH_BONUS = 20;
	static const short HALF_OPEN_FILE = 10;
	static const short OPEN_FILE = 15;

	// Bishop pair (issue #111). Worth more as the board opens, hence the higher
	// endgame endpoint. Requires bishops on OPPOSITE square colours, not merely
	// two bishops -- the term exists because the pair covers both colours.
	static const short BISHOP_PAIR_BONUS_MG = 30;
	static const short BISHOP_PAIR_BONUS_EG = 45;

	// Connected rooks (issue #114): same rank or file with nothing between,
	// scored per connected pair. Halved in the endgame, where ROOK_ON_7TH_BONUS
	// already pays for the rook activity that matters most there.
	static const short CONNECTED_ROOKS_BONUS_MG = 15;
	static const short CONNECTED_ROOKS_BONUS_EG = 8;

	// Castling (issue #115). Middlegame-only: in an endgame the king belongs in
	// the centre, and the endgame king PST already says so -- a flat bonus here
	// would fight it. Derived from castling rights plus king placement, never
	// from move history; see .claude/plans/eval-bishop-pair-connected-rooks-castling.md D2.
	static const short CASTLING_DONE_BONUS = 25;
	static const short CASTLING_LOST_PENALTY = 20;

	// Mobility (issues #98, #113): value of one reachable square, per piece
	// type. Weighted per type because an extra square is worth much less to a
	// queen -- which already has many -- than to a knight, and phase-split
	// because a rook's mobility matters more once files open in the endgame.
	//
	// These are literature-standard magnitudes, deliberately NOT hand-tuned:
	// #117 (automated Texel-style tuning) owns the values, and this term is
	// unusually sensitive to them.
	//
	// The knight is worth MORE per square than the bishop, which looks backwards
	// until the counts are included: a bishop sees 7-13 squares to a knight's
	// 2-8, so equal per-square weights would hand the bishop roughly 2.5x the
	// total. g_iPieceValues rates both minors at 300, so that would be an
	// undeclared bishop premium stacking on BISHOP_PAIR_BONUS -- a material
	// change arriving as a side effect of a mobility weight.
	//
	// Mobility overlaps the PSTs, which already reward central placement. The
	// overlap is not marginal: a knight's mobility swing is comparable to its
	// entire PST range, so this roughly doubles the centralization gradient for
	// minors. That may be an improvement, but it is a real change in emphasis
	// rather than a small addition, and it is a #117 retuning input.
	// Per-rank multiplier for the passed-pawn bonus, in 1/16ths, indexed by how
	// far the pawn has advanced from its side's point of view: [1] is its
	// starting rank, [6] is one step from promotion. A passer on the 7th is worth
	// several times one on the 3rd, and the shape matters as much as the
	// magnitude. [0] and [7] are the back and promotion ranks -- unreachable for
	// a pawn, present so the table is indexable by any rank without a bounds test
	// in the hot path.
	//
	// Kept separate from PASSED_PAWN_BONUS rather than folded into it so issue
	// #117 can tune shape and magnitude independently.
	//
	// HALVED from the first measured version, which gave 20/45 cp on the starting
	// rank up to 80/180 at the 7th and measured **-11.52 +/- 4.36 Elo** over 19,980
	// games (run 31300861562). The shape was left alone; only the magnitude moved,
	// so that result and this one differ in one variable. Now roughly 10/22 cp on
	// the starting rank up to 40/90 at the 7th.
	static constexpr short PASSED_PAWN_RANK_SCALE[8] = {8, 8, 10, 14, 20, 28, 32, 32};

	// A passer whose stop square is occupied by an enemy piece is not running
	// anywhere: it has to be dislodged first, and the blockader is usually well
	// placed. Scored at this fraction (in 1/16ths) of the normal bonus. The first
	// measured version had no blockade awareness at all and paid a 7th-rank passer
	// its full value with the enemy king parked in front of it.
	static constexpr short PASSED_PAWN_BLOCKADED_SCALE = 8; // half

	static const short MOBILITY_KNIGHT_MG = 4;
	static const short MOBILITY_KNIGHT_EG = 4;
	static const short MOBILITY_BISHOP_MG = 3;
	static const short MOBILITY_BISHOP_EG = 3;
	static const short MOBILITY_ROOK_MG = 2;
	static const short MOBILITY_ROOK_EG = 4;
	static const short MOBILITY_QUEEN_MG = 1;
	static const short MOBILITY_QUEEN_EG = 2;

	// Square counts are measured against a typical count per piece type rather
	// than against zero, so the term is roughly zero-mean and a cramped piece is
	// penalised instead of merely under-rewarded.
	//
	// Without this the count is strictly positive, so every piece carries a
	// permanent bonus that only cancels while material is symmetric -- the term
	// would silently act as a piece-value adjustment across trades, and #117
	// would inherit mobility entangled with material rather than as an
	// independent positional term. Costs nothing at runtime.
	//
	// It also keeps mobility from overwhelming eval_mopup: with absolute counts
	// a KBNvK winner scored +47 of mobility against mop-up's 12, an unsuppressed
	// centralization pull four times the term meant to be steering. Relative
	// counts put it at -4. Same failure mode eval_pst had to solve for the
	// king PST (issue #118 item 4).
	static const short MOBILITY_BASE_KNIGHT = 4;
	static const short MOBILITY_BASE_BISHOP = 7;
	static const short MOBILITY_BASE_ROOK = 7;
	static const short MOBILITY_BASE_QUEEN = 14;

	// Mop-up evaluation (won pawnless endgames) — see issue #70 / epic #110.
	// Gated on: pawnless + decisive material lead. Rewards pushing the losing
	// king to the edge/corner and closing the distance between the two kings.
	static const short MOPUP_MATERIAL_THRESHOLD = 400; // min material lead (cp) before mop-up applies
	static const short MOPUP_CMD_WEIGHT = 10;          // weight on losing king's center-manhattan-distance
	static const short MOPUP_KINGDIST_WEIGHT = 4;      // weight on (MOPUP_MAX_KING_DISTANCE - king-to-king distance)
	static const short MOPUP_MAX_KING_DISTANCE = 7;    // max Chebyshev distance on an 8x8 board

	// Game-phase weights per piece (issue #99). Summed over BOTH colors, so a
	// full set of pieces gives 2*(2*1 + 2*1 + 2*2 + 1*4) = 24 = MAX_GAME_PHASE.
	// Pawns and kings contribute nothing: pawns are present throughout and
	// kings always, so neither carries information about how far the game has
	// progressed.
	static const short PHASE_KNIGHT = 1;
	static const short PHASE_BISHOP = 1;
	static const short PHASE_ROOK = 2;
	static const short PHASE_QUEEN = 4;

	// Mop-up stays a hard gate rather than a blended term (D4): it is a
	// special case for pawnless decisive endings, not a smoothly-scaling
	// positional idea, and fading it in at half strength mid-game would be
	// meaningless.
	//
	// Keyed on the LOSING side's phase, not the total. The retired stage gate
	// was min(material) <= 11500, i.e. a statement about the weaker side alone;
	// keying on total phase instead would let extra material on the WINNING
	// side switch mop-up off. That loses exactly the cases mop-up exists for:
	// KQQ vs K is total phase 8, and is reached by promoting a second queen —
	// so the engine would lose its mating guidance at the moment it queens.
	static const short MOPUP_MAX_LOSER_PHASE = 6; // gate: loser's own phase <= this

	// Distance helpers for mop-up scoring — plain grid math, orientation-independent
	// (works the same whether the square belongs to White or Black).
	static constexpr int CenterAxisDistance(int coord) noexcept { return (coord <= 3) ? (3 - coord) : (coord - 4); }
	static constexpr int CenterManhattanDistance(eSquare square) noexcept
	{
		return CenterAxisDistance(File(square)) + CenterAxisDistance(Rank(square));
	}
	static constexpr int AbsDiff(int a, int b) noexcept { return (a > b) ? (a - b) : (b - a); }
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
	static ScorePair eval_pawns(const EvalContext& ctx, eColor color) noexcept;
	static ScorePair eval_rooks(const EvalContext& ctx, eColor color) noexcept;
	static ScorePair eval_pst(const EvalContext& ctx, eColor color) noexcept;
	static ScorePair eval_mopup(const EvalContext& ctx, eColor color) noexcept;
	static ScorePair eval_bishops(const EvalContext& ctx, eColor color) noexcept;
	static ScorePair eval_castling(const EvalContext& ctx, eColor color) noexcept;
	static ScorePair eval_mobility(const EvalContext& ctx, eColor color) noexcept;

  public:
	int Evaluate(const Board& board) const noexcept override;
	const char* GetType() const noexcept override { return "Complex"; }

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
