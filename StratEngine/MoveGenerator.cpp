// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"
#include "MoveGenerator.h"
#include "Magic.h"
#include "Board.h"
#include "SquareHelper.h"
#include "PieceHelper.h"
#include "MoveFactory.h" // Internal factory helpers

/*
 ----------------------------------------------------------
|                                                          |
|	ComputeLegalMoves laver en liste med alle tilladte	   |
|	traek ud fra det givne braet. Dog undersoeges det ikke |
|	om den nye stilling efterlader kongen i skak.		   |
|	Den benytter i hoej grad de forudberegnede globale	   |
|	tabeller til udregningen, hvorfor traekkene kan findes |
|	ved en kombination af tabelopslag og bitvise  		   |
|	operationer.										   |
|                                                          |
 ----------------------------------------------------------
*/
void MoveGenerator::ComputeLegalMoves(const Board& board, MoveList& moveList)
{
	assert(moveList.empty()); // Check our preconditions ;-)

	const auto color = board.GetCurrentColor();

	const auto boards = board.GetBitBoards();

	/* Pawn moves
	---------------------*/

	//Captures (including en-passant and promotion-captures)
	GeneratePawnCaptures(board, boards.data(), moveList, color);

	//Then normal Promotes
	AddPawnPromoteMoves(boards.data(), color, moveList);

	// Now normal pawn moves
	GeneratePawnNormalMoves(boards.data(), color, moveList);

	/* Officer moves
	---------------------*/
	GenerateOfficerMoves(board, boards.data(), moveList, KNIGHT, color, false);
	GenerateOfficerMoves(board, boards.data(), moveList, BISHOP, color, false);
	GenerateOfficerMoves(board, boards.data(), moveList, ROOK, color, false);
	GenerateOfficerMoves(board, boards.data(), moveList, QUEEN, color, false);

	/* King moves
	-------------------*/
	// Need to have kings on the board
	assert(boards[ePiece::BLACK_KING] && boards[ePiece::WHITE_KING]);

	GenerateOfficerMoves(board, boards.data(), moveList, KING, color, false);

	// Add any legal castling moves
	AddCastleMoves(board, moveList, color, boards.data());
}

void MoveGenerator::GeneratePawnCaptures(const Board& board, const BITBOARD* const bbBitBoards, MoveList& moveList,
                                         eColor color)
{
	BITBOARD bbAttackRight = 0;
	BITBOARD bbAttackLeft = 0;
	if (color == eColor::WHITE) {
		// Laver i foerste omgang maalfelt-bitboards med alle venstre- og hoejreskraa traek for boenderne
		bbAttackRight = (Bits::clearBits(bbBitBoards[ePiece::WHITE_PAWN], g_bbFileMask[eFileNames::RIGHT_FILE]) >> 7);
		bbAttackLeft = (Bits::clearBits(bbBitBoards[ePiece::WHITE_PAWN], g_bbFileMask[eFileNames::LEFT_FILE]) >> 9);
	} else {
		// Laver i foerste omgang maalfelt-bitboards med alle venstre- og hoejreskraa traek for boenderne
		bbAttackRight = (Bits::clearBits(bbBitBoards[ePiece::BLACK_PAWN], g_bbFileMask[eFileNames::RIGHT_FILE]) << 9);
		bbAttackLeft = (Bits::clearBits(bbBitBoards[ePiece::BLACK_PAWN], g_bbFileMask[eFileNames::LEFT_FILE]) << 7);
	}
	// Reducerer bitboardet til felter, hvor modstanderens brikker staar
	// Er der et en-passant felt, medtages det ogsaa
	if (board.ep_square() != NO_SQUARE) {
		bbAttackLeft = Bits::applyMask(bbAttackLeft, bbBitBoards[ePiece::ALL_BLACK_PIECES - static_cast<int>(color)] |
		                                                 g_bbMask[board.ep_square()]);
		bbAttackRight = Bits::applyMask(bbAttackRight, bbBitBoards[ePiece::ALL_BLACK_PIECES - static_cast<int>(color)] |
		                                                   g_bbMask[board.ep_square()]);
	} else {
		bbAttackLeft = Bits::applyMask(bbAttackLeft, bbBitBoards[ePiece::ALL_BLACK_PIECES - static_cast<int>(color)]);
		bbAttackRight = Bits::applyMask(bbAttackRight, bbBitBoards[ePiece::ALL_BLACK_PIECES - static_cast<int>(color)]);
	}

	// Foerst tager vi slagene to the left
	while (bbAttackLeft) {
		// Find first field
		const eSquare to = Board::GetFirstPiece(bbAttackLeft);
		// Bonden kom fra op-og-til-hoejre
		const eSquare from =
		    static_cast<eSquare>(to + (color == eColor::BLACK ? -7 : 9)); //FIXME: Add defines, constants whatever

		// Construct a call-owned move and forward to AddPawnCaptures
		const Move temp = MoveFactory::MakeMove(from, to, MoveType::CAPTURE);
		AddPawnCaptures(board, moveList, bbBitBoards, temp, color);

		bbAttackLeft = Bits::clearLsb(bbAttackLeft);
	}

	// Then we take the captures to the right
	while (bbAttackRight) {
		// Find first field
		auto to = Board::GetFirstPiece(bbAttackRight);

		// Bonden kom fra op-og-til-venstre
		auto from =
		    static_cast<eSquare>(to + (color == eColor::BLACK ? -9 : 7)); //FIXME: Add defines, constants whatever

		const Move temp = MoveFactory::MakeMove(from, to, MoveType::CAPTURE);
		AddPawnCaptures(board, moveList, bbBitBoards, temp, color);
		bbAttackRight = Bits::clearLsb(bbAttackRight);
	}
}

void MoveGenerator::GeneratePawnNormalMoves(const BITBOARD* const bbBitBoards, eColor color, MoveList& moveList)
{
	BITBOARD bbMoveOne = 0;
	BITBOARD bbMoveTwo = 0;
	if (color == eColor::WHITE) {
		// Et skridt frem
		bbMoveOne = Bits::clearBits((bbBitBoards[ePiece::WHITE_PAWN] >> ONE_ROW), bbBitBoards[ALL_PIECES]);
		// Sorterer sidste raekke fra
		bbMoveOne = Bits::clearBits(bbMoveOne, MASK_RANK_8);
		// To skridt frem - hvide boender kun fra startplacering, dvs spillets 2. raekke
		bbMoveTwo =
		    Bits::clearBits(((bbBitBoards[ePiece::WHITE_PAWN] & MASK_RANK_2) >> TWO_ROWS), bbBitBoards[ALL_PIECES]);
		// Sikrer os at midterfeltet ogsaa er frit
		bbMoveTwo = Bits::clearBits(bbMoveTwo, (bbBitBoards[ALL_PIECES] >> ONE_ROW));
	} else {
		// Et skridt frem - men kun hvor der ikke staar nogen foran
		bbMoveOne = Bits::clearBits((bbBitBoards[ePiece::BLACK_PAWN] << ONE_ROW), bbBitBoards[ALL_PIECES]);
		// Sorterer sidste raekke fra
		bbMoveOne = Bits::clearBits(bbMoveOne, MASK_RANK_1);
		// To skridt frem - sorte boender kun fra startplacering, dvs spillets 7. raekke
		bbMoveTwo =
		    Bits::clearBits(((bbBitBoards[ePiece::BLACK_PAWN] & MASK_RANK_7) << TWO_ROWS), bbBitBoards[ALL_PIECES]);
		// Sikrer os at midterfeltet ogsaa er frit
		bbMoveTwo = Bits::clearBits(bbMoveTwo, (bbBitBoards[ALL_PIECES] << ONE_ROW));
	}
	// Combine the two bitboards
	bbMoveOne = Bits::setBits(bbMoveOne, bbMoveTwo);

	const ePiece movPiece = (color == eColor::BLACK) ? ePiece::BLACK_PAWN : ePiece::WHITE_PAWN;
	const int direction = (color == eColor::BLACK) ? -1 : 1;

	// Looper indtil der ikke er flere maal-felter tilbage
	while (bbMoveOne) {
		const eSquare to = Board::GetFirstPiece(bbMoveOne);
		eSquare from = NO_SQUARE;
		MoveType moveType = MoveType::QUIET;
		// Do we have a Pawn one row behind the 'to' field? -> Normal move
		if (Bits::isAnyBitSet(bbBitBoards[movPiece], g_bbMask[to + (ONE_ROW * direction)])) {
			from = static_cast<eSquare>(to + (ONE_ROW * direction));
		} else {
			from = static_cast<eSquare>(to + (TWO_ROWS * direction));
			moveType = MoveType::DOUBLE_PAWN_PUSH;
		}

		moveList.push(MoveFactory::MakeMove(from, to, moveType));

		// Now clear it from our attack board
		bbMoveOne = Bits::clearLsb(bbMoveOne);
	}
}

void MoveGenerator::GenerateOfficerMoves(const Board& board, const BITBOARD* const bbBitBoards, MoveList& moveList,
                                         ePieceType piece, eColor color, bool onlyCaptures)
{
	const auto movPiece = PieceHelper::AsPiece(piece, color);

	BITBOARD bbPiecesToMove = bbBitBoards[movPiece];
	while (bbPiecesToMove) {
		const auto from = Board::GetFirstPiece(bbPiecesToMove);

		BITBOARD bbAttack = GetOfficerAttackBoard(bbBitBoards, from, movPiece);
		if (onlyCaptures) {
			// Keep only squares occupied by the opponent.
			bbAttack = Bits::applyMask(bbAttack, bbBitBoards[ePiece::ALL_BLACK_PIECES - static_cast<int>(color)]);
		}

		// Add all legal moves found above
		AddOfficerMoves(board, moveList, bbAttack, from);

		bbPiecesToMove = Bits::clearLsb(bbPiecesToMove);
	}
}

// Returns a Attackboard for a single piece (move.From.MovPiece)
// FIXME: This method is called a lot - maybe cache results per move generation?
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
		// Dronningen er i virkeligheden et taarn med en loeber ovenpaa hovedet ;-)
		// (dvs kan bevaege sig som baade et taarn og en loeber samtidigt (dog en ad gangen))
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

// <param name="bbBitBoards">Current board bitboards</param>
// <param name="moveList">Collection of found moves</param>
// Adds all possible pawn promotion moves to the move list
// Remark: Pawn Capture+Promotes are handled in AddPawnCaptureMoves()
void MoveGenerator::AddPawnPromoteMoves(const BITBOARD* bbBitBoards, eColor color, MoveList& moveList)
{
	BITBOARD bbAttack = 0;

	if (color == BLACK) {
		// Black pawn promotion options: pawns on 7th rank with no pieces in front of them
		bbAttack =
		    Bits::clearBits(((bbBitBoards[ePiece::BLACK_PAWN] << ONE_ROW) & MASK_RANK_1), bbBitBoards[ALL_PIECES]);
	} else {
		bbAttack =
		    Bits::clearBits(((bbBitBoards[ePiece::WHITE_PAWN] >> ONE_ROW) & MASK_RANK_8), bbBitBoards[ALL_PIECES]);
	}

	// Genererer traek ud fra maalfelt-bitboardet indtil det er tomt
	while (bbAttack) {
		const eSquare to = Board::GetFirstPiece(bbAttack);
		const eSquare from = SquareHelper::PreviousRow(to, color);

		// Add the four selections using factory
		moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(QUEEN, color)));
		moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(ROOK, color)));
		moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(BISHOP, color)));
		moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(KNIGHT, color)));

		bbAttack = Bits::clearLsb(bbAttack);
	}
}

// <param name="moveList">Collection of found moves</param>
// Computes all possible moves, filters so only all captures, en passants and promotes remains and returns unsorted
void MoveGenerator::ComputeCaptures(const Board& board, MoveList& moveList)
{
	const auto color = board.GetCurrentColor();

	const auto boards = board.GetBitBoards();

	/* Pawn moves
	---------------------*/

	//Captures (including en-passant and promotion-captures)
	GeneratePawnCaptures(board, boards.data(), moveList, color);

	//Then normal Promotes
	AddPawnPromoteMoves(boards.data(), color, moveList);

	/* Officer moves
	---------------------*/
	// GetAttackBoard for opposite color
	GenerateOfficerMoves(board, boards.data(), moveList, KNIGHT, color, true);
	GenerateOfficerMoves(board, boards.data(), moveList, BISHOP, color, true);
	GenerateOfficerMoves(board, boards.data(), moveList, ROOK, color, true);
	GenerateOfficerMoves(board, boards.data(), moveList, QUEEN, color, true);
	GenerateOfficerMoves(board, boards.data(), moveList, KING, color, true);
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

		bbAttack = Bits::clearLsb(bbAttack); // Done, clear this square
	}
}

/*
*	To be able to make castling, some constraints must be taken into considerations
*	1) The King or the Rook cannot have moved earlier in the game ** this could pose a problem by Chaining UndoMove()'s
*		- but one of the Rooks _can_ move as long the Castling is the other way
*	2) No other pieces must stand in between them in the move
*	3) The King must not move away from, pass a or move into a check
*		- i.e. K  .  .  R
*			   e1 f1 g1 h1
*		neither e1, f1 or g1 must be threatened
*/
// TODO: Det er vist overkill at teste om alle felterne Kongen flytter forbi er angrebet for hver eneste traekgenerering
// i stedet kunne man flytte det til DoMove()
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

	const eSquare sqFrom = Board::GetFirstPiece(
	    bbBitBoards[static_cast<ePiece>(KING + static_cast<int>(color))]); // There is only one king!!

	// Debug: king must be on its starting square if any castling right is still set
	assert(sqFrom == side.kingSq);
	assert(board.GetPiece(side.kingSq) == side.kingPiece);

	const auto attackColor = (color == eColor::WHITE ? eColor::BLACK : eColor::WHITE);
	const BITBOARD attackBoard = MoveGenerator::GetAttackBoard(board, attackColor);

	// Kingside
	if (board.castling_rights() & side.kingsideFlag) {
		// Are these squares being attacked ? The king must not move away from, pass or move into a check!
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

// Bemaerk: color er for bonden i traekket
// Remarks: Move must be a pawn capture move (including en-passant).
// Also handles promotion captures.
// color: the color of the moving pawn (passed explicitly; the moving piece is not stored in Move).
void MoveGenerator::AddPawnCaptures([[maybe_unused]] const Board& board, MoveList& moveList,
                                    const BITBOARD* bbBitBoards, Move move, eColor color)
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

		// Promotion capture, too?
		if (!IsAnyBackRow(to)) // Nope, normal capture - all done
		{
			moveList.push(MoveFactory::MakeCapture(from, to));
		} else { // Promotion Captures — add each of the 4 promotions (isCapture=true selects PROMOTION_*_CAPTURE type)
			moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(QUEEN, color), true));
			moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(ROOK, color), true));
			moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(BISHOP, color), true));
			moveList.push(MoveFactory::MakePromotion(from, to, PieceHelper::AsPiece(KNIGHT, color), true));
		}
	}
	// Otherwise it must be an en-passant capture
	else {
		[[maybe_unused]] const eSquare epWhere = SquareHelper::PreviousRow(to, color);
		assert(Bits::isAnyBitSet(bbBitBoards[BLACK - color], g_bbMask[epWhere])); // There must be an opponent pawn here
		moveList.push(MoveFactory::MakeEnPassant(from, to));
	}
}

//***************************************
// Method:      GetRookBitboard
// Description:
// FullName:    private MoveGenerator::GetRookBitboard const
// Returns:     BITBOARD -
// Parameter:   const BITBOARD* bbBitBoards -
//			:   const Square& from -
//			:   eColor color -
//***************************************
BITBOARD MoveGenerator::GetRookBitboard(const BITBOARD* bbBitBoards, eSquare from, eColor color) noexcept
{
	const BITBOARD bbAttack = RookAttacks(from, bbBitBoards[ALL_PIECES]);
	return Bits::clearBits(bbAttack, bbBitBoards[ALL_FROM_COLOR + static_cast<int>(color)]);
}

//***************************************
// Method:      GetBishopBitboard
// Description:
// FullName:    private MoveGenerator::GetBishopBitboard const
// Returns:     BITBOARD -
// Parameter:   const BITBOARD* bbBitBoards -
//			:   const Square& from -
//			:   eColor color -
//***************************************
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

//***************************************
// Method:      GetAttackBoard
// Description: Generates all possible CAPTURE type moves for the color and combines the attacked squares into the returned BTBOARD.
//				Does NOT tell which is attacking a given square.
// FullName:    public MoveGenerator::GetAttackBoard const
// Returns:     BITBOARD - The generated Attackboard
// Parameter:   eColor attackByColor - color to generate attack bitboard for
// Remark:      NOTE: This does not consider EN PASSANT type moves as this is not needed currently
//				Parts of the code is DUPLICATED from MoveGen. Consider reusing instead.
//***************************************
BITBOARD MoveGenerator::GetAttackBoard(const Board& board, eColor attackByColor) noexcept
{
	const auto boards = board.GetBitBoards();

	BITBOARD bbAttackBoard = 0; // Attacked squares bitboard
	const BITBOARD bbOwnPieces =
	    boards[ALL_FROM_COLOR +
	           static_cast<int>(attackByColor)]; // Current position of own pieces - used to mask out illegal moves

	/* Pawns */
	if (attackByColor == WHITE) {
		// Target bitboard with normal CAPTURES for pawns
		bbAttackBoard = (((boards[ePiece::WHITE_PAWN] & ~(g_bbFileMask[eFileNames::RIGHT_FILE])) >> 7) |
		                 ((boards[ePiece::WHITE_PAWN] & ~(g_bbFileMask[eFileNames::LEFT_FILE])) >> 9));
	} else {
		// Laver i foerste omgang et maalfelt-bitboard med alle skraa traek for boenderne
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

	// Saa er det officerer og kongen
	//---------------------------------

	/* Kongen */

	auto iFrom = Board::GetFirstPiece(boards[KING + static_cast<int>(attackByColor)]);
	bbAttackBoard |= g_bbKingMoves[iFrom] & ~bbOwnPieces;

	/* Springerne */

	BITBOARD bbPiecesToMove = boards[KNIGHT + static_cast<int>(attackByColor)];

	while (bbPiecesToMove) {
		iFrom = Board::GetFirstPiece(bbPiecesToMove);
		bbAttackBoard |= g_bbKnightMoves[iFrom] & ~bbOwnPieces;
		bbPiecesToMove = Bits::clearLsb(bbPiecesToMove);
	}

	/* Traek for taarnene (og dronninger)
	--------------------*/

	bbPiecesToMove =
	    Bits::setBits(boards[ROOK + static_cast<int>(attackByColor)], boards[QUEEN + static_cast<int>(attackByColor)]);

	while (bbPiecesToMove) {
		iFrom = Board::GetFirstPiece(bbPiecesToMove);
		// Faa mulige traek for dronninger og taarne
		bbAttackBoard |= GetRookBitboard(boards.data(), iFrom, attackByColor);

		bbPiecesToMove = Bits::clearLsb(bbPiecesToMove);
	}

	/* Traek for loeberne (og dronninger)
	--------------------*/

	bbPiecesToMove = Bits::setBits(boards[BISHOP + static_cast<int>(attackByColor)],
	                               boards[QUEEN + static_cast<int>(attackByColor)]);

	while (bbPiecesToMove) {
		iFrom = Board::GetFirstPiece(bbPiecesToMove);

		// Faa mulige traek for dronninger og loebere
		bbAttackBoard |= GetBishopBitboard(boards.data(), iFrom, attackByColor);

		bbPiecesToMove = Bits::clearLsb(bbPiecesToMove);
	}

	return bbAttackBoard;
}

//***************************************
// Method:      GetPawnsAdjacentToEnPassant
// Description: Returns pawns adjacent to enemy pawn (not ep square itself!)
// FullName:    private static MoveGenerator::GetPawnsAdjacentToEnPassant
// Returns:     BITBOARD - Bitboard with attacking pawns
// Parameter:   eColor attackByColor - Color of pawns that would capture
// Parameter:   eSquare epSquare - En passant target square
// Remark:      The actual enemy pawn is on same rank as attacker, not the ep square
//***************************************
BITBOARD MoveGenerator::GetAnyEnPassantAttackingPawns(const BITBOARD* boards, eColor attackByColor,
                                                      eSquare epSquare) noexcept
{
	if (epSquare == NO_SQUARE) {
		return 0;
	}

	const ePiece attackingPawn = PieceHelper::AsPawn(attackByColor);
	const eSquare enemyPawnSquare = SquareHelper::PreviousRow(epSquare, attackByColor);

	// Find pawns horizontally adjacent to the enemy pawn using pre-computed masks
	// Clear files we can't attack from (boundaries handled by mask intersection)
	BITBOARD adjacentSquares = 0;

	const int enemyFile = File(enemyPawnSquare);

	// Left adjacent (from our perspective)
	if (enemyFile > eFileNames::LEFT_FILE) {
		adjacentSquares |= g_bbMask[enemyPawnSquare - 1]; // One file to the left
	}

	// Right adjacent
	if (enemyFile < eFileNames::RIGHT_FILE) {
		adjacentSquares |= g_bbMask[enemyPawnSquare + 1]; // One file to the right
	}

	// Return only squares that actually have our pawns
	return adjacentSquares & boards[attackingPawn];
}