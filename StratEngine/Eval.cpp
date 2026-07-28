// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Eval.h"

#include "Board.h"

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
// TODO: Add bonus for connected Rooks!! (carried over verbatim from the
// pre-#127 switch; still open).
ScorePair EvalComplex::eval_rooks(const EvalContext& ctx, eColor color) noexcept
{
	int score = 0;
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

		// Bonus hvis taarnet er i syvende raekke sent i spillet
		if (rank == seventhRank && ctx.stage != PlayState::MIDDLEGAME)
			score += ROOK_ON_7TH_BONUS;

		// Bonus hvis der er aabne raekker til taarnet
		const BITBOARD ownForwardMask = (color == WHITE) ? g_bbFileUpMask[square] : g_bbFileDownMask[square];
		if (!(ownPawns & ownForwardMask))
		{
			score += HALF_OPEN_FILE;

			if (!(g_bbFileMask[file] & enemyPawns))
				score += OPEN_FILE - HALF_OPEN_FILE;
		}

		remaining = Bits::clearLsb(remaining);
	}

	return ScorePair{ score, score };
}

// eval_pst — piece-square-table contribution for one color's pieces (issue
// #127 restructure — see .claude/plans/eval-context-restructure.md — D5's
// per-piece-type bitboard loops, replacing an earlier mailbox-lookup
// version). Every non-king piece type gets its own bitboard loop, so
// GetPositionalScore is called with a statically-known piece — no
// board.GetPiece(square) mailbox lookup per square. The king is excluded
// from those loops and uses its own stage-selected table instead —
// g_Eval_Bitboards[5] (middlegame) or [6] (endgame) — so it still receives
// exactly one PST contribution, just from a different table than the rest
// (D2 in the plan file). Reordering these per-type loops relative to each
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

	// King PST: stage-selected table, not the generic one above (D2).
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
	const eSquare kingSq = ctx.king_sq[color];
	if (kingSq != NO_SQUARE)
	{
		const ePiece kingPiece = (color == WHITE) ? ePiece::WHITE_KING : ePiece::BLACK_KING;
		if (ctx.stage == PlayState::MIDDLEGAME)
		{
			score += g_Eval_Bitboards[5][getEvalBoard(kingPiece, kingSq)];
		}
		else
		{
			score += g_Eval_Bitboards[6][getEvalBoard(kingPiece, kingSq)];
		}
	}

	return ScorePair{ score, score };
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
	if (ctx.stage == PlayState::MIDDLEGAME ||
		ctx.pawns[WHITE] != 0ULL || ctx.pawns[BLACK] != 0ULL)
		return ScorePair{};

	// A kingless board (default-constructed or failed-parse Board only — see
	// EvalContext::king_sq) reaches this point whenever it also has no pawns:
	// stage is ENDGAME (material 0 <= 11500) and both pawn bitboards are
	// empty. Guarded explicitly rather than left to fall out of the
	// MOPUP_MATERIAL_THRESHOLD check below, which happens to also save it
	// today (absMatDiff == 0) but would not if that threshold ever changed.
	if (ctx.king_sq[WHITE] == NO_SQUARE || ctx.king_sq[BLACK] == NO_SQUARE)
		return ScorePair{};

	const int matDiff = ctx.material[WHITE] - ctx.material[BLACK];
	const int absMatDiff = (matDiff >= 0) ? matDiff : -matDiff;

	if (absMatDiff < MOPUP_MATERIAL_THRESHOLD)
		return ScorePair{};

	const eColor winner = (matDiff > 0) ? WHITE : BLACK;
	if (color != winner)
		return ScorePair{};

	const eColor loser = (winner == WHITE) ? BLACK : WHITE;
	const eSquare winnerKingSq = ctx.king_sq[winner];
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

	PlayState gameStage = PlayState::MIDDLEGAME;
	const int iMinScore = std::min(matScoreWhite, matScoreBlack);
	if (iMinScore <= 11500)		// TODO: King are worth 10000 - but maybe they shouldn't be counted??
		gameStage = PlayState::ENDGAME;

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
	const int rawPhase =
		PHASE_KNIGHT * (std::popcount(boardsSpan[ePiece::WHITE_KNIGHT]) + std::popcount(boardsSpan[ePiece::BLACK_KNIGHT])) +
		PHASE_BISHOP * (std::popcount(boardsSpan[ePiece::WHITE_BISHOP]) + std::popcount(boardsSpan[ePiece::BLACK_BISHOP])) +
		PHASE_ROOK   * (std::popcount(boardsSpan[ePiece::WHITE_ROOK])   + std::popcount(boardsSpan[ePiece::BLACK_ROOK]))   +
		PHASE_QUEEN  * (std::popcount(boardsSpan[ePiece::WHITE_QUEEN])  + std::popcount(boardsSpan[ePiece::BLACK_QUEEN]));
	const int gamePhase = (rawPhase > MAX_GAME_PHASE) ? MAX_GAME_PHASE : rawPhase;

	return EvalContext{
		.boards = boardsSpan,
		.all_pieces = boardsSpan[ALL_PIECES],
		.pawns = { boardsSpan[ePiece::WHITE_PAWN], boardsSpan[ePiece::BLACK_PAWN] },
		.occupied = { boardsSpan[ePiece::ALL_WHITE_PIECES], boardsSpan[ePiece::ALL_BLACK_PIECES] },
		.king_sq = { whiteKingSq, blackKingSq },
		.material = { matScoreWhite, matScoreBlack },
		.stage = gameStage,
		.phase = gamePhase,
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

	// Accumulate each color's phase-dependent terms, then interpolate ONCE per
	// color (D2, issue #99). Material is not tapered and is added after the
	// blend — a piece is worth its value regardless of how far along the game
	// is; only positional judgements shift with phase.
	ScorePair bonusScore[2];

	bonusScore[WHITE] += eval_pawns(ctx, WHITE);
	bonusScore[WHITE] += eval_rooks(ctx, WHITE);
	bonusScore[WHITE] += eval_pst(ctx, WHITE);
	bonusScore[WHITE] += eval_mopup(ctx, WHITE);

	bonusScore[BLACK] += eval_pawns(ctx, BLACK);
	bonusScore[BLACK] += eval_rooks(ctx, BLACK);
	bonusScore[BLACK] += eval_pst(ctx, BLACK);
	bonusScore[BLACK] += eval_mopup(ctx, BLACK);

	const int blended[2] = {
		BlendPhase(bonusScore[WHITE], ctx.phase),
		BlendPhase(bonusScore[BLACK], ctx.phase),
	};

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
//	             the same four term functions Evaluate() above calls.
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
		.stage    = ctx.stage,
		.total    = Evaluate(board),
	};
}
