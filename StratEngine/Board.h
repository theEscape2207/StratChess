#pragma once

#include "BitBoardHelper.h"
#include "Move.h"
#include "GameState.h"
#include "PieceHelper.h"
#include <span>
#include <vector>

class Board final
{
	friend std::ostream& operator<<(std::ostream&, const Board&);

public:
	// Singleton accessor
	static inline Board& Instance() noexcept
	{
		static Board _instance;
		return _instance;
	}

	// --- Position setup ---
	void SetDefaultBoard();
	void SetupFromFEN(const std::string& fen);
	std::string ExtractFEN() const;

	// --- Move execution ---
	bool DoMove(const Move&);
	void UndoMove(const Move&);

	// --- Position queries ---
	bool InCheck() const noexcept;

	// Returns the effective moving piece for a move that has NOT yet been applied.
	// For non-promotion moves: the piece currently on m.from().
	// For promotion moves: the promoted piece (derived from MoveType flags + pawn color).
	ePiece GetEffectiveMovPiece(const Move& m) const noexcept;

	// Returns the piece captured by this move (before it is applied to the board).
	// For CAPTURE / PROMOTION_*_CAPTURE: the piece on move.to().
	// For EP_CAPTURE: the opposite-side pawn (not on move.to()).
	// For all other move types: NO_PIECE.
	ePiece GetCapturedPiece(const Move& m) const noexcept;

	// Returns true if any square covered by mask is occupied
	bool IsOccupied(BITBOARD mask) const noexcept
	{
		return Bits::isAnyBitSet(bitboards_.at(ALL_PIECES), mask);
	}

	bool IsLegalMove(const Move& move)
	{
		if (!DoMove(move)) return false;
		UndoMove(move);
		return true;
	}

	bool is_repetition(int ply) const;

	// --- State accessors ---
	eColor GetCurrentColor() const noexcept { return sideToMove_; }

	ePiece GetPiece(eSquare square) const noexcept { return mailbox_.at(square); }
	ePiece GetPiece(int square) const noexcept     { return mailbox_.at(static_cast<eSquare>(square)); }

	int GetMaterialScore(eColor color) const noexcept { return material_score_[color]; }

	GameInfo GetGameInfo() const noexcept          { return gameInfo_; }
	void SetGameState(GameStates state) noexcept   { gameInfo_.gameState = state; }

	uint64_t get_zobrist_hash() const noexcept     { return zobrist_hash_; }

	std::span<BITBOARD> GetBitBoards() noexcept;

	// Returns the index of the least-significant set bit (square of first piece).
	// Precondition: mask != 0. std::countr_zero compiles to TZCNT on x64.
	static __forceinline eSquare GetFirstPiece(BITBOARD mask) noexcept
	{
		assert(mask != 0);
		return static_cast<eSquare>(std::countr_zero(mask));
	}

	// --- Test setup helpers (prefer SetupFromFEN for new tests) ---

	// Non-copyable / non-movable (singleton)
	Board& operator=(const Board&) = delete;
	Board(const Board&) = delete;
	Board(Board&&) = delete;
	Board& operator=(Board&&) = delete;

private:
	Board();
	~Board() = default;

	using TBitboards = std::array<BITBOARD, ALL_BITBOARDS>;
	using sqPieces   = std::tuple<ePiece, eSquare>;
	using squareCol  = std::vector<sqPieces>;

	// --- Internal position setup ---
	void setup_board(const squareCol&);
	void clear_board();
	
	// --- Low-level piece manipulation (bitboard + mailbox, no material update) ---
	// Note: these also update zobrist_hash_ as a side-effect.
	void add_piece(eSquare square, ePiece piece);
	void remove_piece(eSquare square, ePiece piece);
	void move_piece(ePiece piece, eSquare from, eSquare to);

	// --- Material-tracking piece operations ---
	void add_piece_to_board(ePiece piece, eSquare sq);
	void remove_piece_from_board(ePiece piece, eSquare sq);

	// --- Mailbox helpers ---
	void clear_square(eSquare square) noexcept
	{
		assert(PieceHelper::IsNotEmpty(GetPiece(square)));
		mailbox_[square] = ePiece::NO_PIECE;
	}

	void set_square(eSquare square, ePiece piece) noexcept
	{
		assert(PieceHelper::IsNoPiece(GetPiece(square)));
		mailbox_[square] = piece;
	}

	ePiece get_captured_piece(const Move& move) const noexcept
	{
		if (!MoveHelper::IsCapture(move))
			return ePiece::NO_PIECE;
		return (move.flags() == MoveFlags::EP_CAPTURE) ? 
			PieceHelper::OppositePawn(sideToMove_) : // EP capture is a pawn, but the captured piece is not on the destination square
			mailbox_[move.to()];
	}

	// --- Bitboard helpers ---
	bool clear_bitboard_square(TBitboards::size_type iBoard, eSquare square)
	{
		return BitBoardHelper::ClearBitboardMask(bitboards_[iBoard], g_bbMask[square]);
	}

	void set_bitboard_square(TBitboards::size_type iPiece, eSquare square) noexcept
	{
		BitBoardHelper::SetBitboardMask(bitboards_[iPiece], g_bbMask[square]);
	}

	// Returns the bitboard array index for a piece type + color combination
	static constexpr size_t bitboard_index(ePieceType piece, eColor color) noexcept
	{
		return static_cast<size_t>(piece) + static_cast<size_t>(color);
	}

	// --- Zobrist hash helpers ---
	void update_zobrist_castling(uint8_t old_rights, uint8_t new_rights) noexcept;
	void update_zobrist_ep(eSquare old_ep, eSquare new_ep) noexcept;
	void update_zobrist_side() noexcept;
	void change_player() noexcept
	{
		sideToMove_ = (sideToMove_ == eColor::WHITE) ? eColor::BLACK : eColor::WHITE;
		update_zobrist_side();
	}

	// --- Repetition tracking ---
	void update_threefold_rep(const Move&, ePiece movPiece);
	void push_position();
	void pop_position();
	void reset_repetition_history();

	// --- Debug ---
	bool test_bitboards(std::ostream& stream = std::cout) const;
	void print_all_bitboards(const TBitboards& boards, std::ostream& = std::cout) const;

	// ---- Member variables ----

	eColor   sideToMove_{ eColor::WHITE };
	GameInfo gameInfo_;

	std::array<ePiece, ALL_SQUARES> mailbox_{};
	TBitboards bitboards_{ { 0 } };

	size_t currentPly_{ 0 };

	// Repetition tracking
	std::vector<uint64_t> position_history_;
	size_t last_irreversible_ply_{ 0 };

	int material_score_[NUM_COLORS]{ 0 };

	// Ply-indexed undo state (pre-allocated to MAX_PLY for O(1) unmake)
	std::array<uint64_t, MAX_PLY>  zobrist_history_{ 0 };
	std::array<size_t,   MAX_PLY>  irreversiblePlyHistory_{ 0 };
	std::array<GameInfo, MAX_PLY>  gameInfoHistory_{};
	std::array<ePiece,   MAX_PLY>  capturedHistory_{ ePiece::NO_PIECE };

	uint64_t zobrist_hash_{ 0 };
};

// ============================================================================
// Zobrist key tables for castling, en-passant and side-to-move
// Declared here, defined in Board.cpp.
// ============================================================================
namespace zobrist {
	extern std::array<std::array<uint64_t, NUM_SQUARES>, ALL_PIECETYPES> piece_keys;
	extern std::array<uint64_t, 16> castling_keys;
	extern std::array<uint64_t, NUM_SQUARES> ep_keys;
	extern uint64_t side_key;

	void initialize() noexcept;
}
