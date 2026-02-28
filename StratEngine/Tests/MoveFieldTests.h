#pragma once

// MIGRATED — Safe to delete after review.
// Tests live in StratChessTests/MoveFieldTests.cpp (Catch2 v3).
// No longer compiled by any project.
//
// MoveFieldTests.h — Unit tests for Move class field retirement
//
// Structure: one suite per removed field, run in sequence.
// Each suite verifies post-refactor invariants.
//
// See Docs/Roadmap.md and .claude/plans/prancy-prancing-trinket.md for context.

#include "TestFramework.h"
#include "../Move.h"
#include "../MoveHelper.h"
#include "../defines.h"

// ---------------------------------------------------------------------------
// Phase 1: IsCheck (removed — check annotation moved to Game.cpp call site)
// ---------------------------------------------------------------------------

inline void test_is_check_equality()
{
    // Move identity is based on from/to squares only — never on check state.
    Move m(e2, e4, MoveType::DOUBLE_PAWN_PUSH, WHITE_PAWN, NO_PIECE);
    Move m2(e2, e4, MoveType::DOUBLE_PAWN_PUSH, WHITE_PAWN, NO_PIECE);
    TEST_ASSERT(m == m2,        "Moves with same from/to must be equal");
    TEST_ASSERT(m.IsSameAs(m2), "IsSameAs must hold for same from/to");
}

inline void test_is_check_output_post()
{
    // POST Phase 1: Output() must never produce '+' — check is annotated by the caller.
    Move m(e2, e4, MoveType::DOUBLE_PAWN_PUSH, WHITE_PAWN, NO_PIECE);
    TEST_ASSERT(m.Output().find('+') == std::string::npos,
        "POST Phase1: Output() must NOT contain '+'");
}

// ---------------------------------------------------------------------------
// Phase 2: fromIsNoSquare / toIsNoSquare
// (replaced by unified EMPTY_MOVE = 0xFFFF sentinel in Move::data)
// ---------------------------------------------------------------------------

inline void test_is_empty_and_null_behavior()
{
    // Default-constructed Move must be empty and null.
    Move def;
    TEST_ASSERT(def.IsEmpty(),  "Default Move must be empty");
    TEST_ASSERT(def.is_null(),  "Default Move must be null");
    TEST_ASSERT(!def,           "operator! must be true for empty Move");

    // A real Move must be neither empty nor null.
    Move real(e2, e4);
    TEST_ASSERT(!real.IsEmpty(), "Real move must not be empty");
    TEST_ASSERT(!real.is_null(), "Real move must not be null");

    // Clear() must make the Move empty again.
    real.Clear();
    TEST_ASSERT(real.IsEmpty(), "Cleared Move must be empty");

    // EmptyMove() must be empty.
    TEST_ASSERT(Move::EmptyMove().IsEmpty(), "EmptyMove() must be empty");

    // MoveHelper::IsEmpty must agree with Move::IsEmpty.
    TEST_ASSERT(MoveHelper::IsEmpty(def),           "MoveHelper::IsEmpty on default Move");
    TEST_ASSERT(!MoveHelper::IsEmpty(Move(e2, e4)), "MoveHelper::IsEmpty must be false for real move");
}

inline void run_move_field_tests()
{
    test_is_check_equality();
    test_is_check_output_post();
    test_is_empty_and_null_behavior();
}
