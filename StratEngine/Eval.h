#pragma once

#include "PieceHelper.h"
#include <cstdint>
#include <memory>
#include <span>
#include "defines.h"

class Board;

// Tapered evaluation.
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

// Denominator of the endgame material scale: a score is worth
// scale/ENDGAME_SCALE_MAX of what material and position say it is. A scale of
// ENDGAME_SCALE_MAX leaves the score alone, and 0 makes it GameValues::Draw.
// Fixed point rather than a float so evaluation stays integer-exact and
// reproducible across builds, and a power of two so the division is cheap.
inline constexpr int ENDGAME_SCALE_MAX = 16;

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
	// table is file-asymmetric — the queen-PST regression this caused is
	// guarded by test coverage in `StratChessTests/EvalTests.cpp`.
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

// Dense index for the per-piece-type aggregates in EvalContext below. The
// pieces that get their attacks generated, in the order the generation loop
// visits them. Deliberately not ePiece or ePieceType: those are sparse
// (PAWN = 0, KNIGHT = 2, ... KING = 10, defines.h) and carry no count
// constant, so an array indexed by them would need eleven entries to hold
// four values.
enum eMobilePiece : std::uint8_t {
	MOB_KNIGHT = 0,
	MOB_BISHOP,
	MOB_ROOK,
	MOB_QUEEN,
	NUM_MOBILE_PIECES,
};

// What one pass over the knights, bishops, rooks and queens reduces to.
// Produced by EvalComplex::ComputePieceAggregates() and carried in EvalContext
// below, so no term has to generate an attack set a previous one already had.
//
// Counts, never weighted scores: the term functions own the weights, which is
// what keeps them pure and lets issue #117 retune without touching
// BuildContext.
//
// ALL ZERO when endgame_scale is 0 — no attacks are generated at all for a
// dead-drawn material class, because Evaluate() returns GameValues::Draw for
// one without asking any term anything. Only Breakdown() can observe the
// difference, and only on a position whose total is Draw regardless.
struct PieceAggregates {
	// Per type, the sum over that color's pieces of (safe squares reached minus
	// MOBILITY_BASE_*) — exactly what eval_mobility multiplies by its per-type
	// weight. "Safe" is the mask eval_mobility documents: not our own pieces,
	// not covered by an enemy pawn.
	int mobility_count[NUM_COLORS][NUM_MOBILE_PIECES];
	// Pairs of that color's rooks that see each other along a rank or file with
	// nothing between (issue #114), counted in the same loop that generates the
	// rook attacks so eval_rooks does not regenerate them.
	int connected_rook_pairs[NUM_COLORS];
	// Per type, how many of that color's pieces attack at least one square of
	// the ENEMY king zone (EvalComplex::KingZone). One count per piece; the
	// squares themselves are zone_attacks below.
	int zone_attackers[NUM_COLORS][NUM_MOBILE_PIECES];
	// ENEMY king-zone squares this color attacks, counting OVERLAPS: a square
	// two pieces both hit contributes twice, which is the concentration signal
	// a union would throw away. Same four piece types as zone_attackers, so the
	// two aggregates always describe the same set of pieces -- pawns and the
	// king are in neither.
	int zone_attacks[NUM_COLORS];
};

// EvalContext — shared, per-call intermediates for EvalComplex::Evaluate().
// Built once by EvalComplex::BuildContext() as a plain stack local and passed
// by const reference into each term function; nothing in it is ever stored on
// EvalManager/EvalComplex (see the Lazy SMP sharing-contract comment above).
// Everything here is already computed or trivially available from Board — this
// names shared work, it does not add any.
struct EvalContext {
	std::span<const BITBOARD> boards; // Board::GetBitBoards(), indexed by ePiece
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
	// How much of the assembled score this position's material is worth, as a
	// numerator over ENDGAME_SCALE_MAX. Computed here for the same reason
	// mopup_active is: Evaluate() applies it and Breakdown() reports it, and a
	// breakdown reporting a scale the score was not computed with would be a
	// debugging tool that lies.
	int endgame_scale;
	// Board::castling_rights(), the CastlingRights bit flags. This is a FEN
	// field, so reading it keeps Evaluate() a pure function of the position --
	// which whether a side has actually castled is not, since no FEN records it.
	uint8_t castling_rights;

	// Reductions of the one non-pawn attack generation pass. All zero when
	// endgame_scale is 0 — see PieceAggregates.
	PieceAggregates attacks;
};

// EvalBreakdown — per-term introspection output for the UCI 'eval' command.
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
	int pawns[NUM_COLORS];        // eval_pawns
	int rooks[NUM_COLORS];        // eval_rooks (incl. connected rooks)
	int pst[NUM_COLORS];          // eval_pst
	int mopup[NUM_COLORS];        // eval_mopup
	int bishops[NUM_COLORS];      // eval_bishops
	int castling[NUM_COLORS];     // eval_castling
	int mobility[NUM_COLORS];     // eval_mobility
	int king_shelter[NUM_COLORS]; // eval_king_pawn_cover, shelter
	int king_storm[NUM_COLORS];   // eval_king_pawn_cover, storm
	int king_files[NUM_COLORS];   // eval_king_pawn_cover, file openness
	int king_attack[NUM_COLORS];  // eval_king_attack
	// Included because it is not derivable from the rows: it sets where between
	// the mg and eg endpoints every tapered term landed, and gates eval_mopup.
	int phase;
	// The endgame material scale that was applied, over ENDGAME_SCALE_MAX.
	int endgame_scale;
	// What that scale did to the score, white-POV: scaled minus unscaled, so a
	// consumer adds it to the summed rows and lands on `total`. Reported as one
	// net figure rather than spread across the per-color rows, which would make
	// every row a number no part of the evaluator computes.
	int endgame_adjustment;
	// Side-to-move-relative, exactly as Evaluate() returns it — this field is
	// Evaluate()'s return value, not a re-derivation of it (D8). Material plus
	// the four terms, summed white-minus-black, reproduces it up to the
	// side-to-move sign; that identity is asserted in StratChessTests.
	int total;
};

// Extrema of one row of a king-safety weight table. They exist so
// EvalComplex::KING_SAFETY_MAX_PENALTY can be derived from the tables at
// compile time instead of restated beside them: a retune that pushes a table
// past the declared bound then fails to build. Free functions rather than
// members because a static_assert inside the class body cannot call a member
// function of the class it is still defining. Templated on the row length so
// the same pair serves the 8-entry rank tables and the 4-entry attack weights.
template <std::size_t N> constexpr int EvalRowMin(const short (&row)[N]) noexcept
{
	int lowest = row[0];
	for (const short value : row)
		if (value < lowest)
			lowest = value;
	return lowest;
}

template <std::size_t N> constexpr int EvalRowMax(const short (&row)[N]) noexcept
{
	int highest = row[0];
	for (const short value : row)
		if (value > highest)
			highest = value;
	return highest;
}

// Everything one scan over the king's three files produces (issue #97).
// Separate ScorePairs rather than a sum: each is its own breakdown row and each
// is separately ablatable, because a regression in a mis-scaled shield table,
// one in an over-weighted storm table and one in an open-file penalty that
// double-counts shelter are corrected in completely different ways.
struct KingPawnCover {
	ScorePair shelter;
	ScorePair storm;
	ScorePair files;
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
	// from move history.
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

	// King shelter and pawn storm (issue #97). One scan over the king's own
	// file and its two neighbours produces both, so both are indexed the same
	// way:
	//
	//   [d] is the file's distance from the king's file, 0 or 1. Only two rows
	//       exist because only three files are scanned -- the king's own and
	//       one either side.
	//   [r] is the RELATIVE RANK of the nearest pawn on that file at or ahead
	//       of the king, from the DEFENDER's point of view: 1 is the defending
	//       side's back rank, 8 the enemy's. That is what lets one table serve
	//       both colours, and it is why the tables are not indexed by the
	//       pawn's distance from the king -- a pawn level with the king has
	//       distance 0, which would collide with the absence sentinel below,
	//       and "a pawn on its start square shelters best" stops being
	//       expressible the moment the king steps off its own back rank.
	//
	// INDEX 0 MEANS "NO SUCH PAWN ON THIS FILE", in both tables. It is
	// unambiguous because no pawn can stand on rank 1 or rank 8, so a real pawn
	// always indexes 2..7. Index [1] is the only unreachable entry -- [7] is a
	// real bucket (a White pawn on g7 in front of Kg1 reaches it), so a tuner
	// must not treat its zero as free padding.
	//
	// SHELTER is a bonus (a missing shield pawn is the negative entry at index
	// 0); STORM is a penalty magnitude, subtracted by the term. A storm pawn
	// directly blocked by our own shield pawn -- our nearest pawn on that file
	// standing at exactly r' - 1 -- is halved.
	//
	// Literature-standard shapes at half the usual magnitude, deliberately NOT
	// fitted here: issue #117 (Texel-style tuning) owns the values.
	//
	// What sets the level: shelter, king-file openness and ISOLATED_PAWN_PENALTY
	// all fire on one missing shield pawn without knowing about each other, so at
	// full magnitude that pawn was priced close to twice over. What sets the
	// uniformity: rescaling every table by the same factor keeps the intra-term
	// shape, including shelter-to-storm ratio. KING_STORM is NOT part of that
	// overlap -- a storm pawn is the enemy's, so the isolated penalty cannot see
	// it, and an enemy pawn on the file makes it half-open rather than open, which
	// cancels rather than compounds. Halving shelter alone would leave storm the
	// louder half, inverting the usual ordering. Odd entries round up, so the far
	// ranks are marginally louder than an exact halving.
	// clang-format off
	static constexpr short KING_SHELTER[2][8] = {
	    // r:   -   1    2    3    4   5   6   7
	    {      -9,  0,  15,  10,   5,  2,  0,  0}, // king's own file
	    {      -6,  0,  12,   8,   4,  2,  0,  0}, // adjacent files
	};
	static constexpr short KING_STORM[2][8] = {
	    // r':  -   1    2    3    4   5   6   7
	    {       0,  0,  15,  12,   7,  4,  1,  0}, // king's own file
	    {       0,  0,  12,   9,   6,  3,  1,  0}, // adjacent files
	};
	// clang-format on

	// King-file openness (issue #97), indexed by the same [d] as the tables
	// above. Half-open means WE have no pawn anywhere on the file; open means
	// neither side has. Whole-file and pawns-only, which is deliberately NOT
	// eval_rooks' forward-span-for-own-pawns asymmetry: what this prices is a
	// lane an enemy rook can use, and that is a whole-file property. See
	// eval_king_files.
	static constexpr short KING_FILE_HALF_OPEN[2] = {6, 3};
	static constexpr short KING_FILE_OPEN[2] = {11, 6};

	// Attack pressure on the king zone (issue #97). Weights per attacking
	// piece type, indexed by the same dense eMobilePiece index as the
	// aggregates, plus one weight per attacked zone square and one on the
	// king's lost flight squares. Counts come from ComputePieceAggregates;
	// these are the whole of the term's tuning surface -- see eval_king_attack
	// for the curve they feed.
	// clang-format off
	static constexpr short KING_ATTACK_WEIGHT[NUM_MOBILE_PIECES] = {
	    2, // knight
	    2, // bishop
	    3, // rook
	    5, // queen
	};
	// clang-format on
	static constexpr short KING_ZONE_SQUARE_WEIGHT = 1;

	// The quadratic's divisor and its ceiling. The CAP is what keeps this term
	// commensurate with the pawn-cover tables above rather than dwarfing them:
	// literature danger ceilings sit at 300-500 on their own, which beside a
	// halved shelter table would make shelter a tiebreaker in the attack term's
	// shadow and leave the ablation measuring the wrong thing. At this size a
	// fully committed attack is worth about what a shattered shield is.
	static constexpr int KING_DANGER_DIVISOR = 16;
	static constexpr int KING_DANGER_CAP = 120;
	// An arithmetic bound on the squaring, not a tuning knob: the cap above
	// binds from a danger of 44 upward, far below this. It is here so the
	// multiplication is bounded by the CODE rather than by the current weights,
	// which a retune can change without anyone rechecking the arithmetic.
	static constexpr int KING_DANGER_MAX = 512;
	static_assert(KING_DANGER_MAX < 46341, "KING_DANGER_MAX squared must fit in an int");
	static_assert(KING_DANGER_MAX * KING_DANGER_MAX / KING_DANGER_DIVISOR >= KING_DANGER_CAP,
	              "The clamp binds before the cap -- the cap is then unreachable and not the ceiling");
	static_assert(EvalRowMin(KING_ATTACK_WEIGHT) >= 0 && KING_ZONE_SQUARE_WEIGHT >= 0,
	              "A negative danger weight makes the penalty non-monotone in the attack it measures");

	// The largest magnitude one colour's combined king-safety contribution can
	// reach at the middlegame endpoint, in either direction. Absolute and per
	// colour, not an average: three files each able to hit its table extreme at
	// once, plus a fully capped attack. The attack term only ever penalises, so
	// it enters the worst case and not the best one.
	//
	// Derived from the tables rather than stated beside them, so a retune that
	// outgrows the declared bound fails to compile. EvalTermTests asserts the
	// same bound over the eval corpus, which is what catches a bound that is
	// arithmetically right and wrong about which entries are reachable.
	static constexpr int KING_PAWN_COVER_WORST = -(EvalRowMin(KING_SHELTER[0]) + 2 * EvalRowMin(KING_SHELTER[1])) +
	                                             (EvalRowMax(KING_STORM[0]) + 2 * EvalRowMax(KING_STORM[1])) +
	                                             (KING_FILE_OPEN[0] + 2 * KING_FILE_OPEN[1]);
	static constexpr int KING_SAFETY_WORST_PENALTY = KING_PAWN_COVER_WORST + KING_DANGER_CAP;
	static constexpr int KING_SAFETY_BEST_BONUS = EvalRowMax(KING_SHELTER[0]) + 2 * EvalRowMax(KING_SHELTER[1]);
	static constexpr int KING_SAFETY_MAX_PENALTY =
	    (KING_SAFETY_WORST_PENALTY > KING_SAFETY_BEST_BONUS) ? KING_SAFETY_WORST_PENALTY : KING_SAFETY_BEST_BONUS;

	// KING_SAFETY_MAX_PENALTY bounds the positive direction by the shelter
	// extremum alone, which is only correct while the storm and king-file entries
	// are penalty MAGNITUDES. A negative one among them would turn that sub-term
	// into a bonus and step outside the declared bound without failing anything
	// below, so the sign is asserted rather than assumed.
	static_assert(EvalRowMin(KING_STORM[0]) >= 0 && EvalRowMin(KING_STORM[1]) >= 0 && KING_FILE_OPEN[0] >= 0 &&
	                  KING_FILE_OPEN[1] >= 0 && KING_FILE_HALF_OPEN[0] >= 0 && KING_FILE_HALF_OPEN[1] >= 0,
	              "Storm and king-file entries are penalty magnitudes -- a negative one breaks the bound below");

	// One colour's swing is bounded here; the swing BETWEEN the two sides is
	// twice it, so this ceiling still permits a king-safety difference above a
	// minor piece. It is a tripwire against a retune going an order of magnitude
	// wrong, not a claim that the term cannot decide a game.
	static_assert(KING_SAFETY_MAX_PENALTY < 300, "King safety outweighs a piece per colour -- rescale the tables");

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
	// The gate asks what force the DEFENDER still has, not how much of it there
	// is (issue #118 item 5). A phase budget could not express that: a lone
	// queen is phase 4 and passed the old `loser phase <= 6` gate, so Q+R vs Q
	// was paid for chasing a king that was never going to be cornered. The
	// defender may therefore hold anything but a queen, which is the one piece
	// that can check the winner's king away from the corner indefinitely and
	// leave the plan permanently unfinished. Everything mop-up exists for still
	// qualifies, KQQ vs K included, however much material the winner has.
	//
	// Widening this to "no queen and no rook" is a mistake worth recording,
	// because the second-order effect runs the other way: eval_pst suppresses
	// the winner's king PST exactly when mop-up is active (item 4), so removing
	// a class from the gate does not withdraw a bonus, it re-enables a
	// CENTRALIZING king table. On K+Q vs K+R that turns a reward for walking
	// toward the cornered king into a penalty — the item 4 defect, reinstated
	// for a family of endings won by exactly that walk.

	// Pawnless rook endings (#128), as numerators over ENDGAME_SCALE_MAX.
	//
	// Unlike the classes scaled to zero, none of these is drawn by material:
	// they are drawn by tendency, which is why they are scaled rather than
	// clamped. That distinction is the whole reason for a fractional scale — a
	// clamp would throw away the versions of the ending that do convert, while a
	// factor still leaves the search preferring the better one. They are
	// therefore ordinary strength parameters, retained, adjusted or dropped on
	// match evidence, where the exact-draw classes ship on the rules of chess.
	//
	// The extra minor rarely produces more than a stalemate trick against a rook
	// that is free to give itself up for it, hence the heavy discount. A rook
	// against a bare minor is a different case and gets a light one: it is
	// drawish taken as a whole, but the subset where the evaluation already
	// claims a real advantage converts most of the time, and that subset is
	// exactly the one a scale acts on. Discounting it hard would be reading the
	// aggregate number as if it applied to the positions it does not describe.
	//
	// Rook against rook inverts that reasoning and so takes the heavy discount:
	// material is level, the whole score is positional, and the band a scale acts
	// on is the low one, which is the band that never converts. It is discounted
	// rather than zeroed because the class is left by winning the rook, and a zero
	// removes the king- and rook-activity ordering that steers toward the position
	// where the defence breaks. The scale truncates toward zero, so what survives
	// is that ordering, coarsened into plateaus — not a smooth gradient.
	//
	// A scale of any kind is load-bearing outside this file: it multiplies the
	// score instead of adding to it, which is what quiescence's delta and SEE
	// pruning both assume it cannot do. AIPerplex::quiescence() disables both
	// near a scaled class, keyed on piece count rather than on the class list.
	//
	// That keying rests on a property of the classes below, which is stated here
	// because this is where it would be broken: EVERY SCALED CLASS LEAVES ITS
	// SMALLER SIDE AT MOST TWO MEN. Recognising one whose smaller side holds
	// three or more silently defeats MATERIAL_PRUNING_MIN_PIECES, and that
	// threshold must be raised in the same change. Rook against rook sits exactly
	// on that bound at two men a side, so there is no headroom left in it.
	static const short ROOK_AND_MINOR_VS_ROOK_SCALE = 4;
	static const short ROOK_VS_MINOR_SCALE = 12;
	static const short ROOK_VS_ROOK_SCALE = 4;

	// Distance helpers for mop-up scoring — plain grid math, orientation-independent
	// (works the same whether the square belongs to White or Black).
	static constexpr int CenterAxisDistance(int coord) noexcept { return (coord <= 3) ? (3 - coord) : (coord - 4); }
	static constexpr int CenterManhattanDistance(eSquare square) noexcept
	{
		return CenterAxisDistance(File(square)) + CenterAxisDistance(Rank(square));
	}
	static constexpr int AbsDiff(int a, int b) noexcept { return (a > b) ? (a - b) : (b - a); }
	static constexpr int Clamp(int value, int low, int high) noexcept
	{
		return (value < low) ? low : ((value > high) ? high : value);
	}

	// The anchor of the king's safety zone: the king's own square pulled onto
	// the b..g files and off the two outermost rows.
	//
	// BOTH clamps are load-bearing. The FILE clamp is what makes everything
	// keyed on the zone constant-width: without it a king stepping g1->h1 loses
	// a third of its surroundings, every count taken over them drops, and
	// retreating into the corner reads as safer. The RANK clamp keeps the 3x3
	// block around the anchor wholly on the board, so it is always exactly nine
	// squares.
	//
	// Kg1, Kh1 and Kg2 all anchor on g2 and are therefore treated identically,
	// which is the point.
	static constexpr eSquare KingZoneAnchor(eSquare kingSq) noexcept
	{
		return static_cast<eSquare>(Clamp(Rank(kingSq), 1, 6) * ONE_ROW + Clamp(File(kingSq), 1, 6));
	}

	// The king's safety zone: the 3x3 block around that anchor, plus the same
	// block shifted one rank in `color`'s forward direction. g_bbKingMoves is
	// the eight-square ring WITHOUT its centre, hence the explicit centre bit.
	//
	// Twelve squares everywhere except where the forward rank falls off the
	// board -- a king on its own 7th or 8th rank -- where it is nine. That
	// exception is left standing rather than clamped away: a king on the enemy
	// back rank has nothing in front of it, and inventing squares for it would
	// be worse than a smaller zone. The anchor's rank clamp is what keeps the
	// 3x3 block itself always exactly nine squares.
	//
	// Forward is DECREASING square index for White (a8 = 0 ... h1 = 63), so the
	// shifts read backwards from the colours they serve.
	static constexpr BITBOARD KingZone(eSquare kingSq, eColor color) noexcept
	{
		const eSquare anchor = KingZoneAnchor(kingSq);
		const BITBOARD block = g_bbKingMoves[anchor] | (UNIT << anchor);
		return block | ((color == WHITE) ? (block >> ONE_ROW) : (block << ONE_ROW));
	}

	// A square's rank from `color`'s point of view: 1 is that colour's back
	// rank, 8 the enemy's. The board's own Rank() is 0 = rank 8 ... 7 = rank 1,
	// i.e. White's forward direction is DECREASING. See KING_SHELTER.
	static constexpr int RelativeRank(eSquare square, eColor color) noexcept
	{
		const int row = Rank(square);
		return (color == WHITE) ? (8 - row) : (row + 1);
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
	// this.
	static EvalContext BuildContext(const Board& board) noexcept;

	// Generates every non-pawn piece's attack set once and reduces it to counts.
	// Called by BuildContext, and only when endgame_scale is nonzero.
	//
	// Consolidated here rather than left in eval_mobility because more than one
	// term needs the same attack sets: mobility counts them, eval_rooks needs
	// the rook attacks for connected rooks (issue #114), and king safety
	// (issue #97) needs all of them. Generating them per term costs one PEXT
	// lookup per slider per consumer.
	//
	// Takes the bitboards it reads rather than the half-built EvalContext, and
	// returns its result rather than writing through a reference: that is what
	// lets the optimizer keep the whole pass in registers.
	static PieceAggregates ComputePieceAggregates(std::span<const BITBOARD> boards, BITBOARD whitePawnAttacks,
	                                              BITBOARD blackPawnAttacks,
	                                              const eSquare (&kingSq)[NUM_COLORS]) noexcept;

	// Recognises material configurations that are worth less than they weigh.
	// Returns a numerator over ENDGAME_SCALE_MAX; 0 is a dead draw.
	//
	// Piece counts decide every class but one. It is not a tablebase (#101) and
	// does not judge pawn races; the wrong-coloured-bishop fortress is the sole
	// place a square, rather than a count, is what makes the position drawn.
	static int EndgameScale(std::span<const BITBOARD> boards) noexcept;

	// The pawns-on-the-board branch of the classifier.
	static int WrongBishopFortress(std::span<const BITBOARD> boards) noexcept;

	// The pawnless-with-rooks branch of the classifier. Split out because it is
	// the one branch that has to count both sides' pieces, and inlining it into
	// EndgameScale would put those counts textually in front of the early-outs
	// that exist to avoid them.
	static int PawnlessRookScale(std::span<const BITBOARD> boards) noexcept;

	// Applies a scale to a white-POV score. Split out so Evaluate() and
	// Breakdown() cannot hold two versions of the arithmetic.
	static constexpr int ApplyEndgameScale(int white_pov, int scale) noexcept
	{
		// Integer division truncates toward zero, so this is odd-symmetric:
		// scaling the mirror of a position still yields the mirror of its
		// score, which is what keeps the color-symmetry property intact.
		return (scale == ENDGAME_SCALE_MAX) ? white_pov : (white_pov * scale) / ENDGAME_SCALE_MAX;
	}

	// Material plus every blended term, summed white-minus-black and unscaled.
	static int RawWhitePov(const EvalContext& ctx) noexcept;

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
	// The one exception to the ScorePair signature above: all three king-safety
	// contributions fall out of a single scan over the king's three files, and
	// rescanning them to keep three identical signatures cost measurable nps for
	// nothing. They stay separate all the way to their own breakdown rows.
	static KingPawnCover eval_king_pawn_cover(const EvalContext& ctx, eColor color) noexcept;
	static ScorePair eval_king_attack(const EvalContext& ctx, eColor color) noexcept;

	// The danger curve of D6 as a pure function of the weighted count that feeds
	// it, split out so a monotonicity sweep can drive it directly instead of
	// hunting for positions that happen to produce each input.
	//
	// Every weight is non-negative and every count is a population count, so a
	// negative `danger` is unreachable today. The lower clamp is kept anyway:
	// it is what makes monotonicity a property of THIS function rather than of
	// its callers, and a later term contributing a safety BONUS -- king flight
	// squares were exactly that, before their cost took them out -- would
	// otherwise be squared straight back into a penalty.
	static constexpr int KingDangerPenalty(int danger) noexcept
	{
		const int clamped = Clamp(danger, 0, KING_DANGER_MAX);
		return Clamp(clamped * clamped / KING_DANGER_DIVISOR, 0, KING_DANGER_CAP);
	}

  public:
	int Evaluate(const Board& board) const noexcept override;
	const char* GetType() const noexcept override { return "Complex"; }

	// Per-term introspection for the UCI 'eval' command. Reports what the four
	// private term functions above contribute, per color, for one position;
	// changes nothing and is never called from search.
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
