// MoveFieldTests.cpp — Catch2 [moves] tests for the Move value type.
//
// Covers the packed 16-bit representation (field round-trips), the identity rule, the empty
// sentinel, and the ToCoord/ToUCI distinction.

#include <catch_amalgamated.hpp>
#include "Move.h"
#include "MoveFactory.h"
#include "MoveFormatter.h"
#include "defines.h"

// ── Field encoding ────────────────────────────────────────────────────────────
// Move packs from/to/flags into one uint16_t: bits 0-5 from, 6-11 to, 12-15 flags.

TEST_CASE("Move: from/to/flags round-trip across the full range of each field", "[moves]")
{
	SECTION("Every from-square survives the round-trip")
	{
		for (int i = 0; i < 64; ++i) {
			const auto sq = static_cast<eSquare>(i);
			const Move m(sq, e4, MoveFlags::CAPTURE);
			REQUIRE(m.from() == sq);
			REQUIRE(m.to() == e4);
			REQUIRE(m.flags() == MoveFlags::CAPTURE);
		}
	}
	SECTION("Every to-square survives the round-trip")
	{
		for (int i = 0; i < 64; ++i) {
			const auto sq = static_cast<eSquare>(i);
			const Move m(e2, sq, MoveFlags::CAPTURE);
			REQUIRE(m.from() == e2);
			REQUIRE(m.to() == sq);
			REQUIRE(m.flags() == MoveFlags::CAPTURE);
		}
	}
	SECTION("All 16 flag values survive the round-trip")
	{
		for (int f = 0; f < 16; ++f) {
			const Move m(e2, e4, static_cast<uint8_t>(f));
			REQUIRE(m.flags() == f);
			REQUIRE(m.from() == e2); // flags must not bleed into the square fields
			REQUIRE(m.to() == e4);
		}
	}
	SECTION("SetMove overwrites all three fields")
	{
		Move m(a1, h8, MoveFlags::QUIET);
		m.SetMove(e7, e8, MoveType::PROMOTION_QUEEN);
		CHECK(m.from() == e7);
		CHECK(m.to() == e8);
		CHECK(m.flags() == MoveFlags::PROMOTION_QUEEN);
	}
}

// ── Identity ──────────────────────────────────────────────────────────────────

TEST_CASE("Move: equality compares from/to only and deliberately ignores flags", "[moves]")
{
	// operator== delegates to IsSameAs, which compares to() and from() and nothing else.
	// This is a genuine trap: two moves that do completely different things to the board
	// compare equal. Pinned here so that a future change either preserves it knowingly or
	// has to delete this test on purpose.
	SECTION("Same squares, different promotion piece — equal")
	{
		const Move promoteQueen = MoveFactory::MakePromotion(e7, e8, WHITE_QUEEN, false);
		const Move promoteKnight = MoveFactory::MakePromotion(e7, e8, WHITE_KNIGHT, false);

		REQUIRE(promoteQueen.flags() != promoteKnight.flags()); // genuinely different moves
		CHECK(promoteQueen == promoteKnight);                   // ...yet compare equal
		CHECK(promoteQueen.IsSameAs(promoteKnight));
		CHECK_FALSE(promoteQueen != promoteKnight);
	}
	SECTION("Same squares, quiet vs capture — equal")
	{
		CHECK(MoveFactory::MakeQuiet(c5, d6) == MoveFactory::MakeCapture(c5, d6));
	}
	SECTION("Different squares — not equal")
	{
		CHECK(MoveFactory::MakeQuiet(e2, e4) != MoveFactory::MakeQuiet(e2, e3));
		CHECK(MoveFactory::MakeQuiet(e2, e4) != MoveFactory::MakeQuiet(d2, e4));
	}
}

// ── Empty / null sentinel ─────────────────────────────────────────────────────

TEST_CASE("Move: the empty sentinel is consistent across every route to it", "[moves]")
{
	SECTION("Default-constructed is null")
	{
		const Move def;
		CHECK(def.is_null());
	}
	SECTION("A real move is not null") { CHECK_FALSE(MoveFactory::MakeQuiet(e2, e4).is_null()); }
	SECTION("Clear() returns a real move to the sentinel")
	{
		Move m = MoveFactory::MakeQuiet(e2, e4);
		REQUIRE_FALSE(m.is_null());
		m.Clear();
		CHECK(m.is_null());
	}
	SECTION("EmptyMove() is null and compares equal to a default-constructed Move")
	{
		CHECK(Move::EmptyMove().is_null());
		CHECK(Move::EmptyMove() == Move());
	}
	SECTION("Move stays a 2-byte value")
	{
		// Also a static_assert in Move.h; restated here so the suite reports it.
		CHECK(sizeof(Move) == 2);
	}
}

// ── ToCoord vs ToUCI ──────────────────────────────────────────────────────────
// These two are similar enough to invite a "simplification" that collapses one into the
// other. They are not interchangeable. Every case below is one where they disagree.

TEST_CASE("MoveFormatter: ToCoord and ToUCI are deliberately different formats", "[moves][formatter]")
{
	SECTION("Promotion — ToCoord cannot express the promoted piece, ToUCI can")
	{
		const Move m = MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false);
		CHECK(MoveFormatter::ToCoord(m) == "b7-b8");
		CHECK(MoveFormatter::ToUCI(m) == "b7b8q");
		CHECK(MoveFormatter::ToCoord(m) != MoveFormatter::ToUCI(m));
	}
	SECTION("Underpromotions are indistinguishable in ToCoord but distinct in ToUCI")
	{
		const Move queen = MoveFactory::MakePromotion(b7, b8, WHITE_QUEEN, false);
		const Move knight = MoveFactory::MakePromotion(b7, b8, WHITE_KNIGHT, false);

		CHECK(MoveFormatter::ToCoord(queen) == MoveFormatter::ToCoord(knight));
		CHECK(MoveFormatter::ToUCI(queen) != MoveFormatter::ToUCI(knight));
	}
	SECTION("En passant — ToCoord carries an 'ep' suffix, ToUCI does not")
	{
		const Move m = MoveFactory::MakeEnPassant(f5, e6);
		CHECK(MoveFormatter::ToCoord(m) == "f5-e6ep");
		CHECK(MoveFormatter::ToUCI(m) == "f5e6");
	}
	SECTION("Castling — ToCoord loses the squares entirely, ToUCI keeps them")
	{
		const Move m = MoveFactory::MakeMove(e1, g1, MoveType::KING_CASTLE);
		CHECK(MoveFormatter::ToCoord(m) == "0-0");
		CHECK(MoveFormatter::ToUCI(m) == "e1g1");
	}
}

TEST_CASE("MoveFormatter::ToCoord: defined for all 14 real MoveTypes, empty for flags 6 and 7", "[moves][formatter]")
{
	// ToCoord switches on MoveHelper::AsType(move), which is static_cast<MoveType>(flags()).
	// Flag values 6 and 7 have no enumerator, so they fall through to `return {}`. Freezing
	// that mapping here means a future change to the flag layout or to AsType cannot quietly
	// alter which values are renderable.
	for (int f = 0; f < 16; ++f) {
		const Move m(e2, e4, static_cast<uint8_t>(f));
		const std::string rendered = MoveFormatter::ToCoord(m);

		INFO("flag value = " << f);
		if (f == 6 || f == 7)
			CHECK(rendered.empty());
		else
			CHECK_FALSE(rendered.empty());
	}
}
