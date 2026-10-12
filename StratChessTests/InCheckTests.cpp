// InCheckTests.cpp — Board::InCheck / WaitingSideInCheck against the whole-side attack map
//
// The check tests answer "is this king attacked" with a single-square attacker query. The oracle is
// the whole-side attack map: build the attacker's full attack board and test the king's bit. Every
// node of a shallow walk from positions that stress the edge cases must agree. The walk only sees
// legal positions, so a phantom check inside DoMove would drop moves unseen; each walk's node count,
// taken from python-chess, catches that.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "MoveGenerator.h"
#include "defines.h"

namespace {

	bool attack_map_says_attacked(const Board& board, eColor kingColor)
	{
		const eColor attacker = (kingColor == WHITE ? BLACK : WHITE);
		const BITBOARD attacks = MoveGenerator::GetAttackBoard(board, attacker);
		return Bits::isAnyBitSet(attacks, board.GetBitBoards()[KING + static_cast<int>(kingColor)]);
	}

	struct WalkCounts {
		uint64_t nodes{0};
		uint64_t inCheck{0};
	};

	void walk_and_compare(Board& board, int depth, WalkCounts& counts)
	{
		const eColor toMove = board.GetCurrentColor();
		const eColor waiting = (toMove == WHITE ? BLACK : WHITE);
		const bool expected = attack_map_says_attacked(board, toMove);

		++counts.nodes;
		counts.inCheck += expected ? 1 : 0;
		REQUIRE(board.InCheck() == expected);
		REQUIRE(board.WaitingSideInCheck() == attack_map_says_attacked(board, waiting));

		if (depth == 0)
			return;

		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, moves);
		for (const Move& move : moves) {
			if (!board.DoMove(move))
				continue;
			walk_and_compare(board, depth - 1, counts);
			board.UndoMove(move);
		}
	}

} // namespace

struct WalkCase {
	const char* fen;
	uint64_t nodes; // every node to depth 3, root included, counted by python-chess
};

// clang-format off
// Standard perft positions plus hand-picked ones: kings in opposition (every king move into contact
// must be rejected), an en-passant capture that would expose its own king along a rank or a
// diagonal, and promotions that give check.
static constexpr WalkCase WALK_CASES[] = {
	{"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 9323},
	{"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 99950},
	{"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 3018},
	{"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 9738},
	{"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 63910},
	{"n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1", 10004},
	{"8/8/8/4k3/8/4K3/8/8 w - - 0 1", 299},
	{"8/8/8/KPp4r/8/8/8/4k3 w - c6 0 2", 390},
	{"8/8/4k3/8/2pP4/8/B7/4K3 b - d3 0 1", 535},
	{"4k3/8/8/8/8/8/3p4/5K1R b - - 0 1", 1028},
};
// clang-format on

TEST_CASE("InCheck and WaitingSideInCheck agree with the whole-side attack map", "[board][incheck]")
{
	uint64_t inCheck = 0;
	for (const auto& [fen, nodes] : WALK_CASES) {
		INFO(fen);
		WalkCounts counts;
		Board board(fen);
		walk_and_compare(board, 3, counts);
		CHECK(counts.nodes == nodes);
		inCheck += counts.inCheck;
	}

	// Guard against a vacuous pass: the walk has to reach check positions to test anything.
	CHECK(inCheck > 1'000);
}
