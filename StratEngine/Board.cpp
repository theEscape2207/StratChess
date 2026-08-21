// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Board.h"
#include "MoveGenerator.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "Utils/FENParser.h"
#include <random>

// Zobrist key tables (defined here, declared in Board.h)
namespace zobrist {
	std::array<std::array<uint64_t, NUM_SQUARES>, ALL_PIECETYPES> piece_keys;
	std::array<uint64_t, 16> castling_keys;
	std::array<uint64_t, NUM_SQUARES> ep_keys;
	uint64_t side_key;

	void initialize() noexcept
	{
		// Fills the global key tables exactly once (thread-safe magic static).
		// Every Board instance must see identical keys, or zobrist hashes computed
		// from different Board objects (e.g. thread-local boards under Lazy SMP)
		// would disagree for the same position. This is the only lazy/runtime
		// init in the attack/Zobrist table set; a C++11 function-local static
		// initializer is guaranteed thread-safe by the standard, so concurrent
		// Board construction from multiple helper threads is race-free. See
		// Magic.h for the sliding-piece attack tables, which need no such
		// guard at all — they are `inline constexpr`, fully resolved at
		// compile time with no runtime initialization step whatsoever.
		static const bool once = [] {
			// non-deterministic seed for production use (uncomment for true randomness, but beware non-reproducible hashes across runs)
			// std::random_device rd;
			// std::mt19937 rng(rd());
			// Deterministic seed for reproducibility -- not security-sensitive, so deliberate.
			std::mt19937_64 rng(0x123456789ABCDEF0ULL); // NOLINT(bugprone-random-generator-seed)

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
			return true;
		}();
		(void)once;
	}
} // namespace zobrist

Board::Board()
{
	mailbox_.fill(ePiece::NO_PIECE);
	zobrist::initialize();
}

Board::Board(const std::string& fen) : Board()
{
	[[maybe_unused]] const bool ok = SetupFromFEN(fen);
	assert(ok && "Board(fen): malformed FEN, board left empty");
}

void Board::clear_board()
{
	bitboards_.fill(0);
	mailbox_.fill(ePiece::NO_PIECE);

	sideToMove_ = WHITE;
	state_ = PositionState{};
	reset_repetition_history();
	currentPly_ = 0;

	state_history_.fill(PositionState{});
	material_score_[WHITE] = material_score_[BLACK] = 0;
	zobrist_hash_ = 0;
}

// Adds a piece to all relevant bitboards, the mailbox, and the Zobrist hash.
// Does NOT update material score — use add_piece_to_board for that.
void Board::add_piece(eSquare square, ePiece piece)
{
	set_bitboard_square(piece, square);
	set_bitboard_square(bitboard_index(ALL_FROM_COLOR, PieceHelper::Color(piece)), square);
	set_bitboard_square(ALL_PIECES, square);

	set_square(square, piece);
	zobrist_hash_ ^= zobrist::piece_keys[piece][square];

	assert(test_bitboards());
}

// Removes a piece from all relevant bitboards, the mailbox, and the Zobrist hash.
// Does NOT update material score — use remove_piece_from_board for that.
void Board::remove_piece(eSquare square, ePiece piece)
{
	assert(GetPiece(square) == piece);

	if (!clear_bitboard_square(piece, square))
		test_bitboards();

	clear_bitboard_square(bitboard_index(ALL_FROM_COLOR, PieceHelper::Color(piece)), square);
	clear_bitboard_square(ALL_PIECES, square);

	clear_square(square);
	zobrist_hash_ ^= zobrist::piece_keys[piece][square];

	assert(test_bitboards());
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

// The one rule so far: the side not to move may not be in check, since reaching such a position would
// have required leaving a king en prise. Kings on adjacent squares fail it too, because GetAttackBoard
// includes king attacks. Any further rule belongs here, and only needs the scratch board below.
bool Board::position_is_legal(const squareCol& pieces, eColor sideToMove)
{
	// A scratch board, because attack generation needs a populated one and the caller's board may not
	// be modified until the position is known to be legal. Placement and side to move are all the
	// query reads; castling rights and en-passant cannot make a king attacked.
	Board probe;
	probe.setup_board(pieces);
	probe.sideToMove_ = sideToMove;

	return !probe.WaitingSideInCheck();
}

bool Board::SetupFromFEN(const std::string& fen) { return setup_from_fen_impl(fen, nullptr); }

bool Board::SetupFromFEN(const std::string& fen, std::vector<std::string>& repairs)
{
	repairs.clear();
	return setup_from_fen_impl(fen, &repairs);
}

bool Board::setup_from_fen_impl(const std::string& fen, std::vector<std::string>* repairs)
{
	FENParser::FENGameState state;
	std::vector<std::tuple<ePiece, eSquare>> pieces;

	// Nothing below this point runs on a parse error, so the board keeps its previous contents.
	auto parseError = FENParser::ParseFEN(fen, state, pieces);
	if (parseError) {
		spdlog::default_logger()->error("FEN parse error: {}", *parseError);
		return false;
	}

	// Checked before anything is applied, so a rejected FEN leaves this board as it was.
	if (!position_is_legal(pieces, state.sideToMove)) {
		spdlog::default_logger()->error("FEN describes an illegal position (the side not to move is in check): {}",
		                                fen);
		return false;
	}

	setup_board(pieces);

	sideToMove_ = state.sideToMove;
	state_.ep_square = state.epSquare;
	state_.castling_rights = state.castlingRights;
	state_.halfmove_clock = static_cast<uint16_t>(state.halfMoveClock);
	state_.fullmove_count = static_cast<uint16_t>(state.fullMoveCounter);
	state_.last_move.Clear();

	// Validate parsed metadata and adjust castling/EP if inconsistent with actual pieces
	FENParser::ValidatePositionAgainstFENMetadata(*this, state, repairs);

	state_.ep_square = state.epSquare;
	state_.castling_rights = state.castlingRights;

	// Seed the repetition history with the position itself. push_position() is otherwise
	// only reached from DoMove, so without this a board set up from a FEN carries an empty
	// history: a line returning to its own starting position is invisible, and a game-level
	// three-fold whose first occurrence is the start position needs a fourth to be claimed.
	push_position();

	spdlog::default_logger()->debug("Board set up from FEN: {}", fen);
	return true;
}

std::string Board::ExtractFEN() const
{
	std::string fen;

	// 1. Piece placement (rank 8 → rank 1)
	for (int rank = 0; rank < 8; ++rank) {
		int emptyCount = 0;

		for (int file = 0; file < 8; ++file) {
			eSquare square = static_cast<eSquare>((rank << 3) + file);
			ePiece piece = GetPiece(square);

			if (piece == ePiece::NO_PIECE) {
				emptyCount++;
			} else {
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
	if (state_.castling_rights == CastlingRights::NONE) {
		fen += '-';
	} else {
		if (state_.castling_rights & CastlingRights::WHITE_KINGSIDE)
			fen += 'K';
		if (state_.castling_rights & CastlingRights::WHITE_QUEENSIDE)
			fen += 'Q';
		if (state_.castling_rights & CastlingRights::BLACK_KINGSIDE)
			fen += 'k';
		if (state_.castling_rights & CastlingRights::BLACK_QUEENSIDE)
			fen += 'q';
	}

	// 4. En passant target square
	fen += ' ';
	if (state_.ep_square == NO_SQUARE) {
		fen += '-';
	} else {
		int file = File(state_.ep_square);
		int rank = 7 - Rank(state_.ep_square);
		fen += static_cast<char>('a' + file);
		fen += static_cast<char>('1' + rank);
	}

	// 5. Halfmove clock
	fen += ' ';
	fen += std::to_string(state_.halfmove_clock);

	// 6. Fullmove number
	fen += ' ';
	fen += std::to_string(state_.fullmove_count);

	return fen;
}

void Board::SetDefaultBoard()
{
	clear_board();

	// Black pieces
	add_piece_to_board(ePiece::BLACK_ROOK, a8);
	add_piece_to_board(ePiece::BLACK_KNIGHT, b8);
	add_piece_to_board(ePiece::BLACK_BISHOP, c8);
	add_piece_to_board(ePiece::BLACK_QUEEN, d8);
	add_piece_to_board(ePiece::BLACK_KING, e8);
	add_piece_to_board(ePiece::BLACK_BISHOP, f8);
	add_piece_to_board(ePiece::BLACK_KNIGHT, g8);
	add_piece_to_board(ePiece::BLACK_ROOK, h8);

	for (int i = a7; i <= h7; i++)
		add_piece_to_board(ePiece::BLACK_PAWN, static_cast<eSquare>(i));

	// White pieces
	for (int i = a2; i <= h2; i++)
		add_piece_to_board(ePiece::WHITE_PAWN, static_cast<eSquare>(i));

	add_piece_to_board(ePiece::WHITE_ROOK, a1);
	add_piece_to_board(ePiece::WHITE_KNIGHT, b1);
	add_piece_to_board(ePiece::WHITE_BISHOP, c1);
	add_piece_to_board(ePiece::WHITE_QUEEN, d1);
	add_piece_to_board(ePiece::WHITE_KING, e1);
	add_piece_to_board(ePiece::WHITE_BISHOP, f1);
	add_piece_to_board(ePiece::WHITE_KNIGHT, g1);
	add_piece_to_board(ePiece::WHITE_ROOK, h1);

	spdlog::default_logger()->debug("Default board set up");
}

std::span<const BITBOARD> Board::GetBitBoards() const noexcept { return std::span(bitboards_); }

bool Board::DoMove(const Move& m)
{
	// Defense in depth: currentPly_ should never reach MAX_PLY now that
	// ResetSearchDepth() decouples it from total game length (issue #53).
	assert(currentPly_ < MAX_PLY);

	// capturedPiece must be computed before IsValid (which uses it) and before any board changes.
	const auto capturedPiece = get_captured_piece(m);

	const ePiece movPiece = GetEffectiveMovPiece(m);
	assert(MoveHelper::IsValid(m, movPiece, capturedPiece));
	assert(GetCurrentColor() == PieceHelper::Color(movPiece));

	// Save state for UndoMove
	snapshot_state(capturedPiece);

	const eSquare from = m.from();
	const eSquare to = m.to();

	switch (MoveHelper::AsType(m)) {
	case MoveType::QUIET:
		assert(!PieceHelper::IsActual(capturedPiece));
		move_piece(movPiece, from, to);
		break;

	case MoveType::CAPTURE:
		assert(MoveHelper::IsCapture(m));
		remove_piece_from_board(capturedPiece, to);
		move_piece(movPiece, from, to);
		break;

	case MoveType::DOUBLE_PAWN_PUSH:
		assert(!PieceHelper::IsActual(capturedPiece));
		assert(MoveHelper::IsPawnMove(movPiece));
		move_piece(movPiece, from, to);
		break;

	case MoveType::EP_CAPTURE: {
		assert(MoveHelper::IsPawnMove(movPiece));
		assert(MoveHelper::IsCapture(m) && PieceHelper::IsPawn(capturedPiece));
		// Captured pawn sits one rank behind the destination square
		const eSquare epCapturedPawnSquare = SquareHelper::PreviousRow(to, sideToMove_);
		remove_piece_from_board(capturedPiece, epCapturedPawnSquare);
		move_piece(movPiece, from, to);
		break;
	}

	case MoveType::PROMOTION_KNIGHT:
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_QUEEN:
	case MoveType::PROMOTION_KNIGHT_CAPTURE:
	case MoveType::PROMOTION_BISHOP_CAPTURE:
	case MoveType::PROMOTION_ROOK_CAPTURE:
	case MoveType::PROMOTION_QUEEN_CAPTURE:
		assert(!MoveHelper::IsPawnMove(movPiece)); // movPiece is the promoted piece type
		if (MoveHelper::IsCapture(m))
			remove_piece_from_board(capturedPiece, to);
		remove_piece_from_board(PieceHelper::AsPawn(movPiece), from); // remove the pawn
		add_piece_to_board(movPiece, to);                             // place promoted piece
		break;

	case MoveType::QUEEN_CASTLE:
		assert(from == e1 || from == e8);
		assert(PieceHelper::IsKing(GetPiece(from)));
		assert(PieceHelper::IsKing(movPiece));
		switch (to) {
		case c1: // Long castling — move rook from a1|a8 to d1|d8
		case c8:
			assert(PieceHelper::IsNoPiece(GetPiece(from - 1)));
			assert(PieceHelper::IsNoPiece(GetPiece(from - 2)));
			assert(PieceHelper::IsNoPiece(GetPiece(from - 3)));
			assert(PieceHelper::IsOfPiece(GetPiece(from - 4), PieceHelper::AsPiece(ROOK, sideToMove_)));
			move_piece(PieceHelper::AsPiece(ROOK, sideToMove_), SquareHelper::Calc(to, -2), SquareHelper::Calc(to, +1));
			break;
		default:
			assert(!"Invalid castling 'to' square");
			break;
		}
		move_piece(movPiece, from, to); // move the king
		break;

	case MoveType::KING_CASTLE:
		assert(from == e1 || from == e8);
		assert(PieceHelper::IsKing(GetPiece(from)));
		assert(PieceHelper::IsKing(movPiece));
		switch (to) {
		case g1: // Short castling — move rook from h1|h8 to f1|f8
		case g8:
			assert(PieceHelper::IsNoPiece(GetPiece(from + 1)));
			assert(PieceHelper::IsNoPiece(GetPiece(from + 2)));
			assert(PieceHelper::IsOfPiece(GetPiece(from + 3), PieceHelper::AsPiece(ROOK, sideToMove_)));
			move_piece(PieceHelper::AsPiece(ROOK, sideToMove_), SquareHelper::Calc(to, +1), SquareHelper::Calc(to, -1));
			break;
		default:
			assert(!"Invalid castling 'to' square");
			break;
		}
		move_piece(movPiece, from, to); // move the king
		break;

	default:
		assert(!"Unsupported move type");
		break;
	}

	// Update castling rights
	const uint8_t oldCastlingRights = state_.castling_rights;

	if (PieceHelper::IsKing(movPiece)) {
		if (sideToMove_ == eColor::WHITE)
			state_.castling_rights &= ~CastlingRights::WHITE_BOTH;
		else
			state_.castling_rights &= ~CastlingRights::BLACK_BOTH;
	}

	if (PieceHelper::IsOfType(movPiece, ROOK)) {
		if (from == a1)
			state_.castling_rights &= ~CastlingRights::WHITE_QUEENSIDE;
		else if (from == h1)
			state_.castling_rights &= ~CastlingRights::WHITE_KINGSIDE;
		else if (from == a8)
			state_.castling_rights &= ~CastlingRights::BLACK_QUEENSIDE;
		else if (from == h8)
			state_.castling_rights &= ~CastlingRights::BLACK_KINGSIDE;
	}

	// Capturing a rook on its starting square also revokes castling rights
	if (to == a1)
		state_.castling_rights &= ~CastlingRights::WHITE_QUEENSIDE;
	else if (to == h1)
		state_.castling_rights &= ~CastlingRights::WHITE_KINGSIDE;
	else if (to == a8)
		state_.castling_rights &= ~CastlingRights::BLACK_QUEENSIDE;
	else if (to == h8)
		state_.castling_rights &= ~CastlingRights::BLACK_KINGSIDE;

	if (oldCastlingRights != state_.castling_rights)
		update_zobrist_castling(oldCastlingRights, state_.castling_rights);

	// Update en-passant square
	const eSquare oldEpSquare = state_.ep_square;
	state_.ep_square = MoveHelper::GetEnPassantSquare(m, movPiece);
	if (oldEpSquare != state_.ep_square)
		update_zobrist_ep(oldEpSquare, state_.ep_square);

	// The move that produced the position the board now holds. Move ordering is its only
	// consumer: MoveSorter front-loads recaptures on the square the opponent just moved to,
	// so a searcher needs "what was played to get here" at every ply, and the board is the
	// only object that knows it for a position it has actually reached. It travels with the
	// rest of the ply's PositionState, so the rollback below and UndoMove both restore the
	// preceding move without any handling of their own.
	state_.last_move = m;

	update_threefold_rep(m, movPiece);

	if (sideToMove_ == eColor::BLACK) {
		assert(state_.fullmove_count < UINT16_MAX); // a game this long is a bug, not an input
		state_.fullmove_count++;
	}

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

// Call after a move that will never be undone (a real game move, or a UCI
// position replay). currentPly_ then only ever has to span the depth of an
// in-flight search excursion, never the length of the whole game.
void Board::ResetSearchDepth() noexcept { currentPly_ = 0; }

// Mirror of DoMove(). Assumes the current player is the one who did NOT make the move.
void Board::UndoMove(const Move& m)
{
	currentPly_--;

	// The moving piece is currently on m.to() (placed there by DoMove); read before any state changes.
	const ePiece movingPiece = GetPiece(m.to());
	// capturedPiece is needed for IsValid and for restoring the board; read from history now.
	const auto capturedPiece = state_history_[currentPly_].captured_piece;
	assert(MoveHelper::IsValid(m, movingPiece, capturedPiece));
	assert(GetCurrentColor() != PieceHelper::Color(movingPiece));

	// Restore saved state
	last_irreversible_ply_ = state_history_[currentPly_].last_irreversible_ply;
	restore_state();
	const auto from = m.from();
	const auto to = m.to();

	sideToMove_ = (sideToMove_ == eColor::WHITE) ? eColor::BLACK : eColor::WHITE;

	pop_position();

	switch (MoveHelper::AsType(m)) {
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
		assert(MoveHelper::IsPawnMove(movingPiece));
		move_piece(movingPiece, to, from);
		break;

	case MoveType::EP_CAPTURE:
		assert(MoveHelper::IsPawnMove(movingPiece));
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
	case MoveType::PROMOTION_KNIGHT_CAPTURE:
	case MoveType::PROMOTION_BISHOP_CAPTURE:
	case MoveType::PROMOTION_ROOK_CAPTURE:
	case MoveType::PROMOTION_QUEEN_CAPTURE:
		assert(!MoveHelper::IsPawnMove(movingPiece));
		remove_piece_from_board(movingPiece, to); // remove promoted piece
		if (MoveHelper::IsCapture(m)) {
			assert(PieceHelper::IsActual(capturedPiece));
			add_piece_to_board(capturedPiece, to); // restore captured piece
		}
		add_piece_to_board(PieceHelper::AsPawn(movingPiece), from); // restore pawn
		break;

	case MoveType::QUEEN_CASTLE:
		assert(from == e1 || from == e8);
		assert(PieceHelper::IsKing(GetPiece(to)));
		assert(PieceHelper::IsKing(movingPiece));
		switch (to) {
		case c1: // Move rook back from d1|d8 to a1|a8
		case c8:
			assert(PieceHelper::IsNoPiece(GetPiece(from)));
			assert(PieceHelper::IsNoPiece(GetPiece(to - 1)));
			assert(PieceHelper::IsNoPiece(GetPiece(to - 2)));
			move_piece(PieceHelper::AsPiece(ROOK, sideToMove_), SquareHelper::Calc(to, +1), SquareHelper::Calc(to, -2));
			break;
		default:
			assert(!"Invalid castling 'to' square");
			break;
		}
		move_piece(movingPiece, to, from); // restore king
		break;

	case MoveType::KING_CASTLE:
		assert(from == e1 || from == e8);
		assert(PieceHelper::IsKing(GetPiece(to)));
		assert(PieceHelper::IsKing(movingPiece));
		switch (to) {
		case g1: // Move rook back from f1|f8 to h1|h8
		case g8:
			assert(PieceHelper::IsNoPiece(GetPiece(from)));
			assert(PieceHelper::IsNoPiece(GetPiece(to + 1)));
			move_piece(PieceHelper::AsPiece(ROOK, sideToMove_), SquareHelper::Calc(to, -1), SquareHelper::Calc(to, +1));
			break;
		default:
			assert(!"Invalid castling 'to' square");
			break;
		}
		move_piece(movingPiece, to, from); // restore king
		break;

	default:
		assert(!"Unsupported move type");
	}

	// Restore hash last: move_piece/add_piece_to_board/remove_piece_from_board all XOR into
	// zobrist_hash_ as a side-effect, so overwrite with the pre-move snapshot to get the
	// correct hash back.
	zobrist_hash_ = state_history_[currentPly_].zobrist_hash;
}

// Returns the effective moving piece for a move that has NOT yet been applied.
// For non-promotion moves: the piece currently on m.from() (mailbox lookup).
// For promotion moves: the promoted piece derived from MoveType flags and pawn color.
ePiece Board::GetEffectiveMovPiece(const Move& m) const noexcept
{
	const ePiece onBoard = GetPiece(m.from());
	if (!MoveHelper::IsPromote(m))
		return onBoard;
	const eColor color = PieceHelper::Color(onBoard);
	switch (static_cast<MoveType>(m.flags())) {
	case MoveType::PROMOTION_QUEEN:
	case MoveType::PROMOTION_QUEEN_CAPTURE:
		return PieceHelper::AsPiece(QUEEN, color);
	case MoveType::PROMOTION_ROOK:
	case MoveType::PROMOTION_ROOK_CAPTURE:
		return PieceHelper::AsPiece(ROOK, color);
	case MoveType::PROMOTION_BISHOP:
	case MoveType::PROMOTION_BISHOP_CAPTURE:
		return PieceHelper::AsPiece(BISHOP, color);
	case MoveType::PROMOTION_KNIGHT:
	case MoveType::PROMOTION_KNIGHT_CAPTURE:
		return PieceHelper::AsPiece(KNIGHT, color);
	default:
		return onBoard;
	}
}

// Public wrapper around get_captured_piece — used by Sort and external callers.
ePiece Board::GetCapturedPiece(const Move& m) const noexcept { return get_captured_piece(m); }

// Returns true if the king of the side to move is under attack.
bool Board::InCheck() const noexcept
{
	// Generate attacks for the opponent to see if our king is under attack
	const eColor byColor = (sideToMove_ == WHITE ? BLACK : WHITE);
	const BITBOARD bb = MoveGenerator::GetAttackBoard(*this, byColor);
	return Bits::isAnyBitSet(bb, bitboards_.at(static_cast<BITBOARD>(KING) + sideToMove_));
}

// Returns true if the king of the side NOT to move is under attack — the mirror of InCheck(), and
// an illegal position rather than a legal one. Kings on adjacent squares are covered too, since
// GetAttackBoard includes king attacks.
bool Board::WaitingSideInCheck() const noexcept
{
	const eColor waiting = (sideToMove_ == WHITE ? BLACK : WHITE);
	const BITBOARD bb = MoveGenerator::GetAttackBoard(*this, sideToMove_);
	return Bits::isAnyBitSet(bb, bitboards_.at(static_cast<BITBOARD>(KING) + waiting));
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
void Board::move_piece(ePiece piece, eSquare from, eSquare to)
{
	remove_piece(from, piece);
	add_piece(to, piece);
}

// ============================================================================
// Utility methods
// ============================================================================

// Marks a move as irreversible (pawn move or capture) and updates the fifty-move counter.
void Board::update_threefold_rep(const Move& m, ePiece movPiece)
{
	if (MoveHelper::IsPawnMove(movPiece) || MoveHelper::IsCapture(m)) {
		last_irreversible_ply_ = position_history_.size();
		state_.halfmove_clock = 0;
	} else {
		assert(state_.halfmove_clock < UINT16_MAX); // a game this long is a bug, not an input
		state_.halfmove_clock++;
	}
}

// Verifies that bitboards_.at(ALL_PIECES) equals the OR of all individual piece bitboards.
// On failure, emits a detailed diagnostic via the default spdlog logger (error level).
bool Board::test_bitboards() const
{
	BITBOARD bbOR = 0;

	for (auto i = 0; i < ALL_PIECETYPES; i++)
		bbOR |= bitboards_.at(i);

	if (bbOR != bitboards_.at(ALL_PIECES)) {
		std::ostringstream oss;
		oss << "Individual boards OR'd together:\n";
		BitBoardHelper::print_bitboard(oss, bbOR);
		print_all_bitboards(bitboards_, oss);
		spdlog::default_logger()->error("Bitboard corruption detected:\n{}", oss.str());
		return false;
	}
	return true;
}

void Board::print_all_bitboards(const TBitboards& boards, std::ostream& stream) const
{
	stream << *this;

	stream << "ALL_BLACK_PIECES\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::ALL_BLACK_PIECES));
	stream << "ALL_WHITE_PIECES\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::ALL_WHITE_PIECES));
	stream << "ALL_PIECES\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ALL_PIECES));
	stream << "WHITE_KING\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::WHITE_KING));
	stream << "BLACK_KING\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::BLACK_KING));
	stream << "WHITE_PAWN\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::WHITE_PAWN));
	stream << "BLACK_PAWN\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::BLACK_PAWN));
	stream << "WHITE_BISHOP\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::WHITE_BISHOP));
	stream << "BLACK_BISHOP\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::BLACK_BISHOP));
	stream << "WHITE_QUEEN\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::WHITE_QUEEN));
	stream << "BLACK_QUEEN\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::BLACK_QUEEN));
	stream << "WHITE_ROOK\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::WHITE_ROOK));
	stream << "BLACK_ROOK\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::BLACK_ROOK));
	stream << "WHITE_KNIGHT\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::WHITE_KNIGHT));
	stream << "BLACK_KNIGHT\n";
	BitBoardHelper::print_bitboard(stream, boards.at(ePiece::BLACK_KNIGHT));
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

	for (size_t i = history_size - 3; i >= last_irreversible_ply_ && i < history_size; i -= 2) {
		if (position_history_[i] == zobrist_hash_) {
			repetitions++;

			// Entry at index i is from the current search iff i >= history at root.
			// The root position itself sits one index below that, and a line returning
			// to it is a repetition the side to move can force, so it counts too.
			const bool both_in_search = (ply > 0) && (i + 1 >= history_size - static_cast<size_t>(ply));

			if (both_in_search && repetitions >= 1)
				return true;

			if (repetitions >= 2)
				return true;
		}
	}

	return false;
}

void Board::push_position() { position_history_.push_back(zobrist_hash_); }

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
	if (old_ep != NO_SQUARE)
		zobrist_hash_ ^= zobrist::ep_keys[old_ep];
	if (new_ep != NO_SQUARE)
		zobrist_hash_ ^= zobrist::ep_keys[new_ep];
}

void Board::update_zobrist_side() noexcept { zobrist_hash_ ^= zobrist::side_key; }

// Make a null-move: advance side-to-move without changing pieces. This mirrors
// the DoMove/UndoMove save/restore behaviour so search code can treat the
// position stack uniformly.
void Board::DoNullMove()
{
	// Defense in depth: currentPly_ should never reach MAX_PLY now that
	// ResetSearchDepth() decouples it from total game length (issue #53).
	assert(currentPly_ < MAX_PLY);

	// Record snapshot for undo
	snapshot_state(ePiece::NO_PIECE);

	// A null move forfeits any pending en-passant right, exactly like any
	// other non-double-push move does in DoMove() (see GetEnPassantSquare
	// usage there) — otherwise a stale EP square would illegally survive
	// one extra ply inside the null-move subtree.
	if (state_.ep_square != NO_SQUARE) {
		update_zobrist_ep(state_.ep_square, NO_SQUARE);
		state_.ep_square = NO_SQUARE;
	}

	if (sideToMove_ == eColor::BLACK) {
		assert(state_.fullmove_count < UINT16_MAX); // a game this long is a bug, not an input
		state_.fullmove_count++;
	}

	currentPly_++;

	// Switch side and update zobrist via change_player
	change_player();
	push_position();
}

void Board::UndoNullMove()
{
	// Mirror UndoMove's ply handling: decrement currentPly_, restore saved state
	currentPly_--;

	last_irreversible_ply_ = state_history_[currentPly_].last_irreversible_ply;
	restore_state();

	// Restore side-to-move (toggle)
	sideToMove_ = (sideToMove_ == eColor::WHITE) ? eColor::BLACK : eColor::WHITE;

	pop_position();

	// Restore zobrist hash from history
	zobrist_hash_ = state_history_[currentPly_].zobrist_hash;
}
