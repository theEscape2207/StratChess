#pragma once

#include <deque>		// For PVLine
#include "PieceHelper.h"

// Move representation (16-bit encoding)
// Encoding:
// Bits  0- 5: From square (0-63)
// Bits  6-11: To square (0-63)
// Bits 12-15: Move flags (MoveType, 0-15)
//   Flag bit layout: bit3 = promotion, bit2 = capture
// The captured piece (Content) has been removed from the struct (Phase 4).
//   Board::capturedHistory_[] stores it during DoMove/UndoMove for undo.
//   For MVV-LVA sorting, callers use Board::GetCapturedPiece(move).
// The moving piece (MovPiece) was removed in Phase 3.
//   Callers obtain it via Board::GetEffectiveMovPiece() before DoMove.
class Move final
{
    static constexpr uint16_t EMPTY_MOVE = 0xFFFF;

    // Prints content to stream - Implemented in .cpp
    friend std::ostream& operator<<(std::ostream&, _In_ const Move& move);
public:
    // Copy constructor
    constexpr Move(const Move& rhs) noexcept = default;

    constexpr Move() noexcept : data(EMPTY_MOVE) {}
    constexpr explicit Move(uint16_t d) noexcept : data(d) {}
    constexpr Move(eSquare from, eSquare to, uint8_t flags = 0) noexcept
        : data(static_cast<uint16_t>(from | (to << 6) | (flags << 12)))
    {
    }

    [[nodiscard]] constexpr eSquare from() const noexcept {
        return static_cast<eSquare>(data & 0x3F);
    }

    [[nodiscard]] constexpr eSquare to() const noexcept {
        return static_cast<eSquare>((data >> 6) & 0x3F);
    }

    [[nodiscard]] constexpr uint8_t flags() const noexcept {
        return static_cast<uint8_t>(data >> 12);
    }

    [[nodiscard]] constexpr bool is_null() const noexcept {
        return data == EMPTY_MOVE;
    }

    // Move constructor - use compiler-generated/defaulted implementation so the
    // object can be copied as a whole (avoids repeated bitfield RMW ops).
    Move(Move&& other) noexcept = default;

    // Move assignment operator - defaulted to enable efficient copy of the
    // underlying storage instead of per-field assignments.
    Move& operator=(Move&& other) noexcept = default;

    Move(eSquare from, eSquare to, MoveType type) noexcept
        : data(static_cast<uint16_t>(from | (to << 6) | static_cast<uint8_t>(type) << 12))
    {
    }

    ~Move() noexcept = default;

    // Copy assignment operator
    Move& operator= (_In_ const Move& rhs) noexcept = default;

    constexpr bool operator!() const noexcept {
        return IsEmpty();
    }

    constexpr bool IsEmpty() const noexcept {
        return data == EMPTY_MOVE;
    }

    // Sets the move data fields
    // Note: Used for performance reasons to avoid constructing new Move objects all the time
    // Remark: Type is defaulted to QUIET
    constexpr void SetMove(eSquare from, eSquare to) noexcept
    {
        SetMove(from, to, MoveType::QUIET);
    }
    constexpr void SetMove(eSquare from, eSquare to, MoveType moveType) noexcept
    {
        data = static_cast<uint16_t>(from | (to << 6) | static_cast<uint8_t>(moveType) << 12);
    }

    // Return an evaluation of the material value consequences of the Move.
    // movPiece: the effective moving piece (obtain via Board::GetEffectiveMovPiece before DoMove).
    // content:  the captured piece (obtain via Board::GetCapturedPiece before DoMove; NO_PIECE if quiet).
    // Note: Currently only used for capture move sorting.
    static int Value(_In_ const Move& move, _In_ ePiece movPiece, _In_ ePiece content) noexcept
    {
        // Algorithm:
        // ----------
        // Primarily: return material value of any captured piece minus the value of the moving piece
        // That way we can rank the interesting captures and get the pawn-takes-bishop before queen-takes-bishop
        // Note: We are dividing the move value by 16 (first binary value over Queen factor) to reflect that
        // 1) queen-takes-queen is way more interesting avenue than bishop takes pawn
        // 2) queen-takes-pawn (900 and 100) is more interesting than pawn-moves-normally (100) and
        // 3) queen-takes-queen (900-900) is more interest than pawn-takes-pawn (100-100), but also
        // 4) Pawn move (0-100/16) is more interesting than Rook move (0 - 500/16)
        // Formula: Captured piece value + (Promotion value diff) - Moving piece/16
        // Example: Queen takes Pawn: 100 - 900/16 ~= +43.75, Pawn move: 0 - 100/16 ~= -6.6
        // Best move is then pawn-takes-queen and gets promoted to a Queen: 900 + (900 - 100) - 100/16 = ~1693

        int captureScore = 0;
        const auto movingPieceScore = PieceHelper::Value(movPiece) >> 4;
        const auto type = static_cast<MoveType>(move.flags());
        switch (type)
        {
        case MoveType::QUIET:
        case MoveType::DOUBLE_PAWN_PUSH:
        case MoveType::QUEEN_CASTLE:
        case MoveType::KING_CASTLE:
			return (movingPieceScore * -1);	// TODO: Check what value this provides castling? Not used atm
        case MoveType::CAPTURE:
        case MoveType::EP_CAPTURE:
            captureScore = PieceHelper::Value(content);
            return captureScore - movingPieceScore;
        case MoveType::PROMOTION_KNIGHT:
        case MoveType::PROMOTION_BISHOP:
        case MoveType::PROMOTION_ROOK:
        case MoveType::PROMOTION_QUEEN:
        case MoveType::PROMOTION_KNIGHT_CAPTURE:
        case MoveType::PROMOTION_BISHOP_CAPTURE:
        case MoveType::PROMOTION_ROOK_CAPTURE:
		case MoveType::PROMOTION_QUEEN_CAPTURE:	// +: Queen (etc) and any Capture piece value; -: Pawn value
            captureScore = PieceHelper::Value(movPiece) - PieceHelper::Value(ePiece::WHITE_PAWN);
			if (PieceHelper::IsActual(content))
            {
                captureScore += PieceHelper::Value(content);
            }
            return captureScore - static_cast<int>(g_iPieceValues[PAWN] >> 4); // Promote is encoded differently. Subtract the known moving piece value
        }
        return 0;
    }

    bool operator==(_In_ const Move& rhs) const noexcept
    {
        return IsSameAs(rhs);
    }

    bool operator!=(_In_ const Move& rhs) const noexcept
    {
        return !IsSameAs(rhs);
    }

    bool IsSameAs(_In_ const Move& rhs) const noexcept
    {
        return ((to() == rhs.to())
            && (from() == rhs.from()));
    }

    void Clear() noexcept
    {
        data = EMPTY_MOVE;
    }

    // Coordinate-only output (no piece prefix). Suitable for PV lines and stream consumers.
    std::string Output() const;
    // Piece-prefixed output (e.g. "Pe2-e4"). Requires the moving piece explicitly.
    std::string Output(ePiece movPiece) const;

private:
    uint16_t data{ EMPTY_MOVE }; // bits 0-5: from, 6-11: to, 12-15: flags

public:
    static const Move& EmptyMove() noexcept
    {
        static const Move move;
        return move;
    }
};
// End Class Move

// Phase 4: Move is now a pure 16-bit value — no ePiece Content field.
static_assert(sizeof(Move) == 2, "Move must be exactly 2 bytes after Phase 4 refactoring");

// Move flags — mirror of MoveType enum for constexpr use in get_captured_piece and factory helpers.
namespace MoveFlags {
    constexpr uint8_t QUIET = 0;
    constexpr uint8_t DOUBLE_PAWN_PUSH = 1;
    constexpr uint8_t KING_CASTLE = 2;
    constexpr uint8_t QUEEN_CASTLE = 3;
    constexpr uint8_t CAPTURE = 4;
    constexpr uint8_t EP_CAPTURE = 5;
    constexpr uint8_t PROMOTION_KNIGHT = 8;
    constexpr uint8_t PROMOTION_BISHOP = 9;
    constexpr uint8_t PROMOTION_ROOK = 10;
    constexpr uint8_t PROMOTION_QUEEN = 11;
    constexpr uint8_t PROMOTION_KNIGHT_CAPTURE = 12;
    constexpr uint8_t PROMOTION_BISHOP_CAPTURE = 13;
    constexpr uint8_t PROMOTION_ROOK_CAPTURE = 14;
    constexpr uint8_t PROMOTION_QUEEN_CAPTURE = 15;
    // Bit masks
    constexpr uint8_t CAPTURE_BIT    = 0x4; // bit 2: move involves a capture
    constexpr uint8_t PROMOTION_BIT  = 0x8; // bit 3: move is a promotion
}

// Move list with small buffer optimization
class MoveList {
public:
    static constexpr size_t MAX_MOVES = 218; // Maximum legal moves in any position

    MoveList() noexcept : size_(0) {}

    void push(Move move) noexcept {
        if (size_ < MAX_MOVES) {
            moves_[size_++] = move;
        }
    }

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    void clear() noexcept { size_ = 0; }

    [[nodiscard]] Move* begin() noexcept { return moves_.data(); }
    [[nodiscard]] Move* end() noexcept { return moves_.data() + size_; }
    [[nodiscard]] const Move* begin() const noexcept { return moves_.data(); }
    [[nodiscard]] const Move* end() const noexcept { return moves_.data() + size_; }

    [[nodiscard]] Move& operator[](size_t idx) noexcept { return moves_[idx]; }
    [[nodiscard]] const Move& operator[](size_t idx) const noexcept { return moves_[idx]; }

    // Added for compatibility with old MoveList using vector
    const Move& at(size_t idx) const noexcept {
        return moves_[idx];
    }

    const Move& front() const noexcept {
        return moves_[0];
    }
    const Move& back() const noexcept {
        return moves_[size_ - 1];
    }

private:
    std::array<Move, MAX_MOVES> moves_;
    size_t size_;
};

// Principal variation line
// Keeps the best variant in the vector
using PVLine = std::deque<Move>;
std::ostream& operator<<(std::ostream&, _In_ const PVLine&);

