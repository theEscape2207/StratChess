#include "StdAfx.h"
#include "See.h"

#include "Board.h"
#include "Magic.h"
#include "MoveGenerator.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "SquareHelper.h"

namespace {
	// Isolates the least-significant set bit. Bits:: has clearLsb but no "keep only the lsb".
	[[nodiscard]] constexpr BITBOARD lowest_bit(BITBOARD value) noexcept { return value & (~value + 1); }

	[[nodiscard]] constexpr int value_of(ePieceType type) noexcept { return g_iPieceValues[type >> 1]; }
} // namespace

// Static exchange evaluation as a swap list on a mutated occupancy: each side recaptures on
// move.to() with its least valuable attacker until one of them declines, and `swap` carries the
// running balance so the loop can stop as soon as the threshold is decided either way.
//
// X-rays are handled. Removing an attacker from the occupancy and re-querying the sliders through
// it uncovers the piece behind — rook behind rook, queen behind bishop. Skipping that would be
// cheaper and wrong in exactly the battery positions where an exchange that looks losing wins.
//
// Pins are ignored. A pinned defender is counted as a defender even though it could not legally
// recapture, so an exchange can be reported as losing when it is not. This is the standard
// omission: modelling pins means legality checking inside the swap loop, and it rarely pays.
bool See::see_ge(const Board& board, const Move& move, int threshold) noexcept
{
	assert(MoveHelper::IsCapture(move) || MoveHelper::IsPromote(move));

	const auto boards = board.GetBitBoards();
	const BITBOARD* bb = boards.data();

	const eSquare from = move.from();
	const eSquare to = move.to();

	// The promotion is credited once, at the root of the swap list. From here on the piece
	// standing on `to` — and so the piece at risk — is the promoted one, which is exactly what
	// GetEffectiveMovPiece already returns.
	const ePiece placedPiece = board.GetEffectiveMovPiece(move);
	const ePiece captured = board.GetCapturedPiece(move);

	int gain = PieceHelper::IsActual(captured) ? PieceHelper::Value(captured) : 0;
	if (MoveHelper::IsPromote(move))
		gain += PieceHelper::Value(placedPiece) - PieceHelper::Value(ePiece::WHITE_PAWN);

	// Even unopposed, the move does not reach the threshold.
	int swap = gain - threshold;
	if (swap < 0)
		return false;

	// Even conceding the placed piece to a free recapture, the move still reaches the threshold.
	swap = PieceHelper::Value(placedPiece) - swap;
	if (swap <= 0)
		return true;

	const eColor mover = board.GetCurrentColor();

	// Occupancy after the root move. Bits are cleared rather than toggled: on an en-passant
	// capture the destination square is empty and the captured pawn stands beside it, so a toggle
	// would put a phantom piece on `to` and leave the real victim on the board.
	BITBOARD occupied = Bits::clearBits(bb[ALL_PIECES], g_bbMask[from] | g_bbMask[to]);
	if (MoveHelper::IsEnPassant(move))
		occupied = Bits::clearBits(occupied, g_bbMask[SquareHelper::PreviousRow(to, mover)]);

	BITBOARD attackers = MoveGenerator::AttackersTo(bb, to, occupied);

	// `result` is the answer as it stands: 1 while the side that started the exchange is holding
	// the threshold. Each side that finds a recapture worth making flips it.
	eColor stm = mover;
	int result = 1;

	while (true) {
		stm = static_cast<eColor>(stm ^ 1);
		attackers &= occupied;

		const BITBOARD stmAttackers = attackers & bb[ALL_FROM_COLOR + static_cast<int>(stm)];
		if (stmAttackers == 0)
			break;

		result ^= 1;

		// Least valuable attacker first, and re-query only the ray the departing piece vacated:
		// a diagonal mover can uncover a bishop or queen behind it, an orthogonal one a rook or
		// queen, and a knight can uncover nothing.
		BITBOARD next = stmAttackers & bb[PAWN + static_cast<int>(stm)];
		if (next != 0) {
			if ((swap = value_of(PAWN) - swap) < result)
				break;
			occupied = Bits::clearBits(occupied, lowest_bit(next));
			attackers |=
			    BishopAttacks(to, occupied) & (bb[WHITE_BISHOP] | bb[BLACK_BISHOP] | bb[WHITE_QUEEN] | bb[BLACK_QUEEN]);
		} else if ((next = stmAttackers & bb[KNIGHT + static_cast<int>(stm)]) != 0) {
			if ((swap = value_of(KNIGHT) - swap) < result)
				break;
			occupied = Bits::clearBits(occupied, lowest_bit(next));
		} else if ((next = stmAttackers & bb[BISHOP + static_cast<int>(stm)]) != 0) {
			if ((swap = value_of(BISHOP) - swap) < result)
				break;
			occupied = Bits::clearBits(occupied, lowest_bit(next));
			attackers |=
			    BishopAttacks(to, occupied) & (bb[WHITE_BISHOP] | bb[BLACK_BISHOP] | bb[WHITE_QUEEN] | bb[BLACK_QUEEN]);
		} else if ((next = stmAttackers & bb[ROOK + static_cast<int>(stm)]) != 0) {
			if ((swap = value_of(ROOK) - swap) < result)
				break;
			occupied = Bits::clearBits(occupied, lowest_bit(next));
			attackers |=
			    RookAttacks(to, occupied) & (bb[WHITE_ROOK] | bb[BLACK_ROOK] | bb[WHITE_QUEEN] | bb[BLACK_QUEEN]);
		} else if ((next = stmAttackers & bb[QUEEN + static_cast<int>(stm)]) != 0) {
			if ((swap = value_of(QUEEN) - swap) < result)
				break;
			occupied = Bits::clearBits(occupied, lowest_bit(next));
			attackers |=
			    (BishopAttacks(to, occupied) &
			     (bb[WHITE_BISHOP] | bb[BLACK_BISHOP] | bb[WHITE_QUEEN] | bb[BLACK_QUEEN])) |
			    (RookAttacks(to, occupied) & (bb[WHITE_ROOK] | bb[BLACK_ROOK] | bb[WHITE_QUEEN] | bb[BLACK_QUEEN]));
		} else {
			// A king attacker terminates the swap rather than entering the arithmetic, where its
			// 10000 cp notional value would dominate everything else on the list. If the other
			// side still attacks the square, this recapture is illegal and simply is not
			// available, so the exchange ends one capture earlier than the loop assumed.
			return (attackers & occupied & ~bb[ALL_FROM_COLOR + static_cast<int>(stm)]) != 0 ? (result ^ 1) != 0
			                                                                                 : result != 0;
		}
	}

	return result != 0;
}
