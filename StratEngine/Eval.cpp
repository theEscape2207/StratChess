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

//
//	Evaluate() : 
//	Description: Sums up the material value from both colors. Adds additional bonuses according to heuristics
//	Returns:	 The value of the player in turn subtracted the oppositions value 
// FIXME:		 Evaluate does not know about Check Mate - this is strictly only an evaluation of the current position 
//				 - this means that we miss the first (and best, maybe even only?) opportunity to do check mate!
//
int EvalComplex::Evaluate(const Board& board) const noexcept
{
	const int matScoreBlack = board.GetMaterialScore(BLACK);
	const int matScoreWhite = board.GetMaterialScore(WHITE);

	PlayState gameStage = PlayState::MIDDLEGAME;
	const int iMinScore = std::min(matScoreWhite, matScoreBlack);
	if (iMinScore <= 11500)		// TODO: King are worth 10000 - but maybe they shouldn't be counted??
		//if(iMinScore <= 10600)	// TODO: action on 'gameStage = FINALGAME' is the same as 'gameStage = ENDGAME'
		//	gameStage = PlayState::FINALGAME;
		//else
		gameStage = PlayState::ENDGAME;

	int bonusScore[2] = { 0 };

	const auto boards = board.GetBitBoards();
	const BITBOARD all_pieces = boards[ALL_PIECES];
	const BITBOARD white_pawns = boards[ePiece::WHITE_PAWN];
	const BITBOARD black_pawns = boards[ePiece::BLACK_PAWN];
	const BITBOARD all_white = boards[ePiece::ALL_WHITE_PIECES];
	const BITBOARD all_black = boards[ePiece::ALL_BLACK_PIECES];

	auto remaining = all_pieces;
	while (remaining)
	{
		const eSquare square = Board::GetFirstPiece(remaining);	// First square with a piece on
		const ePiece piece = board.GetPiece(square);		// get the actual piece

		const int rank = Rank(square);
		const int file = File(square);

		const int pieceType = (piece >> 1);	// Samme som /2

		// Add the pieces positional values - using preset table values
		if (pieceType != (KING >> 1))
		{
			bonusScore[PieceHelper::Color(piece)] += GetPositionalScore(square, piece);
		}

		/*
		 *	Adding bonuses and penalties depending on the relation with the other pieces
		 *
		 */
		switch (piece)
		{
		case ePiece::WHITE_PAWN:
			// TODO: Add bonus for passed pawn - bonus should be dependant on game stage

			// Hvis der er en hvid bonde over denne i samme kolonne gives en straf
			if (white_pawns & g_bbFileUpMask[square])
				bonusScore[WHITE] -= DOUBLED_PAWN_PENALTY;

			// Hvis der ikke er en hvid bonde i en af raekkerne ved siden af
			// gives en straf
			if ((file == eFileNames::LEFT_FILE || !(white_pawns & g_bbFileMask[file - 1])) &&
				(file == eFileNames::RIGHT_FILE || !(white_pawns & g_bbFileMask[file + 1])))
				bonusScore[WHITE] -= ISOLATED_PAWN_PENALTY;

			break;

		case ePiece::BLACK_PAWN:

			// Hvis der er en sort bonde under denne i samme kolonne gives en straf
			if (Bits::isAnyBitSet(black_pawns, g_bbFileDownMask[square]))
				bonusScore[BLACK] -= DOUBLED_PAWN_PENALTY;

			// Hvis der ikke er en sort bonde i en af raekkerne ved siden af
			// gives en straf
			if ((file == eFileNames::LEFT_FILE || !(black_pawns & g_bbFileMask[file - 1])) &&
				(file == eFileNames::RIGHT_FILE || !(black_pawns & g_bbFileMask[file + 1])))
				bonusScore[BLACK] -= ISOLATED_PAWN_PENALTY;
			break;

		case ePiece::WHITE_ROOK:

			// Bonus hvis taarnet er i syvende raekke sent i spillet
			if (rank == WHITE_7TH_ROW && gameStage != PlayState::MIDDLEGAME)
				bonusScore[WHITE] += ROOK_ON_7TH_BONUS;

			// Bonus hvis der er aabne raekker til taarnet
			if (!(white_pawns & g_bbFileUpMask[square]))
			{
				bonusScore[WHITE] += HALF_OPEN_FILE;

				if (!(g_bbFileMask[file] & all_black))
					bonusScore[WHITE] += OPEN_FILE - HALF_OPEN_FILE;
				//else if (!(g_bbFileMask[file] & 
				//		  (pBitBoards[ePiece::BLACK_QUEEN] | pBitBoards[ePiece::BLACK_ROOK])))
				//	bonusScore[WHITE] += HALF_OPEN_FILE;
			}
			break;

		case ePiece::BLACK_ROOK:
			// TODO: Add bonus for connected Rooks!!
			// Bonus hvis taarnet er i syvende raekke sent i spillet
			if (rank == BLACK_7TH_ROW && gameStage != PlayState::MIDDLEGAME)
				bonusScore[BLACK] += ROOK_ON_7TH_BONUS;

			// Bonus hvis der er aabne raekker til taarnet
			if (!(black_pawns & g_bbFileDownMask[square]))
			{
				bonusScore[BLACK] += HALF_OPEN_FILE;

				if (!(g_bbFileMask[file] & all_white))
					bonusScore[BLACK] += OPEN_FILE - HALF_OPEN_FILE;
				//else if (!(g_bbFileMask[file] & 
				//		  (pBitBoards[ePiece::WHITE_QUEEN] | pBitBoards[ePiece::WHITE_ROOK])))
				//	bonusScore[BLACK] += HALF_OPEN_FILE;
			}
			break;

		case ePiece::WHITE_KING:
		case ePiece::BLACK_KING:
			// TODO: Add bonus for castling-done!!
			// End game has other requirements for the King placement
			if (gameStage == PlayState::MIDDLEGAME)
				bonusScore[PieceHelper::Color(piece)] +=
				(g_Eval_Bitboards[5][getEvalBoard(piece, square)]);
			else
				bonusScore[PieceHelper::Color(piece)] +=
				(g_Eval_Bitboards[6][getEvalBoard(piece, square)]);
			break;
		case ePiece::WHITE_KNIGHT:
		case ePiece::BLACK_KNIGHT:
		case ePiece::WHITE_BISHOP:
		case ePiece::BLACK_BISHOP:
		case ePiece::WHITE_QUEEN:
		case ePiece::BLACK_QUEEN:
			break;
		default:
			assert(!"What! A new type of piece...");
			break;
		}
		// We're done! Remove the bit and continue
		remaining = Bits::clearLsb(remaining);
	}

	// Mop-up evaluation: in decisively-won, pawnless endings, reward driving the
	// losing king to the edge/corner and closing the distance between the two
	// kings — the win may lie beyond the search horizon otherwise (issue #70).
	if (gameStage != PlayState::MIDDLEGAME &&
		white_pawns == 0ULL && black_pawns == 0ULL)
	{
		const int matDiff = matScoreWhite - matScoreBlack;
		const int absMatDiff = (matDiff >= 0) ? matDiff : -matDiff;

		if (absMatDiff >= MOPUP_MATERIAL_THRESHOLD)
		{
			const eColor winner = (matDiff > 0) ? WHITE : BLACK;
			const eColor loser = (winner == WHITE) ? BLACK : WHITE;

			const eSquare winnerKingSq = Board::GetFirstPiece(
				boards[(winner == WHITE) ? ePiece::WHITE_KING : ePiece::BLACK_KING]);
			const eSquare loserKingSq = Board::GetFirstPiece(
				boards[(loser == WHITE) ? ePiece::WHITE_KING : ePiece::BLACK_KING]);

			bonusScore[winner] += MOPUP_CMD_WEIGHT * CenterManhattanDistance(loserKingSq) +
				MOPUP_KINGDIST_WEIGHT * (MOPUP_MAX_KING_DISTANCE - KingDistance(winnerKingSq, loserKingSq));
		}
	}


	const eColor color = board.GetCurrentColor();

	if (color == WHITE)
	{
		return (matScoreWhite + bonusScore[WHITE]) - (matScoreBlack + bonusScore[BLACK]);
	}
	return (matScoreBlack + bonusScore[BLACK]) - (matScoreWhite + bonusScore[WHITE]);
}
