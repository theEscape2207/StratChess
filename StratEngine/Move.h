#pragma once

#include <cstdint>
#include <deque> // For PVLine
#include "PieceHelper.h"

// Move representation (16-bit encoding)
// Encoding:
// Bits  0- 5: From square (0-63)
// Bits  6-11: To square (0-63)
// Bits 12-15: Move flags (MoveType, 0-15)
//   Flag bit layout: bit3 = promotion, bit2 = capture
//
// Neither the moving piece nor the captured piece is stored in the move:
//   Moving piece  — obtain via Board::GetEffectiveMovPiece() before DoMove.
//   Captured piece — Board::capturedHistory_[] stores it during DoMove for undo support;
//                    obtain via Board::GetCapturedPiece(move) for MVV-LVA scoring.
class Move final {
	static constexpr uint16_t EMPTY_MOVE = 0xFFFF;

	// Prints content to stream - Implemented in .cpp
	friend std::ostream& operator<<(std::ostream&, const Move& move);

  public:
	// Copy constructor
	constexpr Move(const Move& rhs) noexcept = default;

	constexpr Move() noexcept : data(EMPTY_MOVE) {}
	constexpr explicit Move(uint16_t d) noexcept : data(d) {}
	constexpr Move(eSquare from, eSquare to, uint8_t flags = 0) noexcept
	    : data(static_cast<uint16_t>(from | (to << 6) | (flags << 12)))
	{}

	[[nodiscard]] constexpr eSquare from() const noexcept { return static_cast<eSquare>(data & 0x3F); }

	[[nodiscard]] constexpr eSquare to() const noexcept { return static_cast<eSquare>((data >> 6) & 0x3F); }

	[[nodiscard]] constexpr uint8_t flags() const noexcept { return static_cast<uint8_t>(data >> 12); }

	[[nodiscard]] constexpr bool is_null() const noexcept { return data == EMPTY_MOVE; }

	// Move constructor - use compiler-generated/defaulted implementation so the
	// object can be copied as a whole (avoids repeated bitfield RMW ops).
	Move(Move&& other) noexcept = default;

	// Move assignment operator - defaulted to enable efficient copy of the
	// underlying storage instead of per-field assignments.
	Move& operator=(Move&& other) noexcept = default;

	Move(eSquare from, eSquare to, MoveType type) noexcept
	    : data(static_cast<uint16_t>(from | (to << 6) | static_cast<uint8_t>(type) << 12))
	{}

	~Move() noexcept = default;

	// Copy assignment operator
	Move& operator=(const Move& rhs) noexcept = default;

	constexpr void SetMove(eSquare from, eSquare to, MoveType moveType) noexcept
	{
		data = static_cast<uint16_t>(from | (to << 6) | static_cast<uint8_t>(moveType) << 12);
	}

	bool operator==(const Move& rhs) const noexcept { return IsSameAs(rhs); }

	bool operator!=(const Move& rhs) const noexcept { return !IsSameAs(rhs); }

	bool IsSameAs(const Move& rhs) const noexcept { return ((to() == rhs.to()) && (from() == rhs.from())); }

	void Clear() noexcept { data = EMPTY_MOVE; }

	// Move presentation lives in MoveFormatter (ToCoord / ToShort / ToUCI / ToVerbose).
	// Move is a pure 2-byte value; it deliberately owns no formatting.

  private:
	uint16_t data{EMPTY_MOVE}; // bits 0-5: from, 6-11: to, 12-15: flags

  public:
	static const Move& EmptyMove() noexcept
	{
		static const Move move;
		return move;
	}
};
// End Class Move

// Move is a pure 16-bit value: bits 0-5 = from, 6-11 = to, 12-15 = flags.
static_assert(sizeof(Move) == 2, "Move must be exactly 2 bytes");

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
constexpr uint8_t CAPTURE_BIT = 0x4;   // bit 2: move involves a capture
constexpr uint8_t PROMOTION_BIT = 0x8; // bit 3: move is a promotion
} // namespace MoveFlags

// Move list with small buffer optimization
class MoveList {
  public:
	static constexpr size_t MAX_MOVES = 218; // Maximum legal moves in any position

	MoveList() noexcept : size_(0) {}

	void push(Move move) noexcept
	{
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

	const Move& front() const noexcept { return moves_[0]; }
	const Move& back() const noexcept { return moves_[size_ - 1]; }

  private:
	std::array<Move, MAX_MOVES> moves_;
	size_t size_;
};

// Principal variation line
// Keeps the best variant in the vector
using PVLine = std::deque<Move>;
std::ostream& operator<<(std::ostream&, const PVLine&);
