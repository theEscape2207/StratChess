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

	// Which rays a departing attacker can uncover behind itself.
	constexpr unsigned UNCOVERS_NONE = 0;
	constexpr unsigned UNCOVERS_DIAGONAL = 1;
	constexpr unsigned UNCOVERS_ORTHOGONAL = 2;

	struct LeastValuableAttacker {
		ePieceType type;
		unsigned uncovers;
	};

	// Least valuable attacker first. The king is deliberately absent — it terminates the swap
	// instead of entering it, which is why the loop below treats "no entry matched" as its own case.
	constexpr std::array<LeastValuableAttacker, 5> LVA_ORDER = {{
	    {PAWN, UNCOVERS_DIAGONAL},
	    {KNIGHT, UNCOVERS_NONE},
	    {BISHOP, UNCOVERS_DIAGONAL},
	    {ROOK, UNCOVERS_ORTHOGONAL},
	    {QUEEN, UNCOVERS_DIAGONAL | UNCOVERS_ORTHOGONAL},
	}};
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

		// Least valuable attacker first.
		const LeastValuableAttacker* lva = nullptr;
		BITBOARD next = 0;
		for (const auto& candidate : LVA_ORDER) {
			next = stmAttackers & bb[candidate.type + static_cast<int>(stm)];
			if (next != 0) {
				lva = &candidate;
				break;
			}
		}

		if (lva == nullptr) {
			// Only the king is left. It terminates the swap rather than entering the arithmetic,
			// where its 10000 cp notional value would dominate everything else on the list. If the
			// other side still attacks the square, this recapture is illegal and simply is not
			// available, so the exchange ends one capture earlier than the loop assumed.
			return (attackers & occupied & ~bb[ALL_FROM_COLOR + static_cast<int>(stm)]) != 0 ? (result ^ 1) != 0
			                                                                                 : result != 0;
		}

		swap = value_of(lva->type) - swap;
		if (swap < result)
			break;

		// Re-query only the ray the departing piece vacated, so the piece behind it joins the swap.
		occupied = Bits::clearBits(occupied, lowest_bit(next));
		if ((lva->uncovers & UNCOVERS_DIAGONAL) != 0)
			attackers |=
			    BishopAttacks(to, occupied) & (bb[WHITE_BISHOP] | bb[BLACK_BISHOP] | bb[WHITE_QUEEN] | bb[BLACK_QUEEN]);
		if ((lva->uncovers & UNCOVERS_ORTHOGONAL) != 0)
			attackers |=
			    RookAttacks(to, occupied) & (bb[WHITE_ROOK] | bb[BLACK_ROOK] | bb[WHITE_QUEEN] | bb[BLACK_QUEEN]);
	}

	return result != 0;
}
