// TacticalTests.cpp — fast tactical suite [tactical]
//
// Runs ~10 positions at depth 4 (< 5 s total in Release).
// Each position has a single forced best move verified against the engine.
//
// Selection invariant: every position in kFastCases must have a unique best move
// at depth 4. Verify with: go depth 4 on each FEN before committing new positions.
//
// See Docs/TestDesign.md §Phase 0 for rationale.

#include <catch_amalgamated.hpp>
#include "TacticalTestHelpers.h"
#include "Board.h"

// ---------------------------------------------------------------------------
// Position table — fast tier (depth 4, ~10 positions)
// ---------------------------------------------------------------------------

static constexpr TacticalCase kFastCases[] = {
    // — Mate-in-1 ————————————————————————————————————————————————————————
    { "M1: rook back rank",
      "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1",   a1, a8, 4 },
    { "M1: queen back rank",
      "6k1/5ppp/8/8/8/8/3Q4/6K1 w - - 0 1",     d2, d8, 4 },
    { "M1: rook d-file (Rd8#)",
      "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1",   d1, d8, 4 },
    { "M1: rook e-file (Re8#)",
      "6k1/5ppp/8/8/8/8/5PPP/4R1K1 w - - 0 1",   e1, e8, 4 },
    // — Winning captures ——————————————————————————————————————————————————
    { "capture: hanging rook (Qxc1)",
      "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1",         d1, c1, 4 },
    { "capture: hanging queen (Qxd5)",
      "4k3/8/8/3q4/8/8/8/3QK3 w - - 0 1",        d1, d5, 4 },
    { "capture: hanging knight (Bxf3)",
      "4k3/8/8/8/8/5n2/8/3BK3 w - - 0 1",        d1, f3, 4 },
    // — Simple 2-ply tactics ——————————————————————————————————————————————
    { "fork: Nc7+ wins Ra8",
      "r3k3/8/8/3N4/8/8/8/4K3 w - - 0 1",        d5, c7, 4 },
    { "skewer: Re8+ wins Ra8",
      "r3k3/8/8/8/8/8/8/4RK2 w - - 0 1",         e1, e8, 4 },
    { "skewer: Qc8+ wins Rg8",
      "4k1r1/5p2/8/8/2Q5/8/8/4K3 w - - 0 1",     c4, c8, 4 },
};

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

TEST_CASE("Tactical - fast suite", "[tactical]")
{
    auto tc = GENERATE(from_range(kFastCases));

    INFO(tc.label);
    Board board(tc.fen);
    auto ai = make_tactical_engine(board, tc.depth);
    GameInfo info = board.GetGameInfo();
    Move m = ai->GetMove(info);

    REQUIRE(m.from() == tc.expected_from);
    REQUIRE(m.to()   == tc.expected_to);
}

// ---------------------------------------------------------------------------
// Issue #66 regression: QFORK-001 from the exe tactical suite (Tests/
// tactical_test_cases.json). KQ vs KR is won via domination/zugzwang, and
// null-move pruning must not hide the rook win — the original zugzwang guard
// let the side with a lone rook "pass", flattening every winning line to bare
// material. Two moves win (Qa4+ fork, Qb3 threat), so this position cannot
// live in kFastCases (unique-best-move invariant).
// ---------------------------------------------------------------------------
TEST_CASE("Tactical - QFORK-001: zugzwang rook win survives null-move pruning (issue #66)", "[tactical]")
{
    Board board("8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1");
    auto ai = make_tactical_engine(board, 4);
    GameInfo info = board.GetGameInfo();
    Move m = ai->GetMove(info);

    INFO("engine played " << m.Output());
    REQUIRE(m.from() == d1);
    const bool wins_the_rook = (m.to() == a4) || (m.to() == b3);
    REQUIRE(wins_the_rook);
}
