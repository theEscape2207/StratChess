#pragma once

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

inline void run_move_field_tests()
{
    test_is_check_equality();
    test_is_check_output_post();
    // Phase 2 tests will be appended below after Phase 1 is committed.
}
