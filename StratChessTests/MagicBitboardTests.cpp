// MagicBitboardTests.cpp — Catch2 unit tests for PEXT-based sliding-piece attack generation
//
// Hand-verified rook/bishop attack bitboards for a handful of squares/occupancies (open cross,
// corner + blockers, single-diagonal corner, fully-blocked-adjacent). Independent of perft —
// perft only proves attack generation is *consistent* with legal move counts, not that any
// individual attack bitboard is correct in isolation. See Docs/TestDesign.md §Phase 1 [magic].

#include <catch_amalgamated.hpp>
#include "Magic.h"
#include "defines.h"

namespace {
	constexpr BITBOARD squares() { return 0; }

	template <typename... Squares> constexpr BITBOARD squares(eSquare sq, Squares... rest)
	{
		return (UNIT << sq) | squares(rest...);
	}
} // namespace

TEST_CASE("RookAttacks - open cross from d4 on empty board", "[magic]")
{
	const BITBOARD occupied = squares(d4);
	const BITBOARD expected = squares(d8, d7, d6, d5, d3, d2, d1, a4, b4, c4, e4, f4, g4, h4);

	REQUIRE(RookAttacks(d4, occupied) == expected);
}

TEST_CASE("RookAttacks - corner a1 stops at first blocker each direction", "[magic]")
{
	const BITBOARD occupied = squares(a1, a4, d1);
	const BITBOARD expected = squares(a2, a3, a4, b1, c1, d1);

	REQUIRE(RookAttacks(a1, occupied) == expected);
}

TEST_CASE("RookAttacks - fully blocked on all four adjacent squares", "[magic]")
{
	const BITBOARD occupied = squares(d4, d5, d3, c4, e4);
	const BITBOARD expected = squares(d5, d3, c4, e4);

	REQUIRE(RookAttacks(d4, occupied) == expected);
}

TEST_CASE("BishopAttacks - open diagonals from e4 on empty board", "[magic]")
{
	const BITBOARD occupied = squares(e4);
	const BITBOARD expected = squares(a8, b7, c6, d5, // NW
	                                  f5, g6, h7,     // NE
	                                  d3, c2, b1,     // SW
	                                  f3, g2, h1);    // SE

	REQUIRE(BishopAttacks(e4, occupied) == expected);
}

TEST_CASE("BishopAttacks - corner h1 has only one diagonal, unblocked", "[magic]")
{
	const BITBOARD occupied = squares(h1);
	const BITBOARD expected = squares(g2, f3, e4, d5, c6, b7, a8);

	REQUIRE(BishopAttacks(h1, occupied) == expected);
}

TEST_CASE("BishopAttacks - corner h1 diagonal stops at blocker", "[magic]")
{
	const BITBOARD occupied = squares(h1, e4);
	const BITBOARD expected = squares(g2, f3, e4);

	REQUIRE(BishopAttacks(h1, occupied) == expected);
}

TEST_CASE("BishopAttacks - fully blocked on all four adjacent diagonal squares", "[magic]")
{
	const BITBOARD occupied = squares(e4, d5, f5, d3, f3);
	const BITBOARD expected = squares(d5, f5, d3, f3);

	REQUIRE(BishopAttacks(e4, occupied) == expected);
}

TEST_CASE("RookAttacks - fully saturated mask exercises largest table index", "[magic]")
{
	// a1's rook mask is the maximum (12 relevant bits: a2-a7, b1-g1). Setting every mask
	// bit occupied drives _pext_u64 to its maximum index (2^12-1 = 4095) for this square —
	// the corner/edge of g_bbRookAttacks[a1]. The rook is blocked immediately in both of
	// its two available directions (no south/west from a corner).
	const BITBOARD occupied = magic::g_bbRookMask[a1];
	const BITBOARD expected = squares(a2, b1);

	REQUIRE(RookAttacks(a1, occupied) == expected);
}

TEST_CASE("BishopAttacks - fully saturated mask exercises largest table index", "[magic]")
{
	// d4's bishop mask is occupied at every relevant square, driving _pext_u64 to its
	// maximum index for this square. The bishop is blocked immediately on all four
	// diagonals at its nearest neighbor (c5 NW, e5 NE, c3 SW, e3 SE).
	const BITBOARD occupied = magic::g_bbBishopMask[d4];
	const BITBOARD expected = squares(c5, e5, c3, e3);

	REQUIRE(BishopAttacks(d4, occupied) == expected);
}
