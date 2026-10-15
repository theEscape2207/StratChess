// KeyAfterTests.cpp — Board::KeyAfter against the key DoMove actually produces
//
// KeyAfter predicts the child's zobrist key so the search can prefetch its bucket before DoMove
// (#825). It is exact for every legal move except en-passant captures, which leave the captured pawn
// in the key by design. The walk compares the prediction with the real key at every accepted move.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "MoveGenerator.h"
#include "MoveHelper.h"
#include "defines.h"

namespace {

	struct KeyAfterCounts {
		uint64_t moves{0};
		uint64_t enPassant{0};
		uint64_t kingCastles{0};
		uint64_t queenCastles{0};
		uint64_t promotions{0};
	};

	void walk_and_compare(Board& board, int depth, KeyAfterCounts& counts)
	{
		if (depth == 0)
			return;

		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, moves);
		for (const Move& move : moves) {
			const uint64_t predicted = board.KeyAfter(move);
			if (!board.DoMove(move))
				continue;
			++counts.moves;
			if (MoveHelper::IsEnPassant(move)) {
				++counts.enPassant;
			} else {
				counts.kingCastles += (move.flags() == MoveFlags::KING_CASTLE) ? 1 : 0;
				counts.queenCastles += (move.flags() == MoveFlags::QUEEN_CASTLE) ? 1 : 0;
				counts.promotions += MoveHelper::IsPromote(move) ? 1 : 0;
				REQUIRE(predicted == board.get_zobrist_hash());
			}
			walk_and_compare(board, depth - 1, counts);
			board.UndoMove(move);
		}
	}

} // namespace

// The InCheckTests.cpp corpus plus one position:the standard perft positions (castling both sides, promotions with
// and without capture), plus en-passant captures that pin along a rank or a diagonal.
static constexpr const char* KEY_AFTER_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
    "8/8/8/4k3/8/4K3/8/8 w - - 0 1",
    "8/8/8/KPp4r/8/8/8/4k3 w - c6 0 2",
    "8/8/4k3/8/2pP4/8/B7/4K3 b - d3 0 1",
    "4k3/8/8/8/8/8/3p4/5K1R b - - 0 1",
    // Promotion-captures on both corner rooks while both castling rights are live.
    "r3k2r/1P4P1/8/8/8/8/8/4K3 w kq - 0 1",
};

TEST_CASE("KeyAfter predicts DoMove's key for every legal move except en-passant", "[board][keyafter]")
{
	KeyAfterCounts counts;
	for (const char* fen : KEY_AFTER_FENS) {
		INFO(fen);
		Board board(fen);
		walk_and_compare(board, 3, counts);
	}

	// Guard against a vacuous pass: every special-case term must have been exercised.
	CHECK(counts.moves > 100'000);
	CHECK(counts.enPassant > 0);
	CHECK(counts.kingCastles > 0);
	CHECK(counts.queenCastles > 0);
	CHECK(counts.promotions > 0);
}
