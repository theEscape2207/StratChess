// UCITests.cpp — Catch2 [uci] tests for UciHandler::parse_go() and
// cmd_position() (via UciHandlerTestFixture, STRAT_ENABLE_TEST_ACCESS).

#include <catch_amalgamated.hpp>
#include "UCIHandler.h"
#include "AIPerplex.h"
#include "Board.h"
#include "MoveFormatter.h"
#include <sstream>
#include <string>

using P = UciHandler::GoParams;

// Must be defined here — the name must match the friend declaration inside
// UCIHandler.h: friend class UciHandlerTestFixture;
class UciHandlerTestFixture
{
public:
    UciHandler handler;

    void position(const std::string& line) { handler.cmd_position(line); }
    const Board& board() const { return handler.board_; }

    void setoption(const std::string& line) { handler.cmd_setoption(line); }
    void ucinewgame() { handler.cmd_ucinewgame(); }

    // Reads threads_ off the live ai_ instance (via the AIPerplex friend
    // declaration granted to this same fixture class name) — proves the
    // fix actually reaches the freshly-constructed AIPerplex, not just
    // UciHandler's own configured_threads_ bookkeeping.
    unsigned ai_threads() const
    {
        auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
        REQUIRE(perplex != nullptr);
        return perplex->threads_;
    }
};

// Builds a legal UCI move sequence of at least `min_plies` plies from the
// starting position: knight shuffles (Ng1-f3-g1 / Ng8-f6-g8) with a
// queenside pawn push every 80 plies so the game stays "real" (the pawn
// moves also keep the sequence trivially verifiable by hand).
static std::string long_game_moves(int min_plies)
{
    static const char* pawn_moves[] = {
        "a2a3", "a7a6", "b2b3", "b7b6", "c2c3", "c7c6", "d2d3", "d7d6",
        "a3a4", "a6a5", "b3b4", "b6b5", "c3c4", "c6c5", "d3d4", "d6d5",
    };
    std::string moves;
    int plies = 0;
    size_t pawn_i = 0;
    while (plies < min_plies) {
        if (plies % 80 == 0 && pawn_i + 1 < std::size(pawn_moves)) {
            moves += pawn_moves[pawn_i];     moves += ' ';
            moves += pawn_moves[pawn_i + 1]; moves += ' ';
            pawn_i += 2;
            plies += 2;
        }
        moves += "g1f3 g8f6 f3g1 f6g8 ";
        plies += 4;
    }
    moves.pop_back();   // trailing space
    return moves;
}

// ---------------------------------------------------------------------------
// parse_go — standard clock params
// ---------------------------------------------------------------------------

TEST_CASE("parse_go: wtime/btime/winc/binc", "[uci]")
{
    auto p = UciHandler::parse_go("go wtime 120000 btime 90000 winc 2000 binc 1000");
    REQUIRE(p.wtime     == 120000);
    REQUIRE(p.btime     == 90000);
    REQUIRE(p.winc      == 2000);
    REQUIRE(p.binc      == 1000);
    REQUIRE(p.movestogo == 0);
    REQUIRE(p.depth     == 0);
    REQUIRE(p.movetime  == 0);
    REQUIRE(p.infinite  == false);
}

TEST_CASE("parse_go: movestogo", "[uci]")
{
    auto p = UciHandler::parse_go("go wtime 60000 btime 60000 movestogo 20");
    REQUIRE(p.wtime     == 60000);
    REQUIRE(p.movestogo == 20);
}

TEST_CASE("parse_go: movetime", "[uci]")
{
    auto p = UciHandler::parse_go("go movetime 5000");
    REQUIRE(p.movetime == 5000);
    REQUIRE(p.wtime    == 0);
    REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: depth", "[uci]")
{
    auto p = UciHandler::parse_go("go depth 8");
    REQUIRE(p.depth    == 8);
    REQUIRE(p.movetime == 0);
    REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: infinite", "[uci]")
{
    auto p = UciHandler::parse_go("go infinite");
    REQUIRE(p.infinite == true);
    REQUIRE(p.wtime    == 0);
    REQUIRE(p.movetime == 0);
    REQUIRE(p.depth    == 0);
}

TEST_CASE("parse_go: depth + infinite", "[uci]")
{
    // Analysis mode: fixed depth, no time pressure
    auto p = UciHandler::parse_go("go infinite depth 10");
    REQUIRE(p.infinite == true);
    REQUIRE(p.depth    == 10);
}

// ---------------------------------------------------------------------------
// parse_go — robustness
// ---------------------------------------------------------------------------

TEST_CASE("parse_go: unknown tokens are silently skipped", "[uci]")
{
    // GUI may send tokens the engine doesn't know; must not crash or misparse.
    auto p = UciHandler::parse_go("go wtime 5000 ponder searchmoves e2e4 btime 4000");
    REQUIRE(p.wtime == 5000);
    REQUIRE(p.btime == 4000);
    // Unknown tokens ('ponder', 'searchmoves', 'e2e4') produce no field changes
    REQUIRE(p.depth    == 0);
    REQUIRE(p.movetime == 0);
    REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: bare 'go' with no params — all fields default", "[uci]")
{
    auto p = UciHandler::parse_go("go");
    REQUIRE(p.wtime     == 0);
    REQUIRE(p.btime     == 0);
    REQUIRE(p.winc      == 0);
    REQUIRE(p.binc      == 0);
    REQUIRE(p.movestogo == 0);
    REQUIRE(p.depth     == 0);
    REQUIRE(p.movetime  == 0);
    REQUIRE(p.infinite  == false);
}

TEST_CASE("parse_go: params in non-standard order", "[uci]")
{
    // UCI spec does not guarantee ordering — engine must handle any order.
    auto p = UciHandler::parse_go("go binc 500 btime 30000 movestogo 10 winc 1000 wtime 45000");
    REQUIRE(p.wtime     == 45000);
    REQUIRE(p.btime     == 30000);
    REQUIRE(p.winc      == 1000);
    REQUIRE(p.binc      == 500);
    REQUIRE(p.movestogo == 10);
}

// ---------------------------------------------------------------------------
// cmd_position — game-length replay
// ---------------------------------------------------------------------------

TEST_CASE("cmd_position: replay longer than MAX_PLY does not overflow ply history", "[uci]")
{
    // Regression (found by the first fastchess smoke match, 2026-07-03):
    // Board's undo-history arrays are std::array<..., MAX_PLY=256> indexed by
    // currentPly_, and cmd_position originally called ResetSearchDepth() only
    // AFTER the whole replay loop. A game longer than 256 plies therefore
    // wrote out of bounds DURING the replay — access violation in Release
    // (game 8 of the smoke match crashed at ply 265), assert in Debug.
    // The fix resets per replayed move, same as Game.cpp does per committed
    // move. In Release the pre-fix corruption is silent UB, so this test's
    // hard teeth are the Debug assert + the ground-truth state comparison.
    const std::string moves = long_game_moves(300);

    UciHandlerTestFixture fix;
    fix.position("position startpos moves " + moves);

    // Search-depth invariant: every replayed move is permanent, so the
    // undo cursor must be back at 0 when the replay finishes.
    REQUIRE(fix.board().GetSearchDepth() == 0);

    // Ground truth: the same sequence applied with a reset after every move
    // (the known-good permanent-move pattern) must yield the identical position.
    Board truth("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    std::istringstream ss(moves);
    std::string tok;
    int plies = 0;
    while (ss >> tok) {
        Move m = MoveFormatter::FromUCI(tok, truth);
        REQUIRE(!m.is_null());
        REQUIRE(truth.DoMove(m));
        truth.ResetSearchDepth();
        ++plies;
    }
    REQUIRE(plies >= 300);   // the scenario really is longer than MAX_PLY
    REQUIRE(fix.board().get_zobrist_hash() == truth.get_zobrist_hash());
}

// ---------------------------------------------------------------------------
// cmd_setoption Threads — survives cmd_ucinewgame()
// ---------------------------------------------------------------------------

TEST_CASE("cmd_setoption: Threads value survives cmd_ucinewgame()", "[uci][smp]")
{
    // Regression: cmd_ucinewgame() calls init_ai(), which used to construct
    // a brand-new AIPerplex whose threads_ always defaults to 1 — silently
    // discarding any prior 'setoption name Threads value N'. Standard UCI
    // usage is: client sends setoption once at session start, then sends
    // ucinewgame before every game — so this made Threads effectively
    // non-functional. Fixed via UciHandler::configured_threads_, restored
    // on every init_ai() call.
    UciHandlerTestFixture fix;

    // No ai_ yet (run() hasn't been called) — setoption must still record
    // the value for the next init_ai(), not just apply it to a live ai_.
    fix.setoption("setoption name Threads value 4");

    fix.ucinewgame();   // rebuilds ai_ via init_ai()

    REQUIRE(fix.ai_threads() == 4);
}

TEST_CASE("cmd_setoption: Threads takes effect immediately on the live ai_", "[uci][smp]")
{
    // A client may send setoption mid-session without an intervening
    // ucinewgame — the option must take effect right away, not only be
    // queued for the next init_ai().
    UciHandlerTestFixture fix;
    fix.ucinewgame();   // construct the initial ai_ (threads_ defaults to 1)
    REQUIRE(fix.ai_threads() == 1);

    fix.setoption("setoption name Threads value 4");
    REQUIRE(fix.ai_threads() == 4);   // applied to the existing ai_

    fix.ucinewgame();   // rebuild ai_ — must still restore 4, not reset to 1
    REQUIRE(fix.ai_threads() == 4);
}
