// PieceHelperTests.cpp — Catch2 [piecehelper] tests for the piece value table and type conversion.
//
// Covers the bounds of g_iPieceValues against every ePiece that can index it, the piece values
// themselves, and the AsPieceType/AsPiece round-trip.

#include <catch2/catch_test_macros.hpp>
#include "PieceHelper.h"
#include "defines.h"

#include <iterator>

// g_iPieceValues is indexed by (piece >> 1). The aggregate entries and NO_PIECE shift down into
// the same table, so it has to be wide enough for them: 12 >> 1 == 13 >> 1 == 6 and 15 >> 1 == 7.
// A table sized to ALL_PIECETYPES alone stops at index 5 and leaves those reads out of bounds.
TEST_CASE("g_iPieceValues covers every ePiece that can index it", "[piecehelper]")
{
	STATIC_REQUIRE(std::size(g_iPieceValues) == (ePiece::NO_PIECE >> 1) + 1);

	SECTION("every ePiece value indexes inside the table")
	{
		for (int p = ePiece::WHITE_PAWN; p <= ePiece::NO_PIECE; ++p) {
			INFO("ePiece = " << p);
			CHECK(static_cast<size_t>(p >> 1) < std::size(g_iPieceValues));
		}
	}

	SECTION("the non-piece tail scores nothing")
	{
		CHECK(g_iPieceValues[ePiece::ALL_WHITE_PIECES >> 1] == 0u);
		CHECK(g_iPieceValues[ePiece::ALL_BLACK_PIECES >> 1] == 0u);
		CHECK(g_iPieceValues[ePiece::NO_PIECE >> 1] == 0u);
	}
}

TEST_CASE("PieceHelper::Value is colour-independent and matches the table", "[piecehelper]")
{
	CHECK(PieceHelper::Value(ePiece::WHITE_PAWN) == PieceHelper::Value(ePiece::BLACK_PAWN));
	CHECK(PieceHelper::Value(ePiece::WHITE_KNIGHT) == PieceHelper::Value(ePiece::BLACK_KNIGHT));
	CHECK(PieceHelper::Value(ePiece::WHITE_BISHOP) == PieceHelper::Value(ePiece::BLACK_BISHOP));
	CHECK(PieceHelper::Value(ePiece::WHITE_ROOK) == PieceHelper::Value(ePiece::BLACK_ROOK));
	CHECK(PieceHelper::Value(ePiece::WHITE_QUEEN) == PieceHelper::Value(ePiece::BLACK_QUEEN));
	CHECK(PieceHelper::Value(ePiece::WHITE_KING) == PieceHelper::Value(ePiece::BLACK_KING));

	CHECK(PieceHelper::Value(ePiece::WHITE_PAWN) == 100u);
	CHECK(PieceHelper::Value(ePiece::WHITE_KNIGHT) == 300u);
	CHECK(PieceHelper::Value(ePiece::WHITE_BISHOP) == 300u);
	CHECK(PieceHelper::Value(ePiece::WHITE_ROOK) == 500u);
	CHECK(PieceHelper::Value(ePiece::WHITE_QUEEN) == 900u);
	CHECK(PieceHelper::Value(ePiece::WHITE_KING) == 10000u);

	// Ordering is what move ordering actually depends on.
	CHECK(PieceHelper::Value(ePiece::WHITE_PAWN) < PieceHelper::Value(ePiece::WHITE_KNIGHT));
	CHECK(PieceHelper::Value(ePiece::WHITE_BISHOP) < PieceHelper::Value(ePiece::WHITE_ROOK));
	CHECK(PieceHelper::Value(ePiece::WHITE_ROOK) < PieceHelper::Value(ePiece::WHITE_QUEEN));
	CHECK(PieceHelper::Value(ePiece::WHITE_QUEEN) < PieceHelper::Value(ePiece::WHITE_KING));
}

// ePieceType steps in twos, so the type is recovered by clearing the colour bit. Shifting instead
// yields a compact 0-5 index that is not an ePieceType value at all, and breaks this round-trip
// for every piece above pawn.
TEST_CASE("PieceHelper::AsPieceType round-trips through AsPiece", "[piecehelper]")
{
	for (int p = ePiece::WHITE_PAWN; p <= ePiece::BLACK_KING; ++p) {
		const auto piece = static_cast<ePiece>(p);
		INFO("ePiece = " << p);
		CHECK(PieceHelper::AsPiece(PieceHelper::AsPieceType(piece), PieceHelper::Color(piece)) == piece);
	}
}

TEST_CASE("PieceHelper::AsPieceType returns named ePieceType values", "[piecehelper]")
{
	CHECK(PieceHelper::AsPieceType(ePiece::WHITE_PAWN) == PAWN);
	CHECK(PieceHelper::AsPieceType(ePiece::BLACK_PAWN) == PAWN);
	CHECK(PieceHelper::AsPieceType(ePiece::WHITE_KNIGHT) == KNIGHT);
	CHECK(PieceHelper::AsPieceType(ePiece::BLACK_KNIGHT) == KNIGHT);
	CHECK(PieceHelper::AsPieceType(ePiece::WHITE_BISHOP) == BISHOP);
	CHECK(PieceHelper::AsPieceType(ePiece::BLACK_BISHOP) == BISHOP);
	CHECK(PieceHelper::AsPieceType(ePiece::WHITE_ROOK) == ROOK);
	CHECK(PieceHelper::AsPieceType(ePiece::BLACK_ROOK) == ROOK);
	CHECK(PieceHelper::AsPieceType(ePiece::WHITE_QUEEN) == QUEEN);
	CHECK(PieceHelper::AsPieceType(ePiece::BLACK_QUEEN) == QUEEN);
	CHECK(PieceHelper::AsPieceType(ePiece::WHITE_KING) == KING);
	CHECK(PieceHelper::AsPieceType(ePiece::BLACK_KING) == KING);

	// AsPieceType agrees with the compact-index comparison IsOfType uses.
	for (int p = ePiece::WHITE_PAWN; p <= ePiece::BLACK_KING; ++p) {
		const auto piece = static_cast<ePiece>(p);
		INFO("ePiece = " << p);
		CHECK(PieceHelper::IsOfType(piece, PieceHelper::AsPieceType(piece)));
	}
}
