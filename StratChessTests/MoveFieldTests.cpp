// MoveFieldTests.cpp — Catch2 test suite for Move class field refactoring
//
// Migrated from StratEngine/Tests/MoveFieldTests.h.
// Verifies post-refactor invariants for Phase 1 (IsCheck removal)
// and Phase 2 (unified EMPTY_MOVE sentinel).

#include <catch_amalgamated.hpp>
#include "Move.h"
#include "MoveHelper.h"
#include "defines.h"

TEST_CASE("Phase 1 - Move equality is based on from/to squares only", "[moves]")
{
    Move m(e2, e4, MoveType::DOUBLE_PAWN_PUSH, WHITE_PAWN, NO_PIECE);
    Move m2(e2, e4, MoveType::DOUBLE_PAWN_PUSH, WHITE_PAWN, NO_PIECE);

    REQUIRE(m == m2);
    REQUIRE(m.IsSameAs(m2));
}

TEST_CASE("Phase 1 - Output() must not contain '+' (check annotation moved to caller)", "[moves]")
{
    Move m(e2, e4, MoveType::DOUBLE_PAWN_PUSH, WHITE_PAWN, NO_PIECE);
    REQUIRE(m.Output().find('+') == std::string::npos);
}

TEST_CASE("Phase 2 - Default Move is empty and null; Clear() restores empty state", "[moves]")
{
    // Default-constructed Move must be empty and null.
    Move def;
    REQUIRE(def.IsEmpty());
    REQUIRE(def.is_null());
    REQUIRE(!def);

    // A real Move must be neither empty nor null.
    Move real(e2, e4);
    REQUIRE_FALSE(real.IsEmpty());
    REQUIRE_FALSE(real.is_null());

    // Clear() must make the Move empty again.
    real.Clear();
    REQUIRE(real.IsEmpty());

    // EmptyMove() factory must produce an empty Move.
    REQUIRE(Move::EmptyMove().IsEmpty());

    // MoveHelper::IsEmpty must agree with Move::IsEmpty.
    REQUIRE(MoveHelper::IsEmpty(def));
    REQUIRE_FALSE(MoveHelper::IsEmpty(Move(e2, e4)));
}
