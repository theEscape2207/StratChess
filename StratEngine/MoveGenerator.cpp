// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "StdAfx.h"

#include <cassert>

#include "MoveGenerator.h"
#include "Board.h"

#include "MoveHelper.h"
#include "PieceHelper.h"

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
void MoveGenerator::ComputeLegalMoves(_In_ const GameInfo& info, _Inout_ MoveList& moveList)
{
	assert(moveList.empty());		// Check our preconditions ;-)

	const eColor color = Board::Instance().GetCurrentColor();

	const auto boards = Board::Instance().GetBitBoards();

	/* Peasant moves
	---------------------*/

	//Captures (including en-passant and promotion-captures)
	GeneratePawnCaptures(boards.data(), info, moveList, color);

	//Then normal Promotes
	AddPawnPromoteMoves(boards.data(), moveList);

	// Now normal pawn moves
	GeneratePawnNormalMoves(boards.data(), color, moveList);

	/* Officer moves
	---------------------*/
	GenerateOfficerMoves(boards.data(), moveList, KNIGHT, false);
	GenerateOfficerMoves(boards.data(), moveList, BISHOP, false);
	GenerateOfficerMoves(boards.data(), moveList, ROOK, false);
	GenerateOfficerMoves(boards.data(), moveList, QUEEN, false);

	/* King moves
	-------------------*/
	// Maa selvfoelgelig ikke spille uden konge
	assert(boards[ePiece::BLACK_KING] && boards[ePiece::WHITE_KING]);

	GenerateOfficerMoves(boards.data(), moveList, KING, false);

	// Add any legal castling moves
	AddCastleMoves(moveList, color, boards.data(), info);
}

void MoveGenerator::GeneratePawnCaptures(const BITBOARD* const bbBitBoards, const GameInfo & info, MoveList & moveList, eColor color)
{
	BITBOARD bbAttackRight = 0;
	BITBOARD bbAttackLeft = 0;
	if (color == eColor::WHITE)
	{
		// Laver i foerste omgang maalfelt-bitboards med alle venstre- og hoejreskraa traek for boenderne
		bbAttackRight = (Bits::clearBits(bbBitBoards[ePiece::WHITE_PAWN], g_bbFileMask[eFileNames::RIGHT_FILE]) >> 7);
		bbAttackLeft = (Bits::clearBits(bbBitBoards[ePiece::WHITE_PAWN], g_bbFileMask[eFileNames::LEFT_FILE]) >> 9);
	}
	else
	{
		// Laver i foerste omgang maalfelt-bitboards med alle venstre- og hoejreskraa traek for boenderne
		bbAttackRight = (Bits::clearBits(bbBitBoards[ePiece::BLACK_PAWN], g_bbFileMask[eFileNames::RIGHT_FILE]) << 9);
		bbAttackLeft = (Bits::clearBits(bbBitBoards[ePiece::BLACK_PAWN], g_bbFileMask[eFileNames::LEFT_FILE]) << 7);
	}
	// Reducerer bitboardet til felter, hvor modstanderens brikker staar
	// Er der et en-passant felt, medtages det ogsaa
	if (info.epSquare != NO_SQUARE) {
		Bits::clearBitsExceptRef(bbAttackLeft, bbBitBoards[ePiece::ALL_BLACK_PIECES - color] | g_bbMask[info.epSquare]);
		Bits::clearBitsExceptRef(bbAttackRight, bbBitBoards[ePiece::ALL_BLACK_PIECES - color] | g_bbMask[info.epSquare]);
	}
	else {
		Bits::clearBitsExceptRef(bbAttackLeft, bbBitBoards[ePiece::ALL_BLACK_PIECES - color]);
		Bits::clearBitsExceptRef(bbAttackRight, bbBitBoards[ePiece::ALL_BLACK_PIECES - color]);
	}

	Move captureMove(ePiece::WHITE_PAWN);
	if (color == eColor::BLACK)
		captureMove.MovPiece = ePiece::BLACK_PAWN;

	// Foerst tager vi slagene to the left
	while (bbAttackLeft)
	{
		// Find first field
		captureMove.To = Board::GetFirstPiece(bbAttackLeft);

		// Bonden kom fra op-og-til-hoejre
		captureMove.From = static_cast<eSquare>(captureMove.To + (color == eColor::BLACK ? -7 : 9));	//FIXME: Add defines, constants whatever
		AddPawnCaptures(moveList, bbBitBoards, captureMove);

		Bits::clearBitsRef(bbAttackLeft, g_bbMask[captureMove.To]);
	}

	// Then we take the captures to the right
	while (bbAttackRight)
	{
		// Find first field
		captureMove.To = Board::GetFirstPiece(bbAttackRight);

		// Bonden kom fra op-og-til-venstre
		captureMove.From = static_cast<eSquare>(captureMove.To + (color == eColor::BLACK ? -9 : 7));	//FIXME: Add defines, constants whatever
		AddPawnCaptures(moveList, bbBitBoards, captureMove);

		Bits::clearBitsRef(bbAttackRight, g_bbMask[captureMove.To]);
	}
}

void MoveGenerator::GeneratePawnNormalMoves(_In_ const BITBOARD* const bbBitBoards, _In_ eColor color, _Inout_ MoveList& moveList)
{
	BITBOARD bbMoveOne = 0;
	BITBOARD bbMoveTwo = 0;
	if (color == eColor::WHITE)
	{
		// Et skridt frem
		bbMoveOne = Bits::clearBits((bbBitBoards[ePiece::WHITE_PAWN] >> ONE_ROW), bbBitBoards[ALL_PIECES]);
		// Sorterer sidste raekke fra
		Bits::clearBitsRef(bbMoveOne, MASK_RANK_8);
		// To skridt frem - hvide boender kun fra startplacering, dvs spillets 2. raekke 
		bbMoveTwo = Bits::clearBits(((bbBitBoards[ePiece::WHITE_PAWN] & MASK_RANK_2) >> TWO_ROWS), bbBitBoards[ALL_PIECES]);
		// Sikrer os at midterfeltet ogsaa er frit
		Bits::clearBitsRef(bbMoveTwo, (bbBitBoards[ALL_PIECES] >> ONE_ROW));
	}
	else
	{
		// Et skridt frem - men kun hvor der ikke staar nogen foran
		bbMoveOne = Bits::clearBits((bbBitBoards[ePiece::BLACK_PAWN] << ONE_ROW), bbBitBoards[ALL_PIECES]);
		// Sorterer sidste raekke fra
		Bits::clearBitsRef(bbMoveOne, MASK_RANK_1);
		// To skridt frem - sorte boender kun fra startplacering, dvs spillets 7. raekke 
		bbMoveTwo = Bits::clearBits(((bbBitBoards[ePiece::BLACK_PAWN] & MASK_RANK_7) << TWO_ROWS), bbBitBoards[ALL_PIECES]);
		// Sikrer os at midterfeltet ogsaa er frit
		Bits::clearBitsRef(bbMoveTwo, (bbBitBoards[ALL_PIECES] << ONE_ROW));
	}
	// Kombinerer de to bitboards
	Bits::setBitsRef(bbMoveOne, bbMoveTwo);	// TODO: Necessary? We are testing for it right down below

	Move normalMove(ePiece::WHITE_PAWN);
	int direction = 1;

	if (color == eColor::BLACK)
	{
		normalMove.MovPiece = ePiece::BLACK_PAWN;
		direction = -1;
	}

	// Looper indtil der ikke er flere maal-felter tilbage
	while (bbMoveOne)
	{
		const eSquare to = Board::GetFirstPiece(bbMoveOne);
		normalMove.To = to;	// avoids use-after-move below
		// Hvis der staar en bonde paa raekken bag to-feltet, er det et skridt frem
		if (Bits::isAnyBitSet(bbBitBoards[normalMove.MovPiece], g_bbMask[to + (ONE_ROW*direction)]))
		{
			normalMove.From = static_cast<eSquare>(to + (ONE_ROW*direction));
			normalMove.Type = MoveType::Normal;
		}
		else
		{
			normalMove.From = static_cast<eSquare>(to + (TWO_ROWS*direction));
			normalMove.Type = MoveType::PawnTwoForward;
		}
		// Putter traekket i traeklisten
		moveList.emplace_back(normalMove);
		// Now clear it from our attack board
		Bits::clearBitsRef(bbMoveOne, g_bbMask[to]);
	}
}

void MoveGenerator::GenerateOfficerMoves(const BITBOARD* const bbBitBoards, MoveList& moveList, ePieceType piece, bool onlyCaptures)
{
	const Board& pBoard = Board::Instance();

	// Henter braetinformation fra Board-klassen
	const eColor iColor = pBoard.GetCurrentColor();

	Move move(static_cast<ePiece>(piece + iColor));

	BITBOARD bbPiecesToMove = bbBitBoards[move.MovPiece];
	while (bbPiecesToMove)
	{
		move.From = Board::GetFirstPiece(bbPiecesToMove);

		BITBOARD bbAttack = GetOfficerAttackBoard(bbBitBoards, move);
		if (onlyCaptures)
		{
			// Reducerer bitboardet til felter, hvor modstanderens brikker staar
			Bits::clearBitsExceptRef(bbAttack, bbBitBoards[ALL_FROM_COLOR + (iColor == WHITE ? 1 : 0)]);
		}

		// Add all legal moves found above
		AddOfficerMoves(moveList, bbAttack, move);

		Bits::clearBitsRef(bbPiecesToMove, g_bbMask[move.From]);
	}
}

// Returns a Attackboard for a single piece (move.From.MovPiece)
BITBOARD MoveGenerator::GetOfficerAttackBoard(const BITBOARD* bbBitBoards, const Move& move) noexcept
{
	switch (move.MovPiece)
	{
	case ePiece::WHITE_KNIGHT:
	case ePiece::BLACK_KNIGHT:
		return Bits::clearBits(g_bbKnightMoves[move.From], bbBitBoards[ALL_FROM_COLOR + PieceHelper::Color(move.MovPiece)]);
	case ePiece::WHITE_BISHOP:
	case ePiece::BLACK_BISHOP:
		return GetBishopBitboard(bbBitBoards, move.From, PieceHelper::Color(move.MovPiece));
	case ePiece::WHITE_ROOK:
	case ePiece::BLACK_ROOK:
		return GetTowerBitboard(bbBitBoards, move.From, PieceHelper::Color(move.MovPiece));
	case ePiece::WHITE_QUEEN:
	case ePiece::BLACK_QUEEN:
		// Dronningen er i virkeligheden et taarn med en loeber ovenpaa hovedet ;-) 
		// (dvs kan bevaege sig som baade et taarn og en loeber samtidigt (dog en ad gangen))
		return GetBishopBitboard(bbBitBoards, move.From, PieceHelper::Color(move.MovPiece)) |
			GetTowerBitboard(bbBitBoards, move.From, PieceHelper::Color(move.MovPiece));
	case ePiece::WHITE_KING:
	case ePiece::BLACK_KING:
		return Bits::clearBits(g_bbKingMoves[move.From], bbBitBoards[ALL_FROM_COLOR + PieceHelper::Color(move.MovPiece)]);
	case ePiece::WHITE_PAWN:
	case ePiece::BLACK_PAWN:
	default:
		assert(!"Invalid call on GetAttackBoard");
		return 0;
	}
}

// Remark: Pawn Capture+Promotes are handled in AddPawnCaptureMoves()
void MoveGenerator::AddPawnPromoteMoves(const BITBOARD* bbBitBoards, MoveList &moveList)
{
	BITBOARD bbAttack = 0;

	// Henter braetinformation fra Board-klassen
	const eColor color = Board::Instance().GetCurrentColor();

	if (color == BLACK)
	{
		// Ser paa boenderne, der staar syvende raekke, hvor der ikke er en brik foran
		bbAttack = Bits::clearBits(((bbBitBoards[ePiece::BLACK_PAWN] << ONE_ROW) & MASK_RANK_1), bbBitBoards[ALL_PIECES]);
	}
	else
	{
		bbAttack = Bits::clearBits(((bbBitBoards[ePiece::WHITE_PAWN] >> ONE_ROW) & MASK_RANK_8), bbBitBoards[ALL_PIECES]);
	}

	Move promoteMove;
	promoteMove.Type = MoveType::Promote;
	promoteMove.Content = ePiece::NO_PIECE;
	// Genererer traek ud fra maalfelt-bitboardet indtil det er tomt
	while (bbAttack)
	{
		promoteMove.To = Board::GetFirstPiece(bbAttack);
		promoteMove.From = SquareHelper::PreviousRow(promoteMove.To, color);

		// Add the four selections
		promoteMove.MovPiece = static_cast<ePiece>(QUEEN + color);
		moveList.emplace_back(promoteMove);
		promoteMove.MovPiece = static_cast<ePiece>(ROOK + color);	// FIXME: Use after move!
		moveList.emplace_back(promoteMove);
		promoteMove.MovPiece = static_cast<ePiece>(BISHOP + color); // FIXME: Use after move!
		moveList.emplace_back(promoteMove);
		promoteMove.MovPiece = static_cast<ePiece>(KNIGHT + color);	// FIXME: Use after move
		moveList.emplace_back(promoteMove);

		Bits::clearBitsRef(bbAttack, g_bbMask[promoteMove.To]);	// FIXME: Use after move
	}
}

// <param name="moveList">Collection of found moves</param> 
// Computes all possible moves, filters so only all captures, en passants and promotes remains and returns unsorted
void MoveGenerator::ComputeCaptures(_In_ const GameInfo& info, _Inout_ MoveList& moveList)
{
	const auto color = Board::Instance().GetCurrentColor();

	const auto boards = Board::Instance().GetBitBoards();

	/* Peasant moves
	---------------------*/

	//Captures (including en-passant and promotion-captures)
	GeneratePawnCaptures(boards.data(), info, moveList, color);

	//Then normal Promotes
	AddPawnPromoteMoves(boards.data(), moveList);

	/* Officer moves
	---------------------*/
	// GetAttackBoard for opposite color
	GenerateOfficerMoves(boards.data(), moveList, KNIGHT, true);
	GenerateOfficerMoves(boards.data(), moveList, BISHOP, true);
	GenerateOfficerMoves(boards.data(), moveList, ROOK, true);
	GenerateOfficerMoves(boards.data(), moveList, QUEEN, true);
	GenerateOfficerMoves(boards.data(), moveList, KING, true);
}


// The Move is only filled in MovPiece and From fields
void MoveGenerator::AddOfficerMoves(MoveList& moveList, BITBOARD bbAttack, Move& move)
{
	assert(PieceHelper::IsActual(move.MovPiece));
	assert(move.From != NO_SQUARE);

	while (bbAttack)
	{
		move.To = Board::GetFirstPiece(bbAttack);
		move.Content = Board::Instance().GetPiece(move.To);
		move.Type = (PieceHelper::IsActual(move.Content) ? MoveType::Capture : MoveType::Normal);

		moveList.emplace_back(move);
		Bits::clearBitsRef(bbAttack, g_bbMask[move.To]);	// Fjern fra attack bitboardet	// FIXME: Use after move
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
void MoveGenerator::AddCastleMoves(MoveList& moveList, eColor color, const BITBOARD* bbBitBoards, const GameInfo &info)
{
	const auto sqFrom = Board::GetFirstPiece(bbBitBoards[static_cast<ePiece>(KING + color)]);	// There is only one king!!
	const Board& board = Board::Instance();

	if (WHITE == color)
	{
		if (!(info.whiteLongCastle || info.whiteShortCastle))	// no need to look further
			return;
		// We already know which squares are involved in the castlings
		assert(sqFrom == e1);	// king must be in starting position
		assert(board.GetPiece(e1) == ePiece::WHITE_KING);

		if (info.whiteShortCastle)
			assert(board.GetPiece(h1) == ePiece::WHITE_ROOK);
		if (info.whiteLongCastle)
			assert(board.GetPiece(a1) == ePiece::WHITE_ROOK);

		Move castlingMove(NO_SQUARE, sqFrom, MoveType::Castling, ePiece::WHITE_KING, ePiece::NO_PIECE);

		// First handle short castling - 
		// Are the needed squares available?
		static const BITBOARD bbMask_f1g1 = g_bbMask[f1] | g_bbMask[g1];
		if (info.whiteShortCastle &&
			!board.IsOccupied(bbMask_f1g1))
		{
			// Are these squares being attacked ? The king must not move away from, pass or move into a check!
			static const BITBOARD bbMask_e1f1g1 = g_bbMask[e1] | g_bbMask[f1] | g_bbMask[g1];
			if (!IsAttacked(bbMask_e1f1g1, BLACK))
			{
				castlingMove.To = g1;
				// Accepted - Add the move to the list
				moveList.emplace_back(castlingMove);
			}
		}
		static const BITBOARD bbMask_d1c1b1 = g_bbMask[d1] | g_bbMask[c1] | g_bbMask[b1];
		// Are the needed squares available for long castling?
		if (info.whiteLongCastle &&
			!board.IsOccupied(bbMask_d1c1b1))
		{
			// Are these squares being attacked ? The king must not move away from, pass or move into a check!
			static const BITBOARD bbMask_e1d1c1 = g_bbMask[e1] | g_bbMask[d1] | g_bbMask[c1];
			if (!IsAttacked(bbMask_e1d1c1, BLACK))
			{
				// Accepted - Add the move to the list
				castlingMove.To = c1;		// FIXME: Use after move
				moveList.emplace_back(castlingMove);
			}
		}
	}
	else	// BLACK
	{
		if (!(info.blackLongCastle || info.blackShortCastle))	// no need to look further
			return;
		// We already know which squares are involved in the castlings
		assert(sqFrom == e8);	// king must be in starting position
		assert(board.GetPiece(e8) == ePiece::BLACK_KING);

		if (info.blackShortCastle && (info.lastMove.To != h8))
			assert(board.GetPiece(h8) == ePiece::BLACK_ROOK);
		if (info.blackLongCastle && (info.lastMove.To != a8))
			assert(board.GetPiece(a8) == ePiece::BLACK_ROOK);

		Move castlingMove(NO_SQUARE, sqFrom, MoveType::Castling, ePiece::BLACK_KING, ePiece::NO_PIECE);

		// First handle short castling - 
		// Are the needed squares available?
		static const BITBOARD bbMask_f8g8 = g_bbMask[f8] | g_bbMask[g8];
		if (info.blackShortCastle &&
			!board.IsOccupied(bbMask_f8g8))
		{
			// Are these squares being attacked ? The king must not move away from, pass or move into a check!
			static const BITBOARD bbMask_e8f8g8 = g_bbMask[e8] | g_bbMask[f8] | g_bbMask[g8];
			if (!IsAttacked(bbMask_e8f8g8, WHITE))
			{
				// Accepted - Add the move to the list
				castlingMove.To = g8;
				moveList.emplace_back(castlingMove);
			}
		}
		// Are the needed squares available?
		static const BITBOARD bbMask_d8c8b8 = g_bbMask[d8] | g_bbMask[c8] | g_bbMask[b8];
		if (info.blackLongCastle &&
			!board.IsOccupied(bbMask_d8c8b8))
		{
			// Are these squares being attacked ? The king must not move away from, pass or move into a check!
			static const BITBOARD bbMask_e8d8c8 = g_bbMask[e8] | g_bbMask[d8] | g_bbMask[c8];
			if (!IsAttacked(bbMask_e8d8c8, WHITE))
			{
				// Accepted - Add the move to the list
				castlingMove.To = c8;
				moveList.emplace_back(castlingMove);
			}
		}
	}
}

// Bemaerk: color er for bonden i traekket
void MoveGenerator::AddPawnCaptures(MoveList& moveList, const BITBOARD* bbBitBoards, Move& peasantMove)
{
	const auto color = PieceHelper::Color(peasantMove.MovPiece);

	// Forventer kun slag, hvor vi rent faktisk har en bonde paa fra-feltet!
	assert(Bits::isAnyBitSet(bbBitBoards[color], g_bbMask[peasantMove.From]));

	const Board& board = Board::Instance();

	// Almindeligt slag af brik ? 
	if (IsCapture(bbBitBoards, color, peasantMove))
	{
		peasantMove.Content = board.GetPiece(peasantMove.To);
		assert(PieceHelper::IsActual(peasantMove.Content));
		assert(PieceHelper::Color(peasantMove.Content) != PieceHelper::Color(peasantMove.MovPiece));

		// Er det ogsaa en promotion?
		if (!IsAnyBackRow(peasantMove.To))		// Nope, normalt slag
		{
			// Set it to be a peasant
			peasantMove.MovPiece = static_cast<ePiece>(PAWN + color);
			peasantMove.Type = MoveType::Capture;
			moveList.emplace_back(std::move(peasantMove));
		}
		else
		{		// Jeps, det er en bondeforvandling
			peasantMove.Type = MoveType::PromoteCapture;
			// Add the 4 different selections - now moving piece is changed!
			peasantMove.MovPiece = static_cast<ePiece>(QUEEN + color);
			moveList.emplace_back(peasantMove);	// Add Queen selection
			peasantMove.MovPiece = static_cast<ePiece>(ROOK + color);	
			moveList.emplace_back(peasantMove);	// Add Rook selection
			peasantMove.MovPiece = static_cast<ePiece>(BISHOP + color);	
			moveList.emplace_back(peasantMove);	// Add Bishop selection
			peasantMove.MovPiece = static_cast<ePiece>(KNIGHT + color);
			moveList.emplace_back(peasantMove);	// Add Knight selection		
		}
	}
	// Hvis der ikke er en brik paa til-feltet er det et en-passant traek
	else
	{
		const eSquare epWhere = SquareHelper::PreviousRow(peasantMove.To, color);
		peasantMove.Content = board.GetPiece(epWhere);
		assert(peasantMove.Content == (BLACK_PAWN - color));
		peasantMove.MovPiece = static_cast<ePiece>(PAWN + color);
		peasantMove.Type = MoveType::En_Passant;
		moveList.emplace_back(std::move(peasantMove));
	}
}

//***************************************
// Method:      GetTowerBitboard
// Description: 
// FullName:    private MoveGenerator::GetTowerBitboard const
// Returns:     BITBOARD - 
// Parameter:   const BITBOARD* bbBitBoards - 
//			:   const Square& from - 
//			:   eColor color - 
//***************************************
BITBOARD MoveGenerator::GetTowerBitboard(const BITBOARD* bbBitBoards, eSquare from, eColor color) noexcept
{
	// Henholdsvis vandrette og lodrette muligheder
	auto iOccupied = static_cast<int>((bbBitBoards[ALL_PIECES] >> (Rank(from) << 3)) & 255);
	BITBOARD bbAttack = Bits::clearBits(g_bbMovesRank[from][iOccupied], bbBitBoards[ALL_FROM_COLOR + color]);

	iOccupied = static_cast<int>((bbBitBoards[ROTATED90] >> (File(from) << 3)) & 255);
	bbAttack |= Bits::clearBits(g_bbMovesFile[from][iOccupied], bbBitBoards[ALL_FROM_COLOR + color]);

	return bbAttack;
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
	// Traekmuligheder i begge diagonaler
	auto iOccupied = static_cast<int>((bbBitBoards[ROTATED45R] >> g_iDiagonalShifts_a1h8[from])
		& g_bbDiagonalMask_a1h8[from]);
	BITBOARD bbAttack = Bits::clearBits(g_bbMovesa1h8[from][iOccupied], bbBitBoards[ALL_FROM_COLOR + color]);

	iOccupied = static_cast<int>((bbBitBoards[ROTATED45L] >> g_iDiagonalShifts_a8h1[from])
		& g_bbDiagonalMask_a8h1[from]);
	bbAttack |= Bits::clearBits(g_bbMovesa8h1[from][iOccupied], bbBitBoards[ALL_FROM_COLOR + color]);

	return bbAttack;
}

// Fungerer ved at kalde GetAttackBoard() og se om et af dem rammer 'pos'
// TODO: Med denne rutine er det ikke muligt at se _hvilken_ brik der truer 'pos'
// TODO: Currently unused
bool MoveGenerator::IsAttacked(eSquare pos, eColor attackByColor) noexcept
{
	const BITBOARD bb = GetAttackBoard(attackByColor);
	// Hvis angrebsbitboardet indeholder 'pos' returneres true
	return Bits::isAnyBitSet(bb, g_bbMask[pos]);
}

// Fungerer ved at kalde GetAttackBoard() og se om et af dem rammer et af felterne i 'squares' board'et.
// TODO: Med denne rutine er det ikke muligt at se _hvilken_ brik der truer 'squares'
bool MoveGenerator::IsAttacked(BITBOARD squares, eColor attackByColor) noexcept
{
	const BITBOARD bb = GetAttackBoard(attackByColor);
	// Hvis angrebsbitboardet indeholder en i mask 'pos' returneres true
	return Bits::isAnyBitSet(bb, squares);
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
BITBOARD MoveGenerator::GetAttackBoard(eColor attackByColor) noexcept
{
	// Henter braetinformation fra Board-klassen
	const auto boards = Board::Instance().GetBitBoards();
	
	BITBOARD bbAttackBoard = 0;	// Vil indeholde alle modstanderens slutpositioner efter et CAPTURE move

	/* Boenderne */

	// De hvide foerst
	if (attackByColor == WHITE)
	{
		// Laver i foerste omgang et maalfelt-bitboard med alle skraa traek for boenderne
		bbAttackBoard = (((boards[ePiece::WHITE_PAWN] & ~(g_bbFileMask[eFileNames::RIGHT_FILE])) >> 7) |
			((boards[ePiece::WHITE_PAWN] & ~(g_bbFileMask[eFileNames::LEFT_FILE])) >> 9));

		// Reducerer bitboardet til felter, hvor modstanderens brikker staar
		Bits::clearBitsExceptRef(bbAttackBoard, boards[ePiece::ALL_BLACK_PIECES]);

	}
	// saa de sorte
	else
	{
		// Laver i foerste omgang et maalfelt-bitboard med alle skraa traek for boenderne
		bbAttackBoard = (((boards[ePiece::BLACK_PAWN] & ~(g_bbFileMask[eFileNames::RIGHT_FILE])) << 9) |
			((boards[ePiece::BLACK_PAWN] & ~(g_bbFileMask[eFileNames::LEFT_FILE])) << 7));

		// Reducerer bitboardet til felter, hvor modstanderens brikker staar
		Bits::clearBitsExceptRef(bbAttackBoard, boards[ePiece::ALL_WHITE_PIECES]);

		// Bruges i oejeblikket kun til InCheck() og Rokade checks - her er En passant feltet er unoedvendig
		// da det altid kun kan ramme boender - Men foerste gang det bruges andet steds er det ikke godt nok
	}

	// Saa er det officerer og kongen
	//---------------------------------

	/* Kongen */

	auto iFrom = Board::GetFirstPiece(boards[KING + attackByColor]);
	bbAttackBoard |= g_bbKingMoves[iFrom] & ~(boards[ALL_FROM_COLOR + attackByColor]);


	/* Springerne */

	BITBOARD bbPiecesToMove = boards[KNIGHT + attackByColor];

	while (bbPiecesToMove)
	{
		iFrom = Board::GetFirstPiece(bbPiecesToMove);
		bbAttackBoard |= g_bbKnightMoves[iFrom] & ~(boards[ALL_FROM_COLOR + attackByColor]);
		Bits::clearBitsRef(bbPiecesToMove, g_bbMask[iFrom]);
	}


	/* Traek for taarnene (og dronninger)
	--------------------*/

	bbPiecesToMove = Bits::setBits(boards[ROOK + attackByColor], boards[QUEEN + attackByColor]);

	while (bbPiecesToMove)
	{
		iFrom = Board::GetFirstPiece(bbPiecesToMove);
		// Faa mulige traek for dronninger og taarne
		bbAttackBoard |= GetTowerBitboard(boards.data(), iFrom, attackByColor);

		Bits::clearBitsRef(bbPiecesToMove, g_bbMask[iFrom]);
	}


	/* Traek for loeberne (og dronninger)
	--------------------*/

	bbPiecesToMove = Bits::setBits(boards[BISHOP + attackByColor], boards[QUEEN + attackByColor]);

	while (bbPiecesToMove)
	{
		iFrom = Board::GetFirstPiece(bbPiecesToMove);

		// Faa mulige traek for dronninger og loebere
		bbAttackBoard |= GetBishopBitboard(boards.data(), iFrom, attackByColor);

		Bits::clearBitsRef(bbPiecesToMove, g_bbMask[iFrom]);
	}

	return bbAttackBoard;
}
