// TacticalFullTests.cpp — extended tactical suite [tactical_full][slow]
//
// Runs ~25 positions at depth 6 (< 60 s total in Release).
// Each position has a single forced best move verified against the engine.
//
// Selection invariant: every position in kSlowCases must have a unique best move
// at depth 6. Verify with: go depth 6 on each FEN before committing new positions.
//
// See Docs/TestDesign.md §Phase 0 for rationale.

#include <catch_amalgamated.hpp>
#include "TacticalTestHelpers.h"
#include "Board.h"

// ---------------------------------------------------------------------------
// Position table — extended tier (depth 6, ~25 positions, [slow])
// ---------------------------------------------------------------------------

static constexpr TacticalCase kSlowCases[] = {
    // — Mate-in-1 (back rank) ————————————————————————————————————————————————
    { "M1: rook e3 back rank (Re8#)",
      "6k1/5ppp/8/8/8/4R3/5PPP/6K1 w - - 0 1",      e3, e8, 6 },
    { "M1: rook b1 back rank (Rb8#)",
      "6k1/5ppp/8/8/8/8/5PPP/1R4K1 w - - 0 1",       b1, b8, 6 },

    // — Winning captures (piece gives cover) ——————————————————————————————————
    { "capture: Qxa8 (rook+queen battery)",
      "r5k1/5ppp/8/3Q4/8/8/5PPP/4R1K1 w - - 0 1",    d5, a8, 6 },
    { "capture: Qxa8 (queen + pawns)",
      "r3k3/5ppp/8/3Q4/8/8/5PPP/6K1 w - - 0 1",      d5, a8, 6 },
    { "capture: Qxb8 (pawn cover)",
      "1r4k1/5ppp/8/8/8/1Q6/5PPP/6K1 w - - 0 1",      b3, b8, 6 },

    // — Winning captures (hanging pieces) ————————————————————————————————————
    { "capture: Bxa8 (diagonal)",
      "r3k3/8/8/3B4/8/8/8/4K3 w - - 0 1",             d5, a8, 6 },
    { "capture: Nxa8",
      "r3k3/8/1N6/8/8/8/8/4K3 w - - 0 1",             b6, a8, 6 },
    { "capture: Rxb8",
      "1r2k3/8/8/8/8/8/8/1R2K3 w - - 0 1",            b1, b8, 6 },
    { "capture: Qxd8 (wins queen)",
      "3q1k2/8/8/8/8/8/8/3QK3 w - - 0 1",             d1, d8, 6 },
    { "capture: Rxd4",
      "4k3/8/8/8/3r4/8/3R4/4K3 w - - 0 1",            d2, d4, 6 },
    { "capture: Rxd8",
      "3r2k1/8/8/8/8/8/8/3RK3 w - - 0 1",             d1, d8, 6 },
    { "capture: Qxa8 (queen + 2 pawns)",
      "r5k1/6pp/8/8/8/8/6PP/Q5K1 w - - 0 1",          a1, a8, 6 },
    { "capture: Qxb8 (corner rook)",
      "1r4k1/8/8/8/8/8/8/1Q4K1 w - - 0 1",            b1, b8, 6 },
    { "capture: Rxc8",
      "2r3k1/8/8/8/8/8/8/2R3K1 w - - 0 1",            c1, c8, 6 },
    { "capture: Qxa7 (rank 7 rook)",
      "4k3/r7/8/8/8/8/8/Q3K3 w - - 0 1",              a1, a7, 6 },
    { "capture: Bxa8 (long diagonal)",
      "r3k3/8/8/8/8/5B2/8/4K3 w - - 0 1",             f3, a8, 6 },
    { "capture: Rxa8",
      "r3k3/8/8/8/8/8/8/R3K3 w - - 0 1",              a1, a8, 6 },
    { "capture: Qxd4 (bishop)",
      "4k3/8/8/8/3b4/8/8/3QK3 w - - 0 1",             d1, d4, 6 },
    { "capture: Qxc2 (bishop)",
      "4k3/8/8/8/8/8/2b5/3QK3 w - - 0 1",             d1, c2, 6 },
    { "capture: Qxc5 (knight)",
      "4k3/8/8/2n5/8/8/8/2QK4 w - - 0 1",             c1, c5, 6 },
    { "capture: Qxd3 (knight)",
      "4k3/8/8/8/8/3n4/8/3QK3 w - - 0 1",             d1, d3, 6 },
    { "capture: Rxb5 (bishop)",
      "4k3/8/8/1b6/8/8/8/1R2K3 w - - 0 1",            b1, b5, 6 },
    { "capture: Qxe5 (rook)",
      "4k3/8/8/4r3/4Q3/8/8/4K3 w - - 0 1",            e4, e5, 6 },
    { "capture: Qxa8 (direct)",
      "r3k3/8/8/8/8/8/8/Q3K3 w - - 0 1",              a1, a8, 6 },
    { "capture: Rxa3",
      "4k3/8/8/8/8/r7/8/R3K3 w - - 0 1",              a1, a3, 6 },
};

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

TEST_CASE("Tactical - slow suite", "[tactical_full][slow]")
{
    auto tc = GENERATE(from_range(kSlowCases));

    INFO(tc.label);
    Board board(tc.fen);
    auto ai = make_tactical_engine(board, tc.depth);
    GameInfo info = board.GetGameInfo();
    Move m = ai->GetMove(info);

    REQUIRE(m.from() == tc.expected_from);
    REQUIRE(m.to()   == tc.expected_to);
}

// ---------------------------------------------------------------------------
// Null-move pruning: guard must not alter search results in a zugzwang
// position. White has king+pawn only, Black has king only — should_try_null_move's
// zugzwang guard must refuse NMP here, so enabling/disabling NMP must produce
// byte-identical search results (same nodes, move, and score).
// ---------------------------------------------------------------------------
TEST_CASE("Tactical (full) - null-move pruning guard is a no-op in K+P endgame", "[tactical_full][slow]")
{
    const char* fen = "8/8/8/3k4/8/3K4/3P4/8 w - - 0 1";
    constexpr unsigned depth = 5;

    Board board_disabled(fen);
    auto ai_disabled = make_tactical_engine(board_disabled, depth);
    as_perplex(ai_disabled).tuning().null_move_enabled = false;
    GameInfo info_disabled = board_disabled.GetGameInfo();
    Move move_disabled = ai_disabled->GetMove(info_disabled);
    SearchResult result_disabled = as_perplex(ai_disabled).GetLastResult();

    Board board_enabled(fen);
    auto ai_enabled = make_tactical_engine(board_enabled, depth);
    as_perplex(ai_enabled).tuning().null_move_enabled = true;
    GameInfo info_enabled = board_enabled.GetGameInfo();
    Move move_enabled = ai_enabled->GetMove(info_enabled);
    SearchResult result_enabled = as_perplex(ai_enabled).GetLastResult();

    REQUIRE(move_disabled == move_enabled);
    REQUIRE(result_disabled.best_score == result_enabled.best_score);
    REQUIRE(result_disabled.nodes_searched == result_enabled.nodes_searched);
}

// ---------------------------------------------------------------------------
// Null-move pruning: regression test for the m_infoSeq desync bug (Task 4a).
// A material-rich position at sufficient depth guarantees some non-PV node
// satisfies every should_try_null_move() guard and actually calls
// DoNullMove()/recurses — exercising the path Task 1/3's tests never did.
// Before the Task 4a fix, this crashed with an out-of-range m_infoSeq access.
// ---------------------------------------------------------------------------
TEST_CASE("Tactical (full) - null-move pruning does not crash on a real recursion", "[tactical_full][slow]")
{
    Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    constexpr unsigned depth = 7;

    auto ai = make_tactical_engine(board, depth);
    as_perplex(ai).tuning().null_move_enabled = true;
    GameInfo info = board.GetGameInfo();

    Move move = ai->GetMove(info);

    REQUIRE(!move.is_null());
}
