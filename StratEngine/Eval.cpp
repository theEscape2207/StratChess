// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Eval.h"

#include "Board.h"
#include "Magic.h"   // RookAttacks, for connected rooks (issue #114)

#include <bit>       // std::popcount

// static Factory constructor
std::unique_ptr<EvalManager> EvalManager::Create(EvalTypes type)
{
	switch (type) {
	case EvalTypes::NONE:		return nullptr;
	case EvalTypes::SIMPLE:		return std::make_unique<EvalSimple>();
	case EvalTypes::COMPLEX:	return std::make_unique<EvalComplex>();
	default:			throw std::invalid_argument("Unknown Eval type");	// Oops... another eval
	}
}

////////////////////////////
//
// Class EvalSimple
//
/*
 *	Evaluate() :
 *	Description: Sums up the material value from both colors + their positional value
 *	Returns:	 The value of the player in turn subtracted the oppositions value
 */
int EvalSimple::Evaluate(const Board& board) const noexcept
{
	const eColor inTurn = board.GetCurrentColor();

	int totalScore = 0;

	//Check every field in the Board array if the piece is there.
	for (int temp = a8; temp < NUM_SQUARES; ++temp)	// Hmm... iterator instead?
	{
		const auto square = static_cast<eSquare>(temp);
		// Henter Briktype fra BoardArray; enten NO_PIECE eller briktype
		const ePiece piece = board.GetPiece(square);

		//hvis staar en brik paa feltet
		if (PieceHelper::IsActual(piece))
		{
			//Add the eval-tabelvalue to the score. Rotates if Black.
			// Hvem er i tur og er det paagaeldendes brik		+ material value
			const int pieceScore = GetPositionalScore(square, piece) + PieceHelper::Value(piece);

			if (PieceHelper::Color(piece) == inTurn) //-V1051
			{
				totalScore += pieceScore;
			}
			else
			{
				totalScore -= pieceScore;
			}
		}
	}

	return totalScore;
}

/////////////////////////////////////////////////////////
//
// Class EvalComplex implementation
//

// eval_pawns — doubled and isolated pawn penalties for one color's pawns
// (issue #127 restructure — see .claude/plans/eval-context-restructure.md).
// Loops that color's own pawn bitboard directly; the per-pawn logic is
// unchanged from the switch cases it replaces, just no longer keyed off the
// outer per-square loop.
//
// TODO: Add bonus for passed pawn - bonus should be dependant on game stage
// (carried over verbatim from the pre-#127 switch; still open, see issue #116).
ScorePair EvalComplex::eval_pawns(const EvalContext& ctx, eColor color) noexcept
{
	int score = 0;
	const BITBOARD ownPawns = ctx.pawns[color];

	auto remaining = ownPawns;
	while (remaining)
	{
		const eSquare square = Board::GetFirstPiece(remaining);
		const int file = File(square);

		if (color == WHITE)
		{
			// Hvis der er en hvid bonde over denne i samme kolonne gives en straf
			if (ownPawns & g_bbFileUpMask[square])
				score -= DOUBLED_PAWN_PENALTY;
		}
		else
		{
			// Hvis der er en sort bonde under denne i samme kolonne gives en straf
			if (Bits::isAnyBitSet(ownPawns, g_bbFileDownMask[square]))
				score -= DOUBLED_PAWN_PENALTY;
		}

		// Hvis der ikke er en bonde i en af raekkerne ved siden af gives en straf
		if ((file == eFileNames::LEFT_FILE || !(ownPawns & g_bbFileMask[file - 1])) &&
			(file == eFileNames::RIGHT_FILE || !(ownPawns & g_bbFileMask[file + 1])))
			score -= ISOLATED_PAWN_PENALTY;

		remaining = Bits::clearLsb(remaining);
	}

	// No phase sensitivity: doubled/isolated pawn structure matters equally
	// throughout, so the same value goes to both endpoints and the blend is a
	// no-op for this term.
	return ScorePair{ score, score };
}

// eval_rooks — 7th-rank and half-open/open-file bonuses for one color's
// rooks (issue #127 restructure — see
// .claude/plans/eval-context-restructure.md). Loops that color's own rook
// bitboard directly; per-rook logic is unchanged from the switch cases it
// replaces.
//
// "Open" means no PAWNS of either colour on the file (issue #126 / PR #137)
// — not "no enemy pieces at all". An enemy piece sharing the file is usually
// a target for the rook, not a reason to demote the bonus. Preserving this
// (the fixed form, not the pre-#137 all_black/all_white check) is required —
// see the "Superseded note" for the rook terms in the plan file.
//
// Note the two file tests use different scopes: the own-pawn test is
// FORWARD-ONLY (g_bbFileUpMask/g_bbFileDownMask), the enemy-pawn test is
// WHOLE-FILE (g_bbFileMask). So an own pawn behind the rook still leaves the
// file open, while an enemy pawn behind it does not. That asymmetry is
// inherited, not principled — D5 in
// .claude/plans/passed-and-backwards-pawn-terms.md keeps it deliberately,
// because widening the own-pawn test would change far more positions than
// issue #126's actual fix. The open question of whether to widen it is
// recorded on issue #116, not settled here.
//
// Connected rooks (issue #114) are scored here too: same rank or file with
// nothing between, per connected pair.
ScorePair EvalComplex::eval_rooks(const EvalContext& ctx, eColor color) noexcept
{
	int score = 0;            // phase-independent: open/half-open file bonuses
	int seventhRankEg = 0;    // endgame-only contribution
	int connectedPairs = 0;   // tapered separately -- see the return below
	const ePiece rookPiece = (color == WHITE) ? ePiece::WHITE_ROOK : ePiece::BLACK_ROOK;
	const int seventhRank = (color == WHITE) ? WHITE_7TH_ROW : BLACK_7TH_ROW;
	const BITBOARD ownPawns = ctx.pawns[color];
	const BITBOARD enemyPawns = ctx.pawns[(color == WHITE) ? BLACK : WHITE];

	auto remaining = ctx.boards[rookPiece];
	while (remaining)
	{
		const eSquare square = Board::GetFirstPiece(remaining);
		const int rank = Rank(square);
		const int file = File(square);

		// Bonus hvis taarnet er i syvende raekke sent i spillet.
		// Endgame-weighted rather than hard-gated on stage (D3, issue #99):
		// contributes 0 at the mg endpoint and ROOK_ON_7TH_BONUS at eg, which
		// reproduces the old "endgame only" intent continuously instead of as a
		// step. Whether a 7th-rank rook really deserves to be endgame-only is
		// dubious chess — it is often strongest in the middlegame against pawns
		// still on their starting squares — but re-weighting it is a tuning
		// decision for #117, deliberately not made here.
		if (rank == seventhRank)
			seventhRankEg += ROOK_ON_7TH_BONUS;

		// Bonus hvis der er aabne raekker til taarnet
		const BITBOARD ownForwardMask = (color == WHITE) ? g_bbFileUpMask[square] : g_bbFileDownMask[square];
		if (!(ownPawns & ownForwardMask))
		{
			score += HALF_OPEN_FILE;

			if (!(g_bbFileMask[file] & enemyPawns))
				score += OPEN_FILE - HALF_OPEN_FILE;
		}

		// Connected rooks: does this rook see another of its own along a rank or
		// file with nothing in between? RookAttacks already accounts for blockers
		// (PEXT magics, issue #108), so no separate between-mask is needed.
		//
		// `remaining` has had every earlier rook cleared, so testing only against
		// the later ones counts each PAIR exactly once -- no halving needed.
		const BITBOARD laterRooks = Bits::clearLsb(remaining);
		if (laterRooks)
			connectedPairs += std::popcount(RookAttacks(square, ctx.all_pieces) & laterRooks);

		remaining = Bits::clearLsb(remaining);
	}

	return ScorePair{
		score + connectedPairs * CONNECTED_ROOKS_BONUS_MG,
		score + seventhRankEg + connectedPairs * CONNECTED_ROOKS_BONUS_EG
	};
}

// eval_bishops -- bishop pair bonus for one color (issue #111).
//
// Requires bishops on OPPOSITE square colours rather than merely two bishops.
// The term exists because the pair covers both colours; two same-coloured
// bishops (reachable by underpromotion) do not, and a plain count would pay
// for them anyway. One mask and two tests, so correctness is free here.
ScorePair EvalComplex::eval_bishops(const EvalContext& ctx, eColor color) noexcept
{
	// Square 0 is a8, a LIGHT square, and bit 0 of this mask is clear -- so the
	// mask holds the DARK squares. Named for what it actually contains: the
	// pair test is invariant under swapping the two, but a mislabelled constant
	// would mislead the next term that needs square colour for real (bishop-on
	// -own-pawn-colour, opposite-coloured-bishop draw scaling).
	static constexpr BITBOARD DARK_SQUARES = 0x55AA55AA55AA55AAULL;

	const ePiece bishopPiece = (color == WHITE) ? ePiece::WHITE_BISHOP : ePiece::BLACK_BISHOP;
	const BITBOARD bishops = ctx.boards[bishopPiece];

	const bool hasDark = (bishops & DARK_SQUARES) != 0;
	const bool hasLight = (bishops & ~DARK_SQUARES) != 0;

	if (!(hasLight && hasDark))
		return ScorePair{};

	return ScorePair{ BISHOP_PAIR_BONUS_MG, BISHOP_PAIR_BONUS_EG };
}

// eval_castling -- king-shelter proxy for one color (issue #115).
//
// Derived from castling RIGHTS plus king placement, never from move history.
// Whether a side actually castled is not recoverable from a FEN, so a term
// that depended on it would make Evaluate() a function of how a position was
// reached rather than of the position -- two paths to one position would then
// disagree about its score while sharing a transposition-table entry, and
// #117's FEN corpus would score every position as never-castled. See D2 in
// .claude/plans/eval-bishop-pair-connected-rooks-castling.md.
//
// While a right remains the side has decided nothing, so the term is silent.
// Once both rights are gone, the king is either tucked away (bonus) or was
// dragged off its castling squares without ever getting there (penalty).
//
// Middlegame-only: the endgame king belongs in the centre and eval_pst's
// endgame table already pays for that, so a flat bonus would fight it.
ScorePair EvalComplex::eval_castling(const EvalContext& ctx, eColor color) noexcept
{
	const uint8_t sideRights = (color == WHITE)
		? CastlingRights::WHITE_BOTH
		: CastlingRights::BLACK_BOTH;

	if (ctx.castling_rights & sideRights)
		return ScorePair{};   // still flexible; nothing decided yet

	// Kingless board (default-constructed or failed-parse only) -- same guard as
	// eval_pst and eval_mopup.
	const eSquare kingSq = ctx.king_sq[color];
	if (kingSq == NO_SQUARE)
		return ScorePair{};

	const int homeRank = (color == WHITE) ? WHITE_BACK_ROW : BLACK_BACK_ROW;
	if (Rank(kingSq) != homeRank)
		return ScorePair{ -CASTLING_LOST_PENALTY, 0 };

	// Graded by file rather than by the exact castling destination square: a
	// castled king routinely steps to h1 or a1 afterwards, and the term should
	// not evaporate when it does. a1 is the queenside analogue of h1, not of b1 --
	// Kb1-a1 is standard in opposite-side-castling Sicilians.
	//
	// Three levels, not two. A binary test puts a full bonus-to-penalty cliff on
	// one quiet king step (g1->f1), and nothing smooths it: the middlegame king
	// PST is rank-only (defines.h), so this term is the only file signal in the
	// middlegame. f is neither sheltered nor exposed, so it scores zero and the
	// worst single-step swing is halved.
	const int file = File(kingSq);
	if (file <= 2 || file >= 6)             // a,b,c | g,h -- sheltered
		return ScorePair{ CASTLING_DONE_BONUS, 0 };
	if (file == 5)                          // f -- neutral
		return ScorePair{};
	return ScorePair{ -CASTLING_LOST_PENALTY, 0 };   // d,e -- central
}

// eval_pst — piece-square-table contribution for one color's pieces (issue
// #127 restructure — see .claude/plans/eval-context-restructure.md — D5's
// per-piece-type bitboard loops, replacing an earlier mailbox-lookup
// version). Every non-king piece type gets its own bitboard loop, so
// GetPositionalScore is called with a statically-known piece — no
// board.GetPiece(square) mailbox lookup per square. The king is excluded
// from those loops: g_Eval_Bitboards[5] (middlegame) and [6] (endgame) are
// its mg and eg endpoints, blended by phase rather than selected (issue #99),
// so it still receives exactly one PST contribution — just an interpolated
// one. (D2 in eval-context-restructure.md, not this change's D2.) Reordering these per-type loops relative to each
// other, or relative to the old single mailbox loop, cannot change the sum:
// plain int addition over the same multiset of per-square PST values.
ScorePair EvalComplex::eval_pst(const EvalContext& ctx, eColor color) noexcept
{
	int score = 0;

	// One bitboard loop per non-king piece type, each already knowing its
	// own ePiece value — the PST lookup needs nothing else.
	static constexpr ePiece kNonKingPieces[5][NUM_COLORS] = {
		{ ePiece::WHITE_PAWN,   ePiece::BLACK_PAWN   },
		{ ePiece::WHITE_KNIGHT, ePiece::BLACK_KNIGHT },
		{ ePiece::WHITE_BISHOP, ePiece::BLACK_BISHOP },
		{ ePiece::WHITE_ROOK,   ePiece::BLACK_ROOK   },
		{ ePiece::WHITE_QUEEN,  ePiece::BLACK_QUEEN  },
	};

	for (const auto& piecePair : kNonKingPieces)
	{
		const ePiece piece = piecePair[color];

		auto remaining = ctx.boards[piece];
		while (remaining)
		{
			const eSquare square = Board::GetFirstPiece(remaining);
			score += GetPositionalScore(square, piece);
			remaining = Bits::clearLsb(remaining);
		}
	}

	// King PST: its own tapered pair, not the generic table above.
	// TODO: Add bonus for castling-done!! (carried over verbatim from the
	// pre-#127 switch; still open).
	//
	// Guard against a missing king: ctx.king_sq[color] is NO_SQUARE for a
	// color with no king on the board (default-constructed or failed-parse
	// Board only — see the EvalContext::king_sq comment in Eval.h). The old
	// switch-based Evaluate() never reached this code for a kingless board at
	// all, because the outer loop iterated ALL_PIECES and a truly empty board
	// has none; skipping here reproduces that "no king PST contribution"
	// outcome instead of indexing g_Eval_Bitboards with GetFirstPiece(0),
	// which is undefined (Debug: assert trips; Release: reads out of bounds).
	// The king is the first genuinely tapered term (D3, issue #99). The two
	// tables were always an (mg, eg) pair — g_Eval_Bitboards[5] wants the king
	// tucked away, [6] wants it centralized — they were just selected
	// discontinuously. Blending them removes a cliff of up to 100 cp that a
	// single capture could cross mid-search (the mg table is uniformly
	// -40/-20/0 by rank; the eg table peaks at +60 centrally).
	// Suppressed entirely while this color is mopping up (issue #118 item 4,
	// D5(a) in the plan). Otherwise the endgame king table charges the winner
	// 10 cp per step of centralization given up to walk toward the cornered
	// loser, against the 4 cp per step mop-up pays for closing in — so
	// approaching scored NEGATIVE overall, and mop-up only softened a
	// disincentive it was written to remove. Letting mop-up own king placement
	// outright restores the standard formulation, and decouples
	// MOPUP_CMD_WEIGHT from the endgame king PST slope — two numbers that
	// otherwise express one concept and would fight each other under #117.
	int kingMg = 0;
	int kingEg = 0;
	const eSquare kingSq = ctx.king_sq[color];
	if (kingSq != NO_SQUARE && !ctx.mopup_active[color])
	{
		const ePiece kingPiece = (color == WHITE) ? ePiece::WHITE_KING : ePiece::BLACK_KING;
		const int kingIdx = getEvalBoard(kingPiece, kingSq);
		kingMg = g_Eval_Bitboards[5][kingIdx];
		kingEg = g_Eval_Bitboards[6][kingIdx];
	}

	return ScorePair{ score + kingMg, score + kingEg };
}

// eval_mobility -- how many squares this color's pieces can move to (issues
// #98 and #113).
//
// PSEUDO-LEGAL, not legal: filtering for check-legality would need move
// generation per piece per node, and Evaluate() runs at quiescence frequency.
// Every strong engine counts pseudo-legal squares here; the difference is noise
// next to the term's weight.
//
// SAFE mobility: squares an enemy pawn covers are excluded, because a square a
// pawn guards is not one a knight can usefully occupy.
//
// Enemy-OCCUPIED squares are INCLUDED -- a piece that can capture is active, and
// this needs one mask rather than two. Both conventions exist in the literature;
// what matters is that every piece type below uses the same one, which is why
// the mask is computed once here rather than per type.
//
// The king is deliberately absent: king mobility is a king-safety signal and
// belongs with issue #97, where it can be weighed against attacker counts rather
// than paid as a flat per-square bonus.
ScorePair EvalComplex::eval_mobility(const EvalContext& ctx, eColor color) noexcept
{
	const eColor enemy = (color == WHITE) ? BLACK : WHITE;
	const BITBOARD usable = ~ctx.occupied[color] & ~ctx.pawn_attacks[enemy];

	int mg = 0;
	int eg = 0;

	auto knights = ctx.boards[(color == WHITE) ? ePiece::WHITE_KNIGHT : ePiece::BLACK_KNIGHT];
	while (knights)
	{
		const eSquare square = Board::GetFirstPiece(knights);
		const int count = std::popcount(g_bbKnightMoves[square] & usable);
		mg += count * MOBILITY_KNIGHT_MG;
		eg += count * MOBILITY_KNIGHT_EG;
		knights = Bits::clearLsb(knights);
	}

	auto bishops = ctx.boards[(color == WHITE) ? ePiece::WHITE_BISHOP : ePiece::BLACK_BISHOP];
	while (bishops)
	{
		const eSquare square = Board::GetFirstPiece(bishops);
		const int count = std::popcount(BishopAttacks(square, ctx.all_pieces) & usable);
		mg += count * MOBILITY_BISHOP_MG;
		eg += count * MOBILITY_BISHOP_EG;
		bishops = Bits::clearLsb(bishops);
	}

	auto rooks = ctx.boards[(color == WHITE) ? ePiece::WHITE_ROOK : ePiece::BLACK_ROOK];
	while (rooks)
	{
		const eSquare square = Board::GetFirstPiece(rooks);
		const int count = std::popcount(RookAttacks(square, ctx.all_pieces) & usable);
		mg += count * MOBILITY_ROOK_MG;
		eg += count * MOBILITY_ROOK_EG;
		rooks = Bits::clearLsb(rooks);
	}

	// A queen is a rook and a bishop on the same square; there is no separate
	// PEXT table for it (Magic.h), and the union is what every generator uses.
	auto queens = ctx.boards[(color == WHITE) ? ePiece::WHITE_QUEEN : ePiece::BLACK_QUEEN];
	while (queens)
	{
		const eSquare square = Board::GetFirstPiece(queens);
		const BITBOARD attacks =
			RookAttacks(square, ctx.all_pieces) | BishopAttacks(square, ctx.all_pieces);
		const int count = std::popcount(attacks & usable);
		mg += count * MOBILITY_QUEEN_MG;
		eg += count * MOBILITY_QUEEN_EG;
		queens = Bits::clearLsb(queens);
	}

	return ScorePair{ mg, eg };
}

// eval_mopup — mop-up evaluation for one color (issue #127 restructure — see
// .claude/plans/eval-context-restructure.md; original term issue #70 /
// epic #110). In decisively-won, pawnless endings, reward driving the losing
// king to the edge/corner and closing the distance between the two kings —
// the win may lie beyond the search horizon otherwise. Only the winning
// color receives a nonzero contribution; the losing color, and both colors
// when the gating conditions aren't met, get 0 — matching the original
// bonusScore[winner]-only update this replaces.
ScorePair EvalComplex::eval_mopup(const EvalContext& ctx, eColor color) noexcept
{
	// The gate — pawnless, decisive material lead, both kings on the board, and
	// the loser stripped down — is evaluated once in BuildContext, and only the leading color is
	// marked active. eval_pst reads the same flag to suppress that color's king
	// PST (issue #118 item 4), so the two terms cannot disagree about whether a
	// position is a mop-up. A kingless board is excluded there, so the king
	// squares read below are known valid.
	if (!ctx.mopup_active[color])
		return ScorePair{};

	const eColor loser = (color == WHITE) ? BLACK : WHITE;
	const eSquare winnerKingSq = ctx.king_sq[color];
	const eSquare loserKingSq = ctx.king_sq[loser];

	const int mopup = MOPUP_CMD_WEIGHT * CenterManhattanDistance(loserKingSq) +
		MOPUP_KINGDIST_WEIGHT * (MOPUP_MAX_KING_DISTANCE - KingDistance(winnerKingSq, loserKingSq));

	// Gated, not blended (D4): once the gate opens the term applies at full
	// strength at both endpoints.
	return ScorePair{ mopup, mopup };
}

// BuildContext — the one construction site for EvalContext (issue #127
// restructure — see .claude/plans/eval-context-restructure.md). Both
// Evaluate() below and the term-level test fixture
// (StratChessTests/EvalTests.cpp's EvalComplexTestFixture) call this, so
// phase detection and every other context field can only be computed one
// way — no risk of the test fixture silently drifting onto a stale copy of
// the `11500` threshold or similar (see issue #99, which will eventually
// replace that threshold).
EvalContext EvalComplex::BuildContext(const Board& board) noexcept
{
	const int matScoreWhite = board.GetMaterialScore(WHITE);
	const int matScoreBlack = board.GetMaterialScore(BLACK);

	const auto boardsSpan = board.GetBitBoards();

#ifndef NDEBUG
	// Debug-only bitboard/mailbox consistency tripwire. The pre-#127 switch
	// had `default: assert(!"What! A new type of piece...")` in its per-square
	// mailbox loop, tripping if board.GetPiece() ever returned something
	// outside the twelve real piece values while iterating ALL_PIECES. The
	// per-type bitboard loops this restructure introduced (eval_pawns,
	// eval_rooks, eval_pst) no longer route through that mailbox lookup at
	// all, so there is no per-square switch left for that trap to live in.
	// This reproduces the same intent at the one place all of them now read
	// from: the union of the twelve per-piece-type bitboards must equal
	// ALL_PIECES, or a bitboard has drifted out of sync with what the board
	// thinks is occupied.
	{
		BITBOARD unionOfTypes = 0ULL;
		for (int p = ePiece::WHITE_PAWN; p <= ePiece::BLACK_KING; ++p)
			unionOfTypes |= boardsSpan[p];
		assert(unionOfTypes == boardsSpan[ALL_PIECES] &&
			"Eval: per-type piece bitboards do not reconstruct ALL_PIECES");

		// The per-COLOR occupancy is a separate pair of bitboards, and until
		// eval_mobility (#98) no Eval term read them -- so the check above never
		// covered them. Mobility masks against occupied[], so a drift there
		// silently mis-scores every position rather than tripping anything.
		assert((boardsSpan[ePiece::ALL_WHITE_PIECES] | boardsSpan[ePiece::ALL_BLACK_PIECES])
				== boardsSpan[ALL_PIECES] &&
			"Eval: per-color occupancy does not reconstruct ALL_PIECES");
	}
#endif

	// king_sq is NO_SQUARE for a color with no king on the board — only
	// reachable via a default-constructed or failed-parse Board (see the
	// EvalContext::king_sq comment in Eval.h). GetFirstPiece has an
	// assert(mask != 0) precondition that is compiled out in Release, so it
	// must not be called on an empty king bitboard.
	const eSquare whiteKingSq = (boardsSpan[ePiece::WHITE_KING] != 0ULL)
		? Board::GetFirstPiece(boardsSpan[ePiece::WHITE_KING]) : NO_SQUARE;
	const eSquare blackKingSq = (boardsSpan[ePiece::BLACK_KING] != 0ULL)
		? Board::GetFirstPiece(boardsSpan[ePiece::BLACK_KING]) : NO_SQUARE;

	// Game phase from non-king, non-pawn piece counts (issue #99). Summed over
	// both colors and clamped: promotions can push the raw sum past
	// MAX_GAME_PHASE (three queens on one side is 12 from queens alone), and an
	// unclamped phase would extrapolate outside the interpolation range instead
	// of saturating at "opening".
	const int phaseWhite =
		PHASE_KNIGHT * std::popcount(boardsSpan[ePiece::WHITE_KNIGHT]) +
		PHASE_BISHOP * std::popcount(boardsSpan[ePiece::WHITE_BISHOP]) +
		PHASE_ROOK   * std::popcount(boardsSpan[ePiece::WHITE_ROOK])   +
		PHASE_QUEEN  * std::popcount(boardsSpan[ePiece::WHITE_QUEEN]);
	const int phaseBlack =
		PHASE_KNIGHT * std::popcount(boardsSpan[ePiece::BLACK_KNIGHT]) +
		PHASE_BISHOP * std::popcount(boardsSpan[ePiece::BLACK_BISHOP]) +
		PHASE_ROOK   * std::popcount(boardsSpan[ePiece::BLACK_ROOK])   +
		PHASE_QUEEN  * std::popcount(boardsSpan[ePiece::BLACK_QUEEN]);
	const int rawPhase = phaseWhite + phaseBlack;
	const int gamePhase = (rawPhase > MAX_GAME_PHASE) ? MAX_GAME_PHASE : rawPhase;

	// Mop-up gate, evaluated here rather than inside eval_mopup so eval_pst can
	// consult the same answer (issue #118 item 4): pawnless, both kings present,
	// a decisive material lead, and the LOSER reduced to little material. Only
	// the leader is active. Gating on the loser's phase rather than the total is
	// what keeps KQQ-vs-K in scope — see MOPUP_MAX_LOSER_PHASE in Eval.h.
	//
	// The kingless guard is load-bearing and is why this cannot simply fall out
	// of the material check: a default-constructed or failed-parse Board is
	// pawnless at phase 0, and while its material difference is 0 today, that
	// would stop saving us if MOPUP_MATERIAL_THRESHOLD ever changed.
	bool mopupActive[NUM_COLORS] = { false, false };
	if (boardsSpan[ePiece::WHITE_PAWN] == 0ULL && boardsSpan[ePiece::BLACK_PAWN] == 0ULL &&
		whiteKingSq != NO_SQUARE && blackKingSq != NO_SQUARE)
	{
		const int matDiff = matScoreWhite - matScoreBlack;
		const int absMatDiff = (matDiff >= 0) ? matDiff : -matDiff;
		if (absMatDiff >= MOPUP_MATERIAL_THRESHOLD)
		{
			const eColor winner = (matDiff > 0) ? WHITE : BLACK;
			const int loserPhase = (winner == WHITE) ? phaseBlack : phaseWhite;
			if (loserPhase <= MOPUP_MAX_LOSER_PHASE)
				mopupActive[winner] = true;
		}
	}

	// Pawn attack sets, shifted exactly as MoveGenerator::GeneratePawnCaptures
	// does. The file mask drops the pawns that would wrap around the board edge:
	// a pawn on the h-file has no "right" capture. Squares are included whether or
	// not anything stands on them -- this is the set a pawn COVERS, which is what
	// makes a square unusable for an enemy piece, not the set it can capture on.
	const BITBOARD whitePawnsBb = boardsSpan[ePiece::WHITE_PAWN];
	const BITBOARD blackPawnsBb = boardsSpan[ePiece::BLACK_PAWN];
	const BITBOARD whitePawnAttacks =
		(Bits::clearBits(whitePawnsBb, g_bbFileMask[eFileNames::RIGHT_FILE]) >> 7) |
		(Bits::clearBits(whitePawnsBb, g_bbFileMask[eFileNames::LEFT_FILE]) >> 9);
	const BITBOARD blackPawnAttacks =
		(Bits::clearBits(blackPawnsBb, g_bbFileMask[eFileNames::RIGHT_FILE]) << 9) |
		(Bits::clearBits(blackPawnsBb, g_bbFileMask[eFileNames::LEFT_FILE]) << 7);

	return EvalContext{
		.boards = boardsSpan,
		.all_pieces = boardsSpan[ALL_PIECES],
		.pawns = { whitePawnsBb, blackPawnsBb },
		.occupied = { boardsSpan[ePiece::ALL_WHITE_PIECES], boardsSpan[ePiece::ALL_BLACK_PIECES] },
		.pawn_attacks = { whitePawnAttacks, blackPawnAttacks },
		.king_sq = { whiteKingSq, blackKingSq },
		.material = { matScoreWhite, matScoreBlack },
		.phase = gamePhase,
		.mopup_active = { mopupActive[WHITE], mopupActive[BLACK] },
		// GetGameInfo() returns by value, but the whole-struct copy folds away here:
		// a dedicated single-field accessor measured identically (-2.01% vs -2.03%
		// nps against main), so it was not worth the extra Board API surface.
		.castling_rights = board.GetGameInfo().castlingRights,
	};
}

//
//	Evaluate() :
//	Description: Sums up the material value from both colors. Adds additional bonuses according to heuristics
//	Returns:	 The value of the player in turn subtracted the oppositions value
// FIXME:		 Evaluate does not know about Check Mate - this is strictly only an evaluation of the current position
//				 - this means that we miss the first (and best, maybe even only?) opportunity to do check mate!
//
int EvalComplex::Evaluate(const Board& board) const noexcept
{
	const EvalContext ctx = BuildContext(board);

	// Interpolate each term at this position's phase and sum the results.
	// Material is not tapered and is added after — a piece is worth its value
	// regardless of how far along the game is; only positional judgements shift.
	//
	// Blended PER TERM rather than once over the accumulated pair, which is a
	// deliberate departure from D2 in the plan file. Integer division truncates,
	// so BlendPhase(a) + BlendPhase(b) and BlendPhase(a + b) can differ by up to
	// one centipawn per term. Blending once is marginally more accurate, but it
	// makes the per-term breakdown #129 prints unable to sum to the score it
	// reports — and that reconstructibility is an asserted invariant, not a
	// nicety. Only terms with mg != eg can truncate at all — eval_pawns and
	// eval_mopup set both endpoints equal, so they blend exactly — which bounds
	// the cost at under 1 cp per tapered term, i.e. under 2 cp per color today.
	// Deterministic, and far below anything this engine can measure; a
	// breakdown whose rows do not add up is a debugging tool that lies.
	int blended[2] = { 0, 0 };
	for (const eColor c : { WHITE, BLACK })
	{
		blended[c] = BlendPhase(eval_pawns(ctx, c),    ctx.phase)
		           + BlendPhase(eval_rooks(ctx, c),    ctx.phase)
		           + BlendPhase(eval_pst(ctx, c),      ctx.phase)
		           + BlendPhase(eval_mopup(ctx, c),    ctx.phase)
		           + BlendPhase(eval_bishops(ctx, c),  ctx.phase)
		           + BlendPhase(eval_castling(ctx, c), ctx.phase)
		           + BlendPhase(eval_mobility(ctx, c), ctx.phase);
	}

	const eColor color = board.GetCurrentColor();

	if (color == WHITE)
	{
		return (ctx.material[WHITE] + blended[WHITE]) - (ctx.material[BLACK] + blended[BLACK]);
	}
	return (ctx.material[BLACK] + blended[BLACK]) - (ctx.material[WHITE] + blended[WHITE]);
}

//
//	Breakdown() :
//	Description: Per-term introspection for the UCI 'eval' command (issue #129
//	             phase 2). Reports each term's contribution per color for one
//	             position. Read-only — no score changes, and search never calls
//	             this.
//	Returns:	 An EvalBreakdown whose rows come from the same BuildContext and
//	             the same term functions Evaluate() above calls.
//
// `total` is obtained by calling Evaluate(board) rather than re-applying its
// side-to-move sign flip here. That keeps the flip in exactly one place, so a
// future change to it cannot leave the breakdown reporting a total the search
// would disagree with. The cost is a second BuildContext + term pass per call
// — irrelevant at one call per interactive 'eval' command.
//
EvalBreakdown EvalComplex::Breakdown(const Board& board) const noexcept
{
	const EvalContext ctx = BuildContext(board);

	// Rows are reported BLENDED at this position's phase — i.e. the number each
	// term actually contributes to `total`, not its mg or eg endpoint. That is
	// what keeps the printed table summing to the score (the #129 honesty
	// invariant); the endpoints are visible in the term functions themselves.
	return EvalBreakdown{
		.material = { ctx.material[WHITE], ctx.material[BLACK] },
		.pawns    = { BlendPhase(eval_pawns(ctx, WHITE), ctx.phase), BlendPhase(eval_pawns(ctx, BLACK), ctx.phase) },
		.rooks    = { BlendPhase(eval_rooks(ctx, WHITE), ctx.phase), BlendPhase(eval_rooks(ctx, BLACK), ctx.phase) },
		.pst      = { BlendPhase(eval_pst(ctx, WHITE),   ctx.phase), BlendPhase(eval_pst(ctx, BLACK),   ctx.phase) },
		.mopup    = { BlendPhase(eval_mopup(ctx, WHITE), ctx.phase), BlendPhase(eval_mopup(ctx, BLACK), ctx.phase) },
		.bishops  = { BlendPhase(eval_bishops(ctx, WHITE), ctx.phase), BlendPhase(eval_bishops(ctx, BLACK), ctx.phase) },
		.castling = { BlendPhase(eval_castling(ctx, WHITE), ctx.phase), BlendPhase(eval_castling(ctx, BLACK), ctx.phase) },
		.mobility = { BlendPhase(eval_mobility(ctx, WHITE), ctx.phase), BlendPhase(eval_mobility(ctx, BLACK), ctx.phase) },
		.phase    = ctx.phase,
		.total    = Evaluate(board),
	};
}
