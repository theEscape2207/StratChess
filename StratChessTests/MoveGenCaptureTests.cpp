// MoveGenCaptureTests.cpp — Catch2 tests for MoveGenerator::ComputeCaptures().
//
// ComputeCaptures() feeds quiescence and nothing else, and no test exercised it in
// isolation, so a one-token colour-index inversion left it unable to generate any
// capture by a knight, bishop, rook, queen or king for either side (#306). Perft
// could not see it: that drives ComputeLegalMoves(), which takes the other branch.
//
// The cases below cover one capture per piece type per colour, the promotion output
// that shares the function, and the two properties that make the result usable:
// only enemy-occupied targets, and no quiet moves.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include "MoveHelper.h"
#include "PieceHelper.h"
#include "defines.h"
#include <algorithm>
#include <string>
#include <vector>

// ── Helpers ──────────────────────────────────────────────────────────────────

static int CountPieces(const Board& board)
{
	int count = 0;
	for (int square = 0; square < NUM_SQUARES; ++square)
		if (board.GetPiece(static_cast<eSquare>(square)) != ePiece::NO_PIECE)
			++count;
	return count;
}

// Coordinate strings for every move ComputeCaptures() produces, sorted so the
// expectations below can be written without depending on generation order.
static std::vector<std::string> CaptureMoves(const char* fen)
{
	Board board(fen);
	// A rejected FEN leaves an empty board behind, and every expectation below would
	// then hold vacuously, so prove the position actually loaded. Board::setup_from_fen_impl
	// rejects a position whose side-not-to-move is in check, which is easy to author by
	// accident.
	REQUIRE(CountPieces(board) >= 2);

	MoveList moves;
	MoveGenerator::ComputeCaptures(board, moves);

	std::vector<std::string> out;
	out.reserve(moves.size());
	for (const auto& move : moves) {
		const ePiece target = board.GetPiece(move.to());
		if (PieceHelper::IsActual(target)) {
			CHECK(MoveHelper::IsCapture(move));
		} else if (MoveHelper::IsPromote(move)) {
			CHECK_FALSE(MoveHelper::IsCapture(move));
		} else {
			CHECK(MoveHelper::AsType(move) == MoveType::EP_CAPTURE);
		}
		out.emplace_back(MoveFormatter::ToUCI(move));
	}
	std::sort(out.begin(), out.end());
	return out;
}

// Asserts that every generated move lands on a square an enemy piece occupies, and that
// no move targets a king. En-passant is excluded: its target square is empty by
// definition, so it is checked separately.
//
// The non-emptiness REQUIRE is load-bearing, not decoration: a property expressed as a
// loop over the move list holds vacuously when the list is empty, which is precisely the
// state #306 left ComputeCaptures() in. Without it this helper passes on the broken code.
static void CheckEveryTargetHoldsAnEnemyPiece(const char* fen)
{
	INFO("fen = " << fen);
	Board board(fen);
	REQUIRE(CountPieces(board) >= 2);

	MoveList moves;
	MoveGenerator::ComputeCaptures(board, moves);
	REQUIRE(moves.size() > 0);

	const eColor us = board.GetCurrentColor();
	for (const auto& move : moves) {
		if (MoveHelper::IsPromote(move) && !MoveHelper::IsCapture(move))
			continue; // quiet promotions are generated on purpose
		const ePiece target = board.GetPiece(move.to());
		INFO("move = " << MoveFormatter::ToUCI(move));
		CHECK(PieceHelper::IsActual(target));
		CHECK(PieceHelper::Color(target) != us);
		// A king is never a legal capture target. DoMove() would reject the reply that
		// left it en prise, so no such position reaches the search, but the generator
		// should not be the thing relied on to discover that.
		CHECK_FALSE(PieceHelper::IsKing(target));
	}
}

// ── One capture per piece type, both colours ─────────────────────────────────
// Each position offers exactly one capture, so the expected list pins both that the
// capture is generated and that nothing spurious comes with it.

TEST_CASE("ComputeCaptures - rook capture is generated for both colours", "[movegen][capture]")
{
	// Rooks facing each other down the d-file, one square apart.
	CHECK(CaptureMoves("4k3/8/8/3r4/3R4/8/8/4K3 w - - 0 1") == std::vector<std::string>{"d4d5"});
	CHECK(CaptureMoves("4k3/8/8/3r4/3R4/8/8/4K3 b - - 0 1") == std::vector<std::string>{"d5d4"});
}

TEST_CASE("ComputeCaptures - knight capture is generated for both colours", "[movegen][capture]")
{
	CHECK(CaptureMoves("4k3/8/8/8/3n4/8/2N5/4K3 w - - 0 1") == std::vector<std::string>{"c2d4"});
	CHECK(CaptureMoves("4k3/8/8/8/3n4/8/2N5/4K3 b - - 0 1") == std::vector<std::string>{"d4c2"});
}

TEST_CASE("ComputeCaptures - bishop capture is generated for both colours", "[movegen][capture]")
{
	CHECK(CaptureMoves("4k3/8/8/8/3b4/8/1B6/4K3 w - - 0 1") == std::vector<std::string>{"b2d4"});
	CHECK(CaptureMoves("4k3/8/8/8/3b4/8/1B6/4K3 b - - 0 1") == std::vector<std::string>{"d4b2"});
}

TEST_CASE("ComputeCaptures - queen capture is generated for both colours", "[movegen][capture]")
{
	CHECK(CaptureMoves("4k3/8/8/8/3q4/8/1Q6/4K3 w - - 0 1") == std::vector<std::string>{"b2d4"});
	CHECK(CaptureMoves("4k3/8/8/8/3q4/8/1Q6/4K3 b - - 0 1") == std::vector<std::string>{"d4b2"});
}

TEST_CASE("ComputeCaptures - king capture is generated for both colours", "[movegen][capture]")
{
	// The captured piece is undefended, so the capture is legal as well as generated.
	CHECK(CaptureMoves("7k/8/8/8/8/8/3r4/3K4 w - - 0 1") == std::vector<std::string>{"d1d2"});
	CHECK(CaptureMoves("3k4/3R4/8/8/8/8/8/7K b - - 0 1") == std::vector<std::string>{"d8d7"});
}

TEST_CASE("ComputeCaptures - pawn capture is generated for both colours", "[movegen][capture]")
{
	CHECK(CaptureMoves("4k3/8/8/8/8/3p4/4P3/4K3 w - - 0 1") == std::vector<std::string>{"e2d3"});
	CHECK(CaptureMoves("4k3/8/8/8/8/3p4/4P3/4K3 b - - 0 1") == std::vector<std::string>{"d3e2"});
}

// ── Shape of the result ──────────────────────────────────────────────────────

TEST_CASE("ComputeCaptures - a blocked officer with no enemy target yields nothing", "[movegen][capture]")
{
	// The rook's only reachable squares are empty or hold its own king: no captures,
	// and in particular no quiet rook moves.
	CHECK(CaptureMoves("4k3/8/8/8/8/8/8/R3K3 w - - 0 1").empty());
}

TEST_CASE("ComputeCaptures - every generated target holds an enemy piece", "[movegen][capture]")
{
	// Crowded positions with many pieces of both colours in contact: the property must
	// hold there, not just in the one-capture cases above. Kiwipete is included because
	// it is the densest capture position in common use.
	CheckEveryTargetHoldsAnEnemyPiece("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 1");
	CheckEveryTargetHoldsAnEnemyPiece("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 0 1");
	CheckEveryTargetHoldsAnEnemyPiece("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
	CheckEveryTargetHoldsAnEnemyPiece("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R b KQkq - 0 1");
}

TEST_CASE("ComputeCaptures - en-passant is generated and stays on the pawn path", "[movegen][capture]")
{
	// The one target square that is empty rather than enemy-occupied. It is ORed into
	// the enemy mask by GeneratePawnCaptures only, so the officer path must never reach
	// it — asserting the exact list pins both halves.
	CHECK(CaptureMoves("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1") == std::vector<std::string>{"e5d6"});
	CHECK(CaptureMoves("4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1") == std::vector<std::string>{"e4d3"});
}

TEST_CASE("ComputeCaptures - captures are a subset of the legal move list", "[movegen][capture]")
{
	// ComputeCaptures must never invent a move ComputeLegalMoves does not also produce.
	constexpr const char* busy = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 1";
	Board board(busy);

	MoveList captures;
	MoveGenerator::ComputeCaptures(board, captures);
	MoveList legal;
	MoveGenerator::ComputeLegalMoves(board, legal);
	REQUIRE(captures.size() > 0);

	for (const auto& capture : captures) {
		const bool found = std::any_of(legal.begin(), legal.end(), [&capture](const Move& m) {
			// Move equality ignores flags, so compare the coordinates it does carry.
			return m.from() == capture.from() && m.to() == capture.to();
		});
		INFO("capture not present in the legal move list: " << MoveFormatter::ToUCI(capture));
		CHECK(found);
	}
}

TEST_CASE("ComputeCaptures - promotions are generated, including capture-promotions", "[movegen][capture]")
{
	// Quiet promotion: the pawn steps to an empty back-rank square.
	const auto quiet = CaptureMoves("4k3/1P6/8/8/8/8/8/4K3 w - - 0 1");
	CHECK(quiet.size() == 4);
	CHECK(std::find(quiet.begin(), quiet.end(), "b7b8q") != quiet.end());

	// Capture-promotion: a rook on a8 gives the pawn a second promotion square.
	const auto capturing = CaptureMoves("r3k3/1P6/8/8/8/8/8/4K3 w - - 0 1");
	CHECK(std::find(capturing.begin(), capturing.end(), "b7a8q") != capturing.end());
	CHECK(std::find(capturing.begin(), capturing.end(), "b7b8q") != capturing.end());
}
