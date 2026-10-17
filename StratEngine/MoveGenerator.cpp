// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "MoveGenerator.h"
#include "Magic.h"
#include "Board.h"
#include "SquareHelper.h"
#include "PieceHelper.h"
#include "MoveFactory.h" // Internal factory helpers

namespace {
	// The mask a generator passes for "no restriction".
	constexpr BITBOARD kAllSquares = ~BITBOARD{0};

	// Squares strictly between `a` and `b` when they share a rank, file or diagonal; empty otherwise,
	// and empty for neighbours. Each slider ray stops at the other square, so the two rays meet only
	// between them: the other lines through `a` and `b` are parallel or cross at `a` or `b` themselves.
	BITBOARD Between(eSquare a, eSquare b) noexcept
	{
		if (File(a) == File(b) || Rank(a) == Rank(b))
			return RookAttacks(a, g_bbMask[b]) & RookAttacks(b, g_bbMask[a]);
		if (std::abs(File(a) - File(b)) == std::abs(Rank(a) - Rank(b)))
			return BishopAttacks(a, g_bbMask[b]) & BishopAttacks(b, g_bbMask[a]);
		return 0;
	}
} // namespace

// Generates every pseudo-legal move for the side to move from precomputed tables and bitwise
// operations. Whether a move leaves its own king in check is left to Board::DoMove().
void MoveGenerator::ComputeLegalMoves(const Board& board, MoveList& moveList)
{
	assert(moveList.empty());

	const auto color = board.GetCurrentColor();

	const auto boards = board.GetBitBoards();

	// Pawn captures (including en passant and capture-promotions), then promotions, then pushes.
	GeneratePawnCaptures(board, boards.data(), moveList, color, kAllSquares, kAllSquares, kAllSquares);
	AddPawnPromoteMoves(boards.data(), color, moveList, kAllSquares, kAllSquares);
	GeneratePawnNormalMoves(boards.data(), color, moveList, kAllSquares, kAllSquares);

	GenerateOfficerMoves(board, boards.data(), moveList, KNIGHT, color, kAllSquares, kAllSquares);
	GenerateOfficerMoves(board, boards.data(), moveList, BISHOP, color, kAllSquares, kAllSquares);
	GenerateOfficerMoves(board, boards.data(), moveList, ROOK, color, kAllSquares, kAllSquares);
	GenerateOfficerMoves(board, boards.data(), moveList, QUEEN, color, kAllSquares, kAllSquares);

	assert(boards[ePiece::BLACK_KING] && boards[ePiece::WHITE_KING]);

	GenerateOfficerMoves(board, boards.data(), moveList, KING, color, kAllSquares, kAllSquares);

	// Add any legal castling moves
	AddCastleMoves(board, moveList, color, boards.data());
}

// The ComputeLegalMoves() sequence with its moves masked down to the legal ones:
// - Two checkers: only the king can move.
// - One checker: other pieces must capture it or, against a slider, block between it and the king.
// - A pinned piece has no evasion. It may only move along its pin ray, which meets the checking ray
//   only at the king, and its pinner is blocked, so is not the checker. Pinned pieces are skipped.
// - The king may go where no enemy attacks once it has left its square. Taking the king out of the
//   occupancy is what stops a retreat along the checker's own line; masking the destination lets it
//   take an undefended checker.
// - En passant is the one move these masks cannot decide (two pawns leave one rank), so each
//   candidate is tested exactly.
// - Never castling.
void MoveGenerator::ComputeEvasions(const Board& board, MoveList& moveList)
{
	assert(moveList.empty());
	assert(board.InCheck());

	const auto color = board.GetCurrentColor();
	const auto boards = board.GetBitBoards();
	const BITBOARD* bb = boards.data();

	const eSquare king = Board::GetFirstPiece(bb[KING + static_cast<int>(color)]);
	const BITBOARD occ = bb[ALL_PIECES];
	const BITBOARD own = bb[ALL_FROM_COLOR + static_cast<int>(color)];
	const BITBOARD them = bb[ePiece::ALL_BLACK_PIECES - static_cast<int>(color)];

	BITBOARD kingTargets = 0;
	const BITBOARD occWithoutKing = occ & ~g_bbMask[king];
	for (BITBOARD squares = g_bbKingMoves[king] & ~own; squares; squares = Bits::clearLsb(squares)) {
		const eSquare to = Board::GetFirstPiece(squares);
		if (!(AttackersTo(bb, to, occWithoutKing) & them & ~g_bbMask[to]))
			kingTargets |= g_bbMask[to];
	}

	const BITBOARD checkers = AttackersTo(bb, king, occ) & them;
	assert(checkers);
	if (Bits::clearLsb(checkers)) {
		GenerateOfficerMoves(board, bb, moveList, KING, color, kAllSquares, kingTargets);
		return;
	}

	const eSquare checker = Board::GetFirstPiece(checkers);
	const BITBOARD blocks = Between(king, checker);
	const BITBOARD targets = checkers | blocks;

	// Enemy sliders that see the king through our own pieces; one own piece between makes it pinned.
	const BITBOARD queens = bb[ePiece::WHITE_QUEEN] | bb[ePiece::BLACK_QUEEN];
	const BITBOARD rooksQueens = (bb[ePiece::WHITE_ROOK] | bb[ePiece::BLACK_ROOK] | queens) & them;
	const BITBOARD bishopsQueens = (bb[ePiece::WHITE_BISHOP] | bb[ePiece::BLACK_BISHOP] | queens) & them;
	BITBOARD pinned = 0;
	for (BITBOARD snipers = (RookAttacks(king, them) & rooksQueens) | (BishopAttacks(king, them) & bishopsQueens);
	     snipers; snipers = Bits::clearLsb(snipers)) {
		const BITBOARD blockers = Between(king, Board::GetFirstPiece(snipers)) & occ;
		if (blockers && !Bits::clearLsb(blockers))
			pinned |= blockers & own;
	}
	const BITBOARD movers = ~pinned;

	// En passant resolves the check by removing the checking pawn or by landing between. Tested by
	// the position it leaves: AttackersTo reads attackers from the unchanged bitboards, so the
	// captured pawn is masked out of the result as well as the occupancy.
	BITBOARD epFrom = 0;
	if (board.ep_square() != NO_SQUARE) {
		const eSquare ep = board.ep_square();
		const eSquare captured = SquareHelper::PreviousRow(ep, color);
		if ((g_bbMask[captured] & checkers) || (g_bbMask[ep] & blocks)) {
			for (BITBOARD pawns = GetAnyEnPassantAttackingPawns(bb, color, ep) & movers; pawns;
			     pawns = Bits::clearLsb(pawns)) {
				const eSquare from = Board::GetFirstPiece(pawns);
				const BITBOARD occAfter = (occ & ~g_bbMask[from] & ~g_bbMask[captured]) | g_bbMask[ep];
				if (!(AttackersTo(bb, king, occAfter) & them & ~g_bbMask[captured]))
					epFrom |= g_bbMask[from];
			}
		}
	}

	GeneratePawnCaptures(board, bb, moveList, color, movers, targets, epFrom);
	AddPawnPromoteMoves(bb, color, moveList, movers, blocks);
	GeneratePawnNormalMoves(bb, color, moveList, movers, blocks);

	GenerateOfficerMoves(board, bb, moveList, KNIGHT, color, movers, targets);
	GenerateOfficerMoves(board, bb, moveList, BISHOP, color, movers, targets);
	GenerateOfficerMoves(board, bb, moveList, ROOK, color, movers, targets);
	GenerateOfficerMoves(board, bb, moveList, QUEEN, color, movers, targets);
	GenerateOfficerMoves(board, bb, moveList, KING, color, kAllSquares, kingTargets);
}

void MoveGenerator::GeneratePawnCaptures(const Board& board, const BITBOARD* const bbBitBoards, MoveList& moveList,
                                         eColor color, BITBOARD movers, BITBOARD targets, BITBOARD epFrom)
{
	const BITBOARD pawns = bbBitBoards[PieceHelper::AsPawn(color)] & movers;
	BITBOARD bbAttackRight = 0;
	BITBOARD bbAttackLeft = 0;
	// Every diagonal target, before masking to what can actually be captured.
	if (color == eColor::WHITE) {
		bbAttackRight = (Bits::clearBits(pawns, g_bbFileMask[eFileNames::RIGHT_FILE]) >> 7);
		bbAttackLeft = (Bits::clearBits(pawns, g_bbFileMask[eFileNames::LEFT_FILE]) >> 9);
	} else {
		bbAttackRight = (Bits::clearBits(pawns, g_bbFileMask[eFileNames::RIGHT_FILE]) << 9);
		bbAttackLeft = (Bits::clearBits(pawns, g_bbFileMask[eFileNames::LEFT_FILE]) << 7);
	}
	// Keep only target squares holding an enemy piece, plus the en-passant square if a pawn may take it.
	BITBOARD captureSquares = bbBitBoards[ePiece::ALL_BLACK_PIECES - static_cast<int>(color)] & targets;
	if (board.ep_square() != NO_SQUARE && epFrom)
		captureSquares |= g_bbMask[board.ep_square()];
	bbAttackLeft = Bits::applyMask(bbAttackLeft, captureSquares);
	bbAttackRight = Bits::applyMask(bbAttackRight, captureSquares);

	// Captures to the left: the pawn stands one rank behind and one file to the right.
	while (bbAttackLeft) {
		const eSquare to = Board::GetFirstPiece(bbAttackLeft);
		const eSquare from = static_cast<eSquare>(to + (color == eColor::BLACK ? -7 : 9));

		const Move temp = MoveFactory::MakeMove(from, to, MoveType::CAPTURE);
		AddPawnCaptures(board, moveList, bbBitBoards, temp, color, epFrom);

		bbAttackLeft = Bits::clearLsb(bbAttackLeft);
	}

	// Captures to the right: the pawn stands one rank behind and one file to the left.
	while (bbAttackRight) {
		const auto to = Board::GetFirstPiece(bbAttackRight);
		const auto from = static_cast<eSquare>(to + (color == eColor::BLACK ? -9 : 7));

		const Move temp = MoveFactory::MakeMove(from, to, MoveType::CAPTURE);
		AddPawnCaptures(board, moveList, bbBitBoards, temp, color, epFrom);
		bbAttackRight = Bits::clearLsb(bbAttackRight);
	}
}

void MoveGenerator::GeneratePawnNormalMoves(const BITBOARD* const bbBitBoards, eColor color, MoveList& moveList,
                                            BITBOARD movers, BITBOARD targets)
{
	const BITBOARD pawns = bbBitBoards[PieceHelper::AsPawn(color)] & movers;
	BITBOARD bbMoveOne = 0;
	BITBOARD bbMoveTwo = 0;
	// One step onto an empty square, excluding the promotion rank (AddPawnPromoteMoves owns it);
	// two steps from the starting rank, with both squares empty.
	if (color == eColor::WHITE) {
		bbMoveOne = Bits::clearBits((pawns >> ONE_ROW), bbBitBoards[ALL_PIECES]);
		bbMoveOne = Bits::clearBits(bbMoveOne, MASK_RANK_8);
		bbMoveTwo = Bits::clearBits(((pawns & MASK_RANK_2) >> TWO_ROWS), bbBitBoards[ALL_PIECES]);
		bbMoveTwo = Bits::clearBits(bbMoveTwo, (bbBitBoards[ALL_PIECES] >> ONE_ROW));
	} else {
		bbMoveOne = Bits::clearBits((pawns << ONE_ROW), bbBitBoards[ALL_PIECES]);
		bbMoveOne = Bits::clearBits(bbMoveOne, MASK_RANK_1);
		bbMoveTwo = Bits::clearBits(((pawns & MASK_RANK_7) << TWO_ROWS), bbBitBoards[ALL_PIECES]);
		bbMoveTwo = Bits::clearBits(bbMoveTwo, (bbBitBoards[ALL_PIECES] << ONE_ROW));
	}
	// A pawn standing one behind a target is its single pusher, and blocks any double push onto it, so
	// the from-square read below off the unmasked bitboard is never a pinned pawn's.
	bbMoveOne = Bits::setBits(bbMoveOne, bbMoveTwo) & targets;

	const ePiece movPiece = (color == eColor::BLACK) ? ePiece::BLACK_PAWN : ePiece::WHITE_PAWN;
	const int direction = (color == eColor::BLACK) ? -1 : 1;

	while (bbMoveOne) {
		const eSquare to = Board::GetFirstPiece(bbMoveOne);
		eSquare from = NO_SQUARE;
		MoveType moveType = MoveType::QUIET;
		// A pawn one row behind the target means a single push; otherwise it is a double push.
		if (Bits::isAnyBitSet(bbBitBoards[movPiece], g_bbMask[to + (ONE_ROW * direction)])) {
			from = static_cast<eSquare>(to + (ONE_ROW * direction));
		} else {
			from = static_cast<eSquare>(to + (TWO_ROWS * direction));
			moveType = MoveType::DOUBLE_PAWN_PUSH;
		}

		moveList.push(MoveFactory::MakeMove(from, to, moveType));

		bbMoveOne = Bits::clearLsb(bbMoveOne);
	}
}

void MoveGenerator::GenerateOfficerMoves(const Board& board, const BITBOARD* const bbBitBoards, MoveList& moveList,
                                         ePieceType piece, eColor color, BITBOARD movers, BITBOARD targets)
{
	const auto movPiece = PieceHelper::AsPiece(piece, color);

	BITBOARD bbPiecesToMove = bbBitBoards[movPiece] & movers;
	while (bbPiecesToMove) {
		const auto from = Board::GetFirstPiece(bbPiecesToMove);

		const BITBOARD bbAttack = GetOfficerAttackBoard(bbBitBoards, from, movPiece) & targets;

		AddOfficerMoves(board, moveList, bbAttack, from);

		bbPiecesToMove = Bits::clearLsb(bbPiecesToMove);
	}
}

// Squares a single non-pawn piece on `from` can move to, own pieces excluded.
BITBOARD MoveGenerator::GetOfficerAttackBoard(const BITBOARD* bbBitBoards, eSquare from, ePiece piece) noexcept
{
	const eColor color = PieceHelper::Color(piece);
	switch (piece) {
	case ePiece::WHITE_KNIGHT:
	case ePiece::BLACK_KNIGHT:
		return Bits::clearBits(g_bbKnightMoves[from], bbBitBoards[ALL_FROM_COLOR + static_cast<int>(color)]);
	case ePiece::WHITE_BISHOP:
	case ePiece::BLACK_BISHOP:
		return GetBishopBitboard(bbBitBoards, from, color);
	case ePiece::WHITE_ROOK:
	case ePiece::BLACK_ROOK:
		return GetRookBitboard(bbBitBoards, from, color);
	case ePiece::WHITE_QUEEN:
	case ePiece::BLACK_QUEEN:
		// A queen moves as a rook and a bishop from the same square.
		return GetBishopBitboard(bbBitBoards, from, color) | GetRookBitboard(bbBitBoards, from, color);
	case ePiece::WHITE_KING:
	case ePiece::BLACK_KING:
		return Bits::clearBits(g_bbKingMoves[from], bbBitBoards[ALL_FROM_COLOR + static_cast<int>(color)]);
	case ePiece::WHITE_PAWN:
	case ePiece::BLACK_PAWN:
	default:
		assert(!"Invalid call on GetOfficerAttackBoard");
		return 0;
	}
}

// Adds every non-capturing pawn promotion. Capture-promotions are added by AddPawnCaptures().
void MoveGenerator::AddPawnPromoteMoves(const BITBOARD* bbBitBoards, eColor color, MoveList& moveList, BITBOARD movers,
                                        BITBOARD targets)
{
	const BITBOARD pawns = bbBitBoards[PieceHelper::AsPawn(color)] & movers;
	BITBOARD bbAttack = 0;

	if (color == BLACK) {
		// Black pawn promotion options: pawns on 7th rank with no pieces in front of them
		bbAttack = Bits::clearBits(((pawns << ONE_ROW) & MASK_RANK_1), bbBitBoards[ALL_PIECES]);
	} else {
		bbAttack = Bits::clearBits(((pawns >> ONE_ROW) & MASK_RANK_8), bbBitBoards[ALL_PIECES]);
	}
	bbAttack &= targets;

	while (bbAttack) {
		const eSquare to = Board::GetFirstPiece(bbAttack);
		const eSquare from = SquareHelper::PreviousRow(to, color);

		moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(QUEEN, color)));
		moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(ROOK, color)));
		moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(BISHOP, color)));
		moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(KNIGHT, color)));

		bbAttack = Bits::clearLsb(bbAttack);
	}
}

// Generates captures, en-passant captures and promotions only, unsorted.
void MoveGenerator::ComputeCaptures(const Board& board, MoveList& moveList)
{
	const auto color = board.GetCurrentColor();

	const auto boards = board.GetBitBoards();

	const BITBOARD them = boards[ePiece::ALL_BLACK_PIECES - static_cast<int>(color)];

	GeneratePawnCaptures(board, boards.data(), moveList, color, kAllSquares, kAllSquares, kAllSquares);
	AddPawnPromoteMoves(boards.data(), color, moveList, kAllSquares, kAllSquares);

	GenerateOfficerMoves(board, boards.data(), moveList, KNIGHT, color, kAllSquares, them);
	GenerateOfficerMoves(board, boards.data(), moveList, BISHOP, color, kAllSquares, them);
	GenerateOfficerMoves(board, boards.data(), moveList, ROOK, color, kAllSquares, them);
	GenerateOfficerMoves(board, boards.data(), moveList, QUEEN, color, kAllSquares, them);
	GenerateOfficerMoves(board, boards.data(), moveList, KING, color, kAllSquares, them);
}

void MoveGenerator::ComputeQuiets(const Board& board, MoveList& moveList)
{
	const auto color = board.GetCurrentColor();

	const auto boards = board.GetBitBoards();

	const BITBOARD empty = ~boards[ALL_PIECES];

	GeneratePawnNormalMoves(boards.data(), color, moveList, kAllSquares, kAllSquares);

	GenerateOfficerMoves(board, boards.data(), moveList, KNIGHT, color, kAllSquares, empty);
	GenerateOfficerMoves(board, boards.data(), moveList, BISHOP, color, kAllSquares, empty);
	GenerateOfficerMoves(board, boards.data(), moveList, ROOK, color, kAllSquares, empty);
	GenerateOfficerMoves(board, boards.data(), moveList, QUEEN, color, kAllSquares, empty);
	GenerateOfficerMoves(board, boards.data(), moveList, KING, color, kAllSquares, empty);

	AddCastleMoves(board, moveList, color, boards.data());
}

// Adds all moves in the given attack bitboard to the move list
void MoveGenerator::AddOfficerMoves(const Board& board, MoveList& moveList, BITBOARD bbAttack, eSquare from)
{
	assert(from != NO_SQUARE);

	while (bbAttack) {
		const auto to = Board::GetFirstPiece(bbAttack);
		const bool isCapture = PieceHelper::IsActual(board.GetPiece(to));
		const MoveType moveType = isCapture ? MoveType::CAPTURE : MoveType::QUIET;
		// MoveType flag encodes whether it's a capture; captured piece is retrieved from the board when needed.
		moveList.push(MoveFactory::MakeMove(from, to, moveType));

		bbAttack = Bits::clearLsb(bbAttack);
	}
}

// Castling requires: the right is still held (king and that rook have not moved), the squares
// between king and rook are empty, and the king does not start on, pass through or land on an
// attacked square.
void MoveGenerator::AddCastleMoves(const Board& board, MoveList& moveList, eColor color, const BITBOARD* bbBitBoards)
{
	// Per-side castling layout:
	//   kingPiece, kingSq, enemyColor,
	//   kingsideFlag,  kingsideTarget, kingsideTransitMask,  kingsideAttackMask,
	//   queensideFlag, queensideTarget, queensideTransitMask, queensideAttackMask
	struct CastlingSide {
		ePiece kingPiece;
		eSquare kingSq;
		eColor enemyColor;

		uint8_t kingsideFlag;
		eSquare kingsideTarget;
		BITBOARD kingsideTransitMask; // must be empty  (f, g)
		BITBOARD kingsideAttackMask;  // must not be attacked (e, f, g)

		uint8_t queensideFlag;
		eSquare queensideTarget;
		BITBOARD queensideTransitMask; // must be empty  (d, c, b)
		BITBOARD queensideAttackMask;  // must not be attacked (e, d, c)
	};

	static constexpr std::array<CastlingSide, 2> sides = {
	    {{ePiece::WHITE_KING, e1, eColor::BLACK, CastlingRights::WHITE_KINGSIDE, g1, g_bbMask[f1] | g_bbMask[g1],
	      g_bbMask[e1] | g_bbMask[f1] | g_bbMask[g1], CastlingRights::WHITE_QUEENSIDE, c1,
	      g_bbMask[d1] | g_bbMask[c1] | g_bbMask[b1], g_bbMask[e1] | g_bbMask[d1] | g_bbMask[c1]},
	     {ePiece::BLACK_KING, e8, eColor::WHITE, CastlingRights::BLACK_KINGSIDE, g8, g_bbMask[f8] | g_bbMask[g8],
	      g_bbMask[e8] | g_bbMask[f8] | g_bbMask[g8], CastlingRights::BLACK_QUEENSIDE, c8,
	      g_bbMask[d8] | g_bbMask[c8] | g_bbMask[b8], g_bbMask[e8] | g_bbMask[d8] | g_bbMask[c8]}}};

	const auto& side = sides[static_cast<int>(color)];

	// Early exit if neither right is available for this side
	if (!(board.castling_rights() & (side.kingsideFlag | side.queensideFlag)))
		return;

	const eSquare sqFrom = Board::GetFirstPiece(bbBitBoards[KING + static_cast<int>(color)]);

	// A castling right implies the king is still on its starting square.
	assert(sqFrom == side.kingSq);
	assert(board.GetPiece(side.kingSq) == side.kingPiece);

	const auto attackColor = (color == eColor::WHITE ? eColor::BLACK : eColor::WHITE);
	const BITBOARD attackBoard = MoveGenerator::GetAttackBoard(board, attackColor);

	// Kingside
	if (board.castling_rights() & side.kingsideFlag) {
		if (!Bits::isAnyBitSet(attackBoard, side.kingsideAttackMask) && !board.IsOccupied(side.kingsideTransitMask)) {
			moveList.push(MoveFactory::MakeMove(sqFrom, side.kingsideTarget, MoveType::KING_CASTLE));
		}
	}

	// Queenside
	if (board.castling_rights() & side.queensideFlag) {
		if (!Bits::isAnyBitSet(attackBoard, side.queensideAttackMask) && !board.IsOccupied(side.queensideTransitMask)) {
			moveList.push(MoveFactory::MakeMove(sqFrom, side.queensideTarget, MoveType::QUEEN_CASTLE));
		}
	}
}

// Adds a pawn capture (including en passant and capture-promotions). `color` is the moving
// pawn's colour, passed explicitly because Move does not store the moving piece.
void MoveGenerator::AddPawnCaptures([[maybe_unused]] const Board& board, MoveList& moveList,
                                    const BITBOARD* bbBitBoards, Move move, eColor color, BITBOARD epFrom)
{
	// Prerequisites: Move must be a pawn capture move (including en-passant).
	// From and To must be set; the pawn of 'color' must be on from.
	assert(PieceHelper::IsPawn(board.GetPiece(move.from())));
	assert(!move.is_null());

	const eSquare from = move.from();
	const eSquare to = move.to();

	// The moving pawn must be on the bitboard square
	assert(Bits::isAnyBitSet(bbBitBoards[color], g_bbMask[from]));

	// Normal capture?
	if (IsEnemyPieceOnTarget(bbBitBoards, color, move)) {
		assert(PieceHelper::IsActual(board.GetPiece(to)));
		assert(PieceHelper::Color(board.GetPiece(to)) != color);

		if (!IsAnyBackRow(to)) {
			moveList.push(MoveFactory::MakeCapture(from, to));
		} else {
			// Capture-promotion: all four pieces, as PROMOTION_*_CAPTURE types.
			moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(QUEEN, color), true));
			moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(ROOK, color), true));
			moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(BISHOP, color), true));
			moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(KNIGHT, color), true));
		}
	}
	// Otherwise an en-passant capture, emitted only if `epFrom` allows this pawn
	else if (Bits::isAnyBitSet(epFrom, g_bbMask[from])) {
		[[maybe_unused]] const eSquare epWhere = SquareHelper::PreviousRow(to, color);
		assert(Bits::isAnyBitSet(bbBitBoards[BLACK - color], g_bbMask[epWhere])); // There must be an opponent pawn here
		moveList.push(MoveFactory::MakeEnPassant(from, to));
	}
}

// Rook moves from `from`, own pieces excluded.
BITBOARD MoveGenerator::GetRookBitboard(const BITBOARD* bbBitBoards, eSquare from, eColor color) noexcept
{
	const BITBOARD bbAttack = RookAttacks(from, bbBitBoards[ALL_PIECES]);
	return Bits::clearBits(bbAttack, bbBitBoards[ALL_FROM_COLOR + static_cast<int>(color)]);
}

// Bishop moves from `from`, own pieces excluded.
BITBOARD MoveGenerator::GetBishopBitboard(const BITBOARD* bbBitBoards, eSquare from, eColor color) noexcept
{
	const BITBOARD bbAttack = BishopAttacks(from, bbBitBoards[ALL_PIECES]);
	return Bits::clearBits(bbAttack, bbBitBoards[ALL_FROM_COLOR + static_cast<int>(color)]);
}

// Attackers of one square, against a supplied occupancy — the query SEE needs and the one
// GetAttackBoard cannot answer: that builds a whole-side attack board and says nothing about
// which piece attacks what.
//
// The pawn terms are GeneratePawnCaptures run backwards. A white pawn on `p` attacks `p - 7`
// (when p is not on the H file) and `p - 9` (when p is not on the A file), so the pawns attacking
// `square` sit at `square + 7` and `square + 9`, and the file guards move onto `square`: the
// first source exists only when `square` is off the A file, the second only when it is off the H
// file. Black mirrors it.
BITBOARD MoveGenerator::AttackersTo(const BITBOARD* bbBitBoards, eSquare square, BITBOARD occupancy) noexcept
{
	const BITBOARD target = g_bbMask[square];

	const BITBOARD pawnAttackers = ((((target & ~g_bbFileMask[eFileNames::LEFT_FILE]) << 7) |
	                                 ((target & ~g_bbFileMask[eFileNames::RIGHT_FILE]) << 9)) &
	                                bbBitBoards[ePiece::WHITE_PAWN]) |
	                               ((((target & ~g_bbFileMask[eFileNames::LEFT_FILE]) >> 9) |
	                                 ((target & ~g_bbFileMask[eFileNames::RIGHT_FILE]) >> 7)) &
	                                bbBitBoards[ePiece::BLACK_PAWN]);

	const BITBOARD bishopsAndQueens = bbBitBoards[ePiece::WHITE_BISHOP] | bbBitBoards[ePiece::BLACK_BISHOP] |
	                                  bbBitBoards[ePiece::WHITE_QUEEN] | bbBitBoards[ePiece::BLACK_QUEEN];
	const BITBOARD rooksAndQueens = bbBitBoards[ePiece::WHITE_ROOK] | bbBitBoards[ePiece::BLACK_ROOK] |
	                                bbBitBoards[ePiece::WHITE_QUEEN] | bbBitBoards[ePiece::BLACK_QUEEN];

	return pawnAttackers |
	       (g_bbKnightMoves[square] & (bbBitBoards[ePiece::WHITE_KNIGHT] | bbBitBoards[ePiece::BLACK_KNIGHT])) |
	       (g_bbKingMoves[square] & (bbBitBoards[ePiece::WHITE_KING] | bbBitBoards[ePiece::BLACK_KING])) |
	       (BishopAttacks(square, occupancy) & bishopsAndQueens) | (RookAttacks(square, occupancy) & rooksAndQueens);
}

// Every square `attackByColor` attacks (own pieces excluded except under pawn diagonals); says
// nothing about which piece attacks a given square (AttackersTo does). Includes the en-passant
// square when a pawn can take it.
BITBOARD MoveGenerator::GetAttackBoard(const Board& board, eColor attackByColor) noexcept
{
	const auto boards = board.GetBitBoards();

	BITBOARD bbAttackBoard = 0;
	const BITBOARD bbOwnPieces = boards[ALL_FROM_COLOR + static_cast<int>(attackByColor)];

	if (attackByColor == WHITE) {
		bbAttackBoard = (((boards[ePiece::WHITE_PAWN] & ~(g_bbFileMask[eFileNames::RIGHT_FILE])) >> 7) |
		                 ((boards[ePiece::WHITE_PAWN] & ~(g_bbFileMask[eFileNames::LEFT_FILE])) >> 9));
	} else {
		bbAttackBoard = (((boards[ePiece::BLACK_PAWN] & ~(g_bbFileMask[eFileNames::RIGHT_FILE])) << 9) |
		                 ((boards[ePiece::BLACK_PAWN] & ~(g_bbFileMask[eFileNames::LEFT_FILE])) << 7));
	}

	// Add en passant target if it exists
	const auto epSquare = board.ep_square();
	if (epSquare != NO_SQUARE) {
		// Check if attacking color has a pawn that can capture en passant
		const BITBOARD adjacentPawns = GetAnyEnPassantAttackingPawns(boards.data(), attackByColor, epSquare);
		if (adjacentPawns) {
			bbAttackBoard |= g_bbMask[epSquare];
		}
	}

	auto iFrom = Board::GetFirstPiece(boards[KING + static_cast<int>(attackByColor)]);
	bbAttackBoard |= g_bbKingMoves[iFrom] & ~bbOwnPieces;

	BITBOARD bbPiecesToMove = boards[KNIGHT + static_cast<int>(attackByColor)];

	while (bbPiecesToMove) {
		iFrom = Board::GetFirstPiece(bbPiecesToMove);
		bbAttackBoard |= g_bbKnightMoves[iFrom] & ~bbOwnPieces;
		bbPiecesToMove = Bits::clearLsb(bbPiecesToMove);
	}

	// Rooks and queens along ranks and files.
	bbPiecesToMove =
	    Bits::setBits(boards[ROOK + static_cast<int>(attackByColor)], boards[QUEEN + static_cast<int>(attackByColor)]);

	while (bbPiecesToMove) {
		iFrom = Board::GetFirstPiece(bbPiecesToMove);
		bbAttackBoard |= GetRookBitboard(boards.data(), iFrom, attackByColor);

		bbPiecesToMove = Bits::clearLsb(bbPiecesToMove);
	}

	// Bishops and queens along diagonals.
	bbPiecesToMove = Bits::setBits(boards[BISHOP + static_cast<int>(attackByColor)],
	                               boards[QUEEN + static_cast<int>(attackByColor)]);

	while (bbPiecesToMove) {
		iFrom = Board::GetFirstPiece(bbPiecesToMove);
		bbAttackBoard |= GetBishopBitboard(boards.data(), iFrom, attackByColor);

		bbPiecesToMove = Bits::clearLsb(bbPiecesToMove);
	}

	return bbAttackBoard;
}

// Pawns of `attackByColor` that could capture en passant onto `epSquare`: those beside the enemy
// pawn, which stands one rank behind the en-passant square.
BITBOARD MoveGenerator::GetAnyEnPassantAttackingPawns(const BITBOARD* boards, eColor attackByColor,
                                                      eSquare epSquare) noexcept
{
	if (epSquare == NO_SQUARE) {
		return 0;
	}

	const ePiece attackingPawn = PieceHelper::AsPawn(attackByColor);
	const eSquare enemyPawnSquare = SquareHelper::PreviousRow(epSquare, attackByColor);

	BITBOARD adjacentSquares = 0;

	const int enemyFile = File(enemyPawnSquare);

	if (enemyFile > eFileNames::LEFT_FILE)
		adjacentSquares |= g_bbMask[enemyPawnSquare - 1];
	if (enemyFile < eFileNames::RIGHT_FILE)
		adjacentSquares |= g_bbMask[enemyPawnSquare + 1];

	return adjacentSquares & boards[attackingPawn];
}