// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Board.h"
#include "MoveGenerator.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "Utils\FENParser.h"
#include <random>

extern std::ofstream outLegalMoves;

// Zobrist key tables (defined here, declared in Board.h)
namespace zobrist {
	std::array<std::array<uint64_t, NUM_SQUARES>, ALL_PIECETYPES> piece_keys;
	std::array<uint64_t, 16> castling_keys;
	std::array<uint64_t, NUM_SQUARES> ep_keys;
	uint64_t side_key;

	void initialize() noexcept {
		
		// non-deterministic seed for production use (uncomment for true randomness, but beware non-reproducible hashes across runs)
		// std::random_device rd;
		// std::mt19937 rng(rd());
		// Deterministic seed for reproducibility
		std::mt19937_64 rng(0x123456789ABCDEF0ULL);

		//std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
		// Fills piece_keys with random 64-bit values for piece-placement Zobrist hashing.
		for (int piece = ePiece::WHITE_PAWN; piece < ALL_PIECETYPES; ++piece) {
			for (int square = 0; square < ALL_SQUARES; ++square) {
				piece_keys[piece][square] = rng();
			}
		}

		for (size_t i = 0; i < 16; ++i)
			castling_keys[i] = rng();

		for (size_t sq = 0; sq < NUM_SQUARES; ++sq)
			ep_keys[sq] = rng();

		side_key = rng();
	}
}

Board::Board()
{
	mailbox_.fill(ePiece::NO_PIECE);
	zobrist::initialize();
}

void Board::clear_board()
{
	bitboards_.fill(0);
	mailbox_.fill(ePiece::NO_PIECE);

	sideToMove_ = WHITE;
	gameInfo_.Reset();
	reset_repetition_history();
	currentPly_ = 0;

	gameInfoHistory_.fill(GameInfo{});
	material_score_[WHITE] = material_score_[BLACK] = 0;
	zobrist_hash_ = 0;
}

// Adds a piece to all relevant bitboards and the mailbox. Updates the Zobrist hash.
// Does NOT update material score — call add_piece_to_board for that.
void Board::add_piece(eSquare square, ePiece piece)
{
	set_bitboard_square(piece, square);
	set_bitboard_square(bitboard_index(ALL_FROM_COLOR, PieceHelper::Color(piece)), square);
	set_bitboard_square(ALL_PIECES, square);

	// Rotated bitboards for sliding piece attack generation
	BitBoardHelper::SetBitboardMask(bitboards_[ROTATED90],  g_bbMaskRotated90[square]);
	BitBoardHelper::SetBitboardMask(bitboards_[ROTATED45R], g_bbMaskRotated45R[square]);
	BitBoardHelper::SetBitboardMask(bitboards_[ROTATED45L], g_bbMaskRotated45L[square]);

	zobrist_hash_ ^= zobrist::piece_keys[piece][square];

	set_square(square, piece);

	assert(test_bitboards(outLegalMoves));
}

// Removes a piece from all relevant bitboards and the mailbox. Updates the Zobrist hash.
// Does NOT update material score — call remove_piece_from_board for that.
void Board::remove_piece(eSquare square, ePiece piece)
{
	assert(GetPiece(square) == piece);

	if (!clear_bitboard_square(piece, square))
		test_bitboards(std::cout);

	clear_bitboard_square(bitboard_index(ALL_FROM_COLOR, PieceHelper::Color(piece)), square);
	clear_bitboard_square(ALL_PIECES, square);

	BitBoardHelper::ClearBitboardMask(bitboards_[ROTATED90],  g_bbMaskRotated90[square]);
	BitBoardHelper::ClearBitboardMask(bitboards_[ROTATED45R], g_bbMaskRotated45R[square]);
	BitBoardHelper::ClearBitboardMask(bitboards_[ROTATED45L], g_bbMaskRotated45L[square]);

	zobrist_hash_ ^= zobrist::piece_keys[piece][square];

	clear_square(square);

	assert(test_bitboards(outLegalMoves));
}

// Sets up the board from a collection of (piece, square) pairs.
// Clears the board first; does not set castling rights or en-passant (use SetupFromFEN).
void Board::setup_board(const squareCol& col)
{
	clear_board();

	for (const auto& sqPiece : col)
		add_piece_to_board(std::get<0>(sqPiece), std::get<1>(sqPiece));

	spdlog::default_logger()->debug("Custom board set up");
}

void Board::SetupFromFEN(const std::string& fen)
{
	FENParser::FENGameState state;
	std::vector<std::tuple<ePiece, eSquare>> pieces;

	auto parseError = FENParser::ParseFEN(fen, state, pieces);
	if (parseError) {
		spdlog::default_logger()->error("FEN parse error: {}", *parseError);
		return;
	}

	setup_board(pieces);

	sideToMove_              = state.sideToMove;
	gameInfo_.epSquare       = state.epSquare;
	gameInfo_.castlingRights = state.castlingRights;
	gameInfo_.fiftyCount     = state.halfMoveClock;
	gameInfo_.fullMoveCount  = state.fullMoveCounter;

	// Validate parsed metadata and adjust castling/EP if inconsistent with actual pieces
	FENParser::ValidatePositionAgainstFENMetadata(*this, state);

	gameInfo_.epSquare       = state.epSquare;
	gameInfo_.castlingRights = state.castlingRights;

	spdlog::default_logger()->debug("Board set up from FEN: {}", fen);
}

std::string Board::ExtractFEN() const
{
	std::string fen;

	// 1. Piece placement (rank 8 → rank 1)
	for (int rank = 0; rank < 8; ++rank) {
		int emptyCount = 0;

		for (int file = 0; file < 8; ++file) {
			eSquare square = static_cast<eSquare>((rank << 3) + file);
			ePiece  piece  = GetPiece(square);

			if (piece == ePiece::NO_PIECE) {
				emptyCount++;
			}
			else {
				if (emptyCount > 0) {
					fen += std::to_string(emptyCount);
					emptyCount = 0;
				}
				fen += g_cPieceNames[piece];
			}
		}

		if (emptyCount > 0)
			fen += std::to_string(emptyCount);

		if (rank < 7)
			fen += '/';
	}

	// 2. Active color
	fen += ' ';
	fen += (sideToMove_ == WHITE) ? 'w' : 'b';

	// 3. Castling availability
	fen += ' ';
	if (gameInfo_.castlingRights == CastlingRights::NONE) {
		fen += '-';
	}
	else {
		if (gameInfo_.castlingRights & CastlingRights::WHITE_KINGSIDE)  fen += 'K';
		if (gameInfo_.castlingRights & CastlingRights::WHITE_QUEENSIDE) fen += 'Q';
		if (gameInfo_.castlingRights & CastlingRights::BLACK_KINGSIDE)  fen += 'k';
		if (gameInfo_.castlingRights & CastlingRights::BLACK_QUEENSIDE) fen += 'q';
	}

	// 4. En passant target square
	fen += ' ';
	if (gameInfo_.epSquare == NO_SQUARE) {
		fen += '-';
	}
	else {
		int file = File(gameInfo_.epSquare);
		int rank = 7 - Rank(gameInfo_.epSquare);
		fen += static_cast<char>('a' + file);
		fen += static_cast<char>('1' + rank);
	}

	// 5. Halfmove clock
	fen += ' ';
	fen += std::to_string(gameInfo_.fiftyCount);

	// 6. Fullmove number
	fen += ' ';
	fen += std::to_string(gameInfo_.fullMoveCount);

	return fen;
}

void Board::SetDefaultBoard()
{
	clear_board();

	// Black pieces
	add_piece_to_board(ePiece::BLACK_ROOK,   a8);
	add_piece_to_board(ePiece::BLACK_KNIGHT, b8);
	add_piece_to_board(ePiece::BLACK_BISHOP, c8);
	add_piece_to_board(ePiece::BLACK_QUEEN,  d8);
	add_piece_to_board(ePiece::BLACK_KING,   e8);
	add_piece_to_board(ePiece::BLACK_BISHOP, f8);
	add_piece_to_board(ePiece::BLACK_KNIGHT, g8);
	add_piece_to_board(ePiece::BLACK_ROOK,   h8);

	for (int i = a7; i <= h7; i++)
		add_piece_to_board(ePiece::BLACK_PAWN, static_cast<eSquare>(i));

	// White pieces
	for (int i = a2; i <= h2; i++)
		add_piece_to_board(ePiece::WHITE_PAWN, static_cast<eSquare>(i));

	add_piece_to_board(ePiece::WHITE_ROOK,   a1);
	add_piece_to_board(ePiece::WHITE_KNIGHT, b1);
	add_piece_to_board(ePiece::WHITE_BISHOP, c1);
	add_piece_to_board(ePiece::WHITE_QUEEN,  d1);
	add_piece_to_board(ePiece::WHITE_KING,   e1);
	add_piece_to_board(ePiece::WHITE_BISHOP, f1);
	add_piece_to_board(ePiece::WHITE_KNIGHT, g1);
	add_piece_to_board(ePiece::WHITE_ROOK,   h1);

	spdlog::default_logger()->debug("Default board set up");
}

std::span<BITBOARD> Board::GetBitBoards() noexcept
{
	return std::span(bitboards_);
}

bool Board::DoMove(const Move& m)
{
	assert(MoveHelper::IsValid(m));
	assert(GetCurrentColor() == PieceHelper::Color(m.MovPiece));

	// Save state for UndoMove
	gameInfoHistory_[currentPly_]        = gameInfo_;
	irreversiblePlyHistory_[currentPly_] = last_irreversible_ply_;
	zobrist_history_[currentPly_]        = zobrist_hash_;

	const eSquare from = m.from();
	const eSquare to   = m.to();

	// Correct except for EP captures (NO_PIECE on to). Will be adjusted below

	const auto capturedPiece = get_captured_piece(m);
	capturedHistory_[currentPly_] = capturedPiece;
	assert(capturedPiece == m.Content);	// Validates captured piece matches move content

	switch (MoveHelper::AsType(m))
	{
	case MoveType::QUIET:
		assert(!PieceHelper::IsActual(m.Content));
		move_piece(m);
		break;

	case MoveType::CAPTURE:
		assert(MoveHelper::IsCapture(m));
		remove_piece_from_board(m.Content, to);
		move_piece(m);
		break;

	case MoveType::DOUBLE_PAWN_PUSH:
		assert(!PieceHelper::IsActual(m.Content));
		assert(MoveHelper::IsPawnMove(m));
		move_piece(m);
		break;

	case MoveType::EP_CAPTURE: {
		assert(MoveHelper::IsPawnMove(m));
		assert(MoveHelper::IsCapture(m) && PieceHelper::IsPawn(m.Content));
		// Captured pawn sits one rank behind the destination square
		const eSquare epCapturedPawnSquare = SquareHelper::PreviousRow(to, sideToMove_);
		remove_piece_from_board(capturedPiece, epCapturedPawnSquare);
		move_piece(m);
		break;
	}

	case MoveType::PROMOTION_KNIGHT:
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_QUEEN:
		assert(!MoveHelper::IsPawnMove(m));  // move is recorded with the promoted piece type
		if (MoveHelper::IsCapture(m))
			remove_piece_from_board(m.Content, to);
		remove_piece_from_board(PieceHelper::AsPawn(m.MovPiece), from);  // remove the pawn
		add_piece_to_board(m.MovPiece, to);                               // place promoted piece
		break;

	case MoveType::QUEEN_CASTLE:
		assert(from == e1 || from == e8);
		assert(PieceHelper::IsKing(GetPiece(from)));
		assert(MoveHelper::IsKingMove(m));
		switch (to)
		{
		case c1:  // Long castling — move rook from a1|a8 to d1|d8
		case c8:
			assert(PieceHelper::IsNoPiece(GetPiece(from - 1)));
			assert(PieceHelper::IsNoPiece(GetPiece(from - 2)));
			assert(PieceHelper::IsNoPiece(GetPiece(from - 3)));
			assert(PieceHelper::IsOfPiece(GetPiece(from - 4), PieceHelper::AsPiece(ROOK, sideToMove_)));
			move_piece(PieceHelper::AsPiece(ROOK, sideToMove_),
				SquareHelper::Calc(to, -2), SquareHelper::Calc(to, +1));
			break;
		default:
			assert(!"Invalid castling 'to' square");
			break;
		}
		move_piece(m);  // move the king
		break;

	case MoveType::KING_CASTLE:
		assert(from == e1 || from == e8);
		assert(PieceHelper::IsKing(GetPiece(from)));
		assert(MoveHelper::IsKingMove(m));
		switch (to)
		{
		case g1:  // Short castling — move rook from h1|h8 to f1|f8
		case g8:
			assert(PieceHelper::IsNoPiece(GetPiece(from + 1)));
			assert(PieceHelper::IsNoPiece(GetPiece(from + 2)));
			assert(PieceHelper::IsOfPiece(GetPiece(from + 3), PieceHelper::AsPiece(ROOK, sideToMove_)));
			move_piece(PieceHelper::AsPiece(ROOK, sideToMove_),
				SquareHelper::Calc(to, +1), SquareHelper::Calc(to, -1));
			break;
		default:
			assert(!"Invalid castling 'to' square");
			break;
		}
		move_piece(m);  // move the king
		break;

	default:
		assert(!"Unsupported move type");
		break;
	}

	// Update castling rights
	const uint8_t oldCastlingRights = gameInfo_.castlingRights;

	if (MoveHelper::IsKingMove(m)) {
		if (sideToMove_ == eColor::WHITE)
			gameInfo_.castlingRights &= ~CastlingRights::WHITE_BOTH;
		else
			gameInfo_.castlingRights &= ~CastlingRights::BLACK_BOTH;
	}

	if (MoveHelper::IsMoveType(m, ROOK)) {
		if      (from == a1) gameInfo_.castlingRights &= ~CastlingRights::WHITE_QUEENSIDE;
		else if (from == h1) gameInfo_.castlingRights &= ~CastlingRights::WHITE_KINGSIDE;
		else if (from == a8) gameInfo_.castlingRights &= ~CastlingRights::BLACK_QUEENSIDE;
		else if (from == h8) gameInfo_.castlingRights &= ~CastlingRights::BLACK_KINGSIDE;
	}

	// Capturing a rook on its starting square also revokes castling rights
	if      (to == a1) gameInfo_.castlingRights &= ~CastlingRights::WHITE_QUEENSIDE;
	else if (to == h1) gameInfo_.castlingRights &= ~CastlingRights::WHITE_KINGSIDE;
	else if (to == a8) gameInfo_.castlingRights &= ~CastlingRights::BLACK_QUEENSIDE;
	else if (to == h8) gameInfo_.castlingRights &= ~CastlingRights::BLACK_KINGSIDE;

	if (oldCastlingRights != gameInfo_.castlingRights)
		update_zobrist_castling(oldCastlingRights, gameInfo_.castlingRights);

	// Update en-passant square
	const eSquare oldEpSquare = gameInfo_.epSquare;
	gameInfo_.epSquare = MoveHelper::GetEnPassantSquare(m);
	if (oldEpSquare != gameInfo_.epSquare)
		update_zobrist_ep(oldEpSquare, gameInfo_.epSquare);

	update_threefold_rep(m);

	if (sideToMove_ == eColor::BLACK)
		gameInfo_.fullMoveCount++;

	currentPly_++;

	// Roll back if the move leaves our own king in check
	if (InCheck()) {
		change_player();
		push_position();
		UndoMove(m);
		return false;
	}

	change_player();
	push_position();
	return true;
}

// Mirror of DoMove(). Assumes the current player is the one who did NOT make the move.
void Board::UndoMove(const Move& m)
{
	assert(MoveHelper::IsValid(m));
	assert(GetCurrentColor() != PieceHelper::Color(m.MovPiece));

	currentPly_--;

	// Restore saved state
	gameInfo_                = gameInfoHistory_[currentPly_];
	last_irreversible_ply_   = irreversiblePlyHistory_[currentPly_];
	zobrist_hash_            = zobrist_history_[currentPly_];
	const auto capturedPiece = capturedHistory_[currentPly_];
	const auto from			 = m.from();
	const auto to			 = m.to();
	const auto movingPiece	 = m.MovPiece;

	sideToMove_ = (sideToMove_ == eColor::WHITE) ? eColor::BLACK : eColor::WHITE;

	if (sideToMove_ == eColor::BLACK)
		gameInfo_.fullMoveCount--;

	pop_position();

	switch (MoveHelper::AsType(m))
	{
	case MoveType::QUIET:
		assert(!PieceHelper::IsActual(capturedPiece));
		move_piece(movingPiece, to, from);
		break;

	case MoveType::CAPTURE:
		assert(MoveHelper::IsCapture(m));
		move_piece(movingPiece, to, from);
		add_piece_to_board(capturedPiece, to);
		break;

	case MoveType::DOUBLE_PAWN_PUSH:
		assert(sideToMove_ == PieceHelper::Color(movingPiece));
		assert(!PieceHelper::IsActual(capturedPiece));
		assert(MoveHelper::IsPawnMove(m));
		move_piece(movingPiece, to, from);
		break;

	case MoveType::EP_CAPTURE:
		assert(MoveHelper::IsPawnMove(m));
		assert(MoveHelper::IsCapture(m) && PieceHelper::IsPawn(capturedPiece));
		move_piece(movingPiece, to, from);
		// Restore captured pawn on the square it was taken from (behind the destination)
		if (sideToMove_ == WHITE)
			add_piece_to_board(ePiece::BLACK_PAWN, SquareHelper::Calc(to, +ONE_ROW));
		else
			add_piece_to_board(ePiece::WHITE_PAWN, SquareHelper::Calc(to, -ONE_ROW));
		break;

	case MoveType::PROMOTION_KNIGHT:
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_QUEEN:
		assert(!MoveHelper::IsPawnMove(m));
		remove_piece_from_board(movingPiece, to);    // remove promoted piece
		if (MoveHelper::IsCapture(m)) {
			assert(PieceHelper::IsActual(capturedPiece));
			add_piece_to_board(capturedPiece, to);      // restore captured piece
		}
		add_piece_to_board(PieceHelper::AsPawn(movingPiece), from);  // restore pawn
		break;

	case MoveType::QUEEN_CASTLE:
		assert(from == e1 || from == e8);
		assert(PieceHelper::IsKing(GetPiece(to)));
		assert(MoveHelper::IsKingMove(m));
		switch (to)
		{
		case c1:  // Move rook back from d1|d8 to a1|a8
		case c8:
			assert(PieceHelper::IsNoPiece(GetPiece(from)));
			assert(PieceHelper::IsNoPiece(GetPiece(to - 1)));
			assert(PieceHelper::IsNoPiece(GetPiece(to - 2)));
			move_piece(PieceHelper::AsPiece(ROOK, sideToMove_),
				SquareHelper::Calc(to, +1), SquareHelper::Calc(to, -2));
			break;
		default:
			assert(!"Invalid castling 'to' square");
			break;
		}
		move_piece(movingPiece, to, from);  // restore king
		break;

	case MoveType::KING_CASTLE:
		assert(from == e1 || from == e8);
		assert(PieceHelper::IsKing(GetPiece(to)));
		assert(MoveHelper::IsKingMove(m));
		switch (to)
		{
		case g1:  // Move rook back from f1|f8 to h1|h8
		case g8:
			assert(PieceHelper::IsNoPiece(GetPiece(from)));
			assert(PieceHelper::IsNoPiece(GetPiece(to + 1)));
			move_piece(PieceHelper::AsPiece(ROOK, sideToMove_),
				SquareHelper::Calc(to, -1), SquareHelper::Calc(to, +1));
			break;
		default:
			assert(!"Invalid castling 'to' square");
			break;
		}
		move_piece(movingPiece, to, from);  // restore king
		break;

	default:
		assert(!"Unsupported move type");
	}
}

// Returns true if the side that just moved left their own king in check.
bool Board::InCheck() const noexcept
{
	// Generate attacks for the opponent to see if our king is under attack
	const eColor byColor = (sideToMove_ == WHITE ? BLACK : WHITE);
	const BITBOARD bb = MoveGenerator::GetAttackBoard(byColor);
	return Bits::isAnyBitSet(bb, bitboards_.at(static_cast<BITBOARD>(KING) + sideToMove_));
}

// Adds a piece and maintains material score.
void Board::add_piece_to_board(ePiece piece, eSquare sq)
{
	add_piece(sq, piece);
	material_score_[PieceHelper::Color(piece)] += PieceHelper::Value(piece);
}

// Removes a piece and maintains material score.
void Board::remove_piece_from_board(ePiece piece, eSquare sq)
{
	remove_piece(sq, piece);
	material_score_[PieceHelper::Color(piece)] -= PieceHelper::Value(piece);
}

// Moves a piece without updating material score (for quiet moves, castling rook moves).
void Board::move_piece(const Move& move)
{
	remove_piece(move.from(), move.MovPiece);
	add_piece(move.to(), move.MovPiece);
}

void Board::move_piece(ePiece piece, eSquare from, eSquare to)
{
	remove_piece(from, piece);
	add_piece(to, piece);
}

// ============================================================================
// Utility methods
// ============================================================================

// Marks a move as irreversible (pawn move or capture) and updates the fifty-move counter.
void Board::update_threefold_rep(const Move& m)
{
	if (MoveHelper::IsPawnMove(m) || MoveHelper::IsCapture(m)) {
		last_irreversible_ply_ = position_history_.size();
		gameInfo_.fiftyCount   = 0;
	}
	else {
		gameInfo_.fiftyCount++;
	}
}

// Verifies that bitboards_.at(ALL_PIECES) equals the OR of all individual piece bitboards.
bool Board::test_bitboards(std::ostream& stream) const
{
	BITBOARD bbOR = 0;

	for (auto i = 0; i < ALL_PIECETYPES; i++)
		bbOR |= bitboards_.at(i);

	if (bbOR != bitboards_.at(ALL_PIECES)) {
		stream << "Individual boards OR'd together:\n";
		BitBoardHelper::PrintBitboardBinary(bbOR, stream);
		print_all_bitboards(bitboards_, stream);
		return false;
	}
	return true;
}

void Board::print_all_bitboards(const TBitboards& boards, std::ostream& stream) const
{
	stream << *this;

	stream << "ALL_BLACK_PIECES\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::ALL_BLACK_PIECES), stream);
	stream << "ALL_WHITE_PIECES\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::ALL_WHITE_PIECES), stream);
	stream << "ALL_PIECES\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ALL_PIECES), stream);
	stream << "WHITE_KING\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_KING), stream);
	stream << "BLACK_KING\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_KING), stream);
	stream << "WHITE_PAWN\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_PAWN), stream);
	stream << "BLACK_PAWN\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_PAWN), stream);
	stream << "WHITE_BISHOP\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_BISHOP), stream);
	stream << "BLACK_BISHOP\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_BISHOP), stream);
	stream << "WHITE_QUEEN\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_QUEEN), stream);
	stream << "BLACK_QUEEN\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_QUEEN), stream);
	stream << "WHITE_ROOK\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_ROOK), stream);
	stream << "BLACK_ROOK\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_ROOK), stream);
	stream << "WHITE_KNIGHT\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::WHITE_KNIGHT), stream);
	stream << "BLACK_KNIGHT\n";
	BitBoardHelper::PrintBitboardBinary(boards.at(ePiece::BLACK_KNIGHT), stream);
}

// Prints the board to the stream as an 8x8 ASCII grid.
std::ostream& operator<<(std::ostream& os, const Board& board)
{
	constexpr std::size_t numRanks = 8;
	constexpr std::size_t numFiles = 8;

	for (unsigned int rank = 0; rank < numRanks; ++rank) {
		os << ONE_ROW - rank << " ";

		for (unsigned int file = 0; file < numFiles; ++file) {
			const BITBOARD squareMask = g_bbMask[(rank << 3) + file];

			std::size_t piece = 0;
			while ((piece < ALL_PIECETYPES) && ((squareMask & board.bitboards_[piece]) == 0))
				++piece;

			if (piece >= ALL_PIECETYPES)
				piece = ALL_PIECETYPES;

			os << " " << g_cPieceNames[piece];
		}
		os << '\n';
	}

	os << "\n   A B C D E F G H\n\n";
	return os;
}

/**
 * Returns true if the current position is a repetition draw.
 *
 * Twofold repetition (position seen once before) counts as a draw when both
 * occurrences are within the current search tree. Threefold repetition always
 * counts as a draw regardless. Only same-side-to-move positions are compared
 * (stepping by 2). Search stops at the last irreversible move.
 */
bool Board::is_repetition(int ply) const
{
	int repetitions = 0;

	const size_t history_size = position_history_.size();
	if (history_size < 4)
		return false;

	for (size_t i = history_size - 3;
		i >= last_irreversible_ply_ && i < history_size;
		i -= 2)
	{
		if (position_history_[i] == zobrist_hash_) {
			repetitions++;

			// Entry at index i is from the current search iff i >= history at root
			const bool both_in_search = (ply > 0) &&
				(i >= history_size - static_cast<size_t>(ply));

			if (both_in_search && repetitions >= 1)
				return true;

			if (repetitions >= 2)
				return true;
		}
	}

	return false;
}

void Board::push_position()
{
	position_history_.push_back(zobrist_hash_);
}

void Board::pop_position()
{
	if (!position_history_.empty())
		position_history_.pop_back();
}

void Board::reset_repetition_history()
{
	position_history_.clear();
	last_irreversible_ply_ = 0;
}

void Board::update_zobrist_castling(uint8_t old_rights, uint8_t new_rights) noexcept
{
	zobrist_hash_ ^= zobrist::castling_keys[old_rights];
	zobrist_hash_ ^= zobrist::castling_keys[new_rights];
}

void Board::update_zobrist_ep(eSquare old_ep, eSquare new_ep) noexcept
{
	if (old_ep != NO_SQUARE) zobrist_hash_ ^= zobrist::ep_keys[old_ep];
	if (new_ep != NO_SQUARE) zobrist_hash_ ^= zobrist::ep_keys[new_ep];
}

void Board::update_zobrist_side() noexcept
{
	zobrist_hash_ ^= zobrist::side_key;
}
