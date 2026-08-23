// BoardInstanceTests.cpp — Catch2 test suite verifying Board is a plain
// constructible, copyable value type (Phase 1 of De-Singleton Board).
//
// Deliberately does NOT use MoveGenerator: at this phase the generator still
// reads Board::Instance() internally, so a generated move would not
// necessarily belong to the local boards under test here. A hand-built quiet
// pawn push (legal from the starting position) is used instead to exercise
// DoMove/UndoMove.

#include <catch2/catch_test_macros.hpp>
#include "Board.h"
#include "MoveFactory.h"

namespace {
	constexpr const char* kStartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
	constexpr const char* kKiwipeteFEN = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	// e2-e3: plain quiet pawn push, legal from the start position, no generator needed
	Move make_e2e3() { return MoveFactory::MakeMove(e2, e3, MoveType::QUIET); }
} // namespace

static_assert(std::is_copy_constructible_v<Board>);
static_assert(std::is_copy_assignable_v<Board>);

TEST_CASE("Board - Two boards hold independent state", "[board_instance]")
{
	Board a(kStartFEN);
	Board b(kKiwipeteFEN);
	REQUIRE(a.get_zobrist_hash() != b.get_zobrist_hash());
	REQUIRE(a.ExtractFEN() != b.ExtractFEN());
}

TEST_CASE("Board - Mutating one board does not affect another", "[board_instance]")
{
	Board a(kStartFEN);
	Board b(kStartFEN);
	const auto hashA = a.get_zobrist_hash();

	REQUIRE(b.DoMove(make_e2e3()));

	REQUIRE(a.get_zobrist_hash() == hashA); // a untouched
	REQUIRE(b.get_zobrist_hash() != hashA); // b advanced
}

TEST_CASE("Board - Same FEN yields same zobrist hash across instances", "[board_instance]")
{
	Board a(kKiwipeteFEN);
	Board b(kKiwipeteFEN);
	REQUIRE(a.get_zobrist_hash() == b.get_zobrist_hash()); // global key tables shared
}

TEST_CASE("Board - Copied board equals original and diverges independently", "[board_instance]")
{
	Board original(kStartFEN);
	Board copy = original;
	REQUIRE(copy.get_zobrist_hash() == original.get_zobrist_hash());
	REQUIRE(copy.ExtractFEN() == original.ExtractFEN());

	const Move m = make_e2e3();
	REQUIRE(copy.DoMove(m));
	REQUIRE(copy.get_zobrist_hash() != original.get_zobrist_hash());

	copy.UndoMove(m);
	REQUIRE(copy.get_zobrist_hash() == original.get_zobrist_hash());
}
