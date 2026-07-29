// UCITests.cpp — Catch2 [uci] tests for UciHandler::parse_go() and
// cmd_position() (via UciHandlerTestFixture, STRAT_ENABLE_TEST_ACCESS).

#include <catch_amalgamated.hpp>
#include "UCIHandler.h"
#include "AIPerplex.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "Eval.h"
#include "Utils/FenBatch.h"
#include <sstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

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
    void eval() { handler.cmd_eval(); }

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

// ---------------------------------------------------------------------------
// cmd_eval — static evaluation introspection (issue #129 phase 1)
// ---------------------------------------------------------------------------

// Redirects std::cout into an in-memory buffer for the lifetime of the
// object; restores the original streambuf on destruction (including when
// unwinding past a failed REQUIRE), so a single assertion failure can never
// leave std::cout silently rewired for the rest of the test binary.
class CoutRedirect
{
public:
    CoutRedirect() : old_(std::cout.rdbuf(buffer_.rdbuf())) {}
    ~CoutRedirect() { std::cout.rdbuf(old_); }

    CoutRedirect(const CoutRedirect&) = delete;
    CoutRedirect& operator=(const CoutRedirect&) = delete;

    std::string str() const { return buffer_.str(); }

private:
    std::ostringstream buffer_;
    std::streambuf* old_;
};

// Parses the integer centipawn value out of a "<label><N> cp" line, e.g.
// label="static eval: " on "static eval: 34 cp (White to move; ...)".
// A real parse of the emitted number, not a substring check — this is what
// makes the honesty-invariant test below actually load-bearing.
//
// The label is matched at the start of a line, not anywhere in the output.
// Phase 2's breakdown prints a "sum (white pov)" line above "white pov:", and
// the two are distinguished today only by the trailing colon: an unanchored
// find() would start silently reading the wrong line the moment anyone added
// one — turning the sign-convention assertions below into confident nonsense
// rather than a failure.
static int extract_cp_score(const std::string& output, const std::string& label)
{
    const std::string anchored = "\n" + output;
    const auto label_pos = anchored.find("\n" + label);
    REQUIRE(label_pos != std::string::npos);

    const auto value_start = label_pos + 1 + label.size();
    const auto value_end = anchored.find(" cp", value_start);
    REQUIRE(value_end != std::string::npos);
    return std::stoi(anchored.substr(value_start, value_end - value_start));
}

TEST_CASE("cmd_eval: works before any position command, does not crash", "[uci]")
{
    UciHandlerTestFixture fix;
    CoutRedirect redirect;
    REQUIRE_NOTHROW(fix.eval());

    const std::string out = redirect.str();
    REQUIRE(out.find("static eval:") != std::string::npos);
    REQUIRE(out.find("white pov:") != std::string::npos);
}

TEST_CASE("cmd_eval: printed score matches EvalManager::Evaluate() directly (honesty invariant)", "[uci]")
{
    // The property that makes this tool trustworthy for #117 (Texel tuning)
    // and #127 (EvalContext restructure's byte-identity check): 'eval' must
    // never compute its own parallel score, only report the same Evaluate()
    // the search calls.
    const std::string fen =
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";   // Kiwipete

    UciHandlerTestFixture fix;
    fix.position("position fen " + fen);

    CoutRedirect redirect;
    fix.eval();
    const int printed = extract_cp_score(redirect.str(), "static eval:");

    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    Board board(fen);
    const int expected = eval->Evaluate(board);

    REQUIRE(printed == expected);
}

TEST_CASE("cmd_eval: output contains neither bestmove nor info", "[uci]")
{
    // 'eval' is not a search response — a GUI must not mistake it for one.
    UciHandlerTestFixture fix;
    fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    CoutRedirect redirect;
    fix.eval();
    const std::string out = redirect.str();

    REQUIRE(out.find("bestmove") == std::string::npos);
    REQUIRE(out.find("info") == std::string::npos);
}

TEST_CASE("cmd_eval: white-pov line matches the stated sign convention", "[uci]")
{
    // Both positions have a large, unambiguous material imbalance (a whole
    // rook) so a sign bug cannot hide behind a near-zero score.
    UciHandlerTestFixture fix;

    SECTION("White to move — white pov equals the side-to-move score") {
        fix.position("position fen 4k3/8/8/8/8/8/8/R3K3 w - - 0 1");   // White up a rook

        CoutRedirect redirect;
        fix.eval();
        const std::string out = redirect.str();

        const int side_to_move_score = extract_cp_score(out, "static eval:");
        const int white_pov = extract_cp_score(out, "white pov:");

        REQUIRE(side_to_move_score != 0);
        REQUIRE(white_pov == side_to_move_score);
    }

    SECTION("Black to move — white pov is the negation of the side-to-move score") {
        fix.position("position fen r3k3/8/8/8/8/8/8/4K3 b - - 0 1");   // Black up a rook

        CoutRedirect redirect;
        fix.eval();
        const std::string out = redirect.str();

        const int side_to_move_score = extract_cp_score(out, "static eval:");
        const int white_pov = extract_cp_score(out, "white pov:");

        REQUIRE(side_to_move_score != 0);
        REQUIRE(white_pov == -side_to_move_score);
    }
}

// ---------------------------------------------------------------------------
// cmd_eval — per-term breakdown (issue #129 phase 2)
// ---------------------------------------------------------------------------
//
// These assert on the *printed* table rather than on EvalBreakdown directly.
// A breakdown that is right internally and mis-rendered is still a debugging
// tool that lies, and #117 (Texel tuning) will be reading the output, not the
// struct. The struct-level check that the rows really are the same terms
// EvalComplex::Evaluate() sums lives in EvalTests.cpp ([eval]).

struct EvalTermRow { int white; int black; int net; };

static std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);
    return lines;
}

// Pulls one "<term> | <white> | <black> | <net>" row out of the printed table.
// Matches on the first whitespace-delimited token so column padding is not
// baked into the test — a change to the column widths must not silently turn
// these assertions into no-ops.
static EvalTermRow extract_term_row(const std::string& output, const std::string& term)
{
    EvalTermRow row{};
    bool found = false;

    for (const std::string& line : split_lines(output)) {
        std::istringstream head(line);
        std::string first;
        if (!(head >> first) || first != term)
            continue;

        const auto bar = line.find('|');
        REQUIRE(bar != std::string::npos);
        std::string values = line.substr(bar + 1);
        std::replace(values.begin(), values.end(), '|', ' ');

        std::istringstream parsed(values);
        REQUIRE(static_cast<bool>(parsed >> row.white >> row.black >> row.net));
        found = true;
        break;
    }

    REQUIRE(found);
    return row;
}

// The "sum (white pov)" line: the label followed by the right-aligned figure.
static int extract_sum_white_pov(const std::string& output)
{
    static const std::string kLabel = "sum (white pov)";
    const auto label_pos = output.find(kLabel);
    REQUIRE(label_pos != std::string::npos);

    const auto line_end = output.find('\n', label_pos);
    const auto value_start = label_pos + kLabel.size();
    const std::string value = (line_end != std::string::npos)
        ? output.substr(value_start, line_end - value_start)
        : output.substr(value_start);

    return std::stoi(value);
}

// Every term the breakdown prints, in table order. Material is a row like any
// other: it is the largest single contribution and a sign error there would be
// the easiest one to miss.
static const char* const kBreakdownTerms[] = { "material", "pawns", "rooks", "pst", "mopup" };

TEST_CASE("cmd_eval: printed breakdown nets are white-minus-black and sum to the evaluator's score", "[uci]")
{
    // The phase 2 honesty invariant (D9). Three separate claims, each of which
    // has failed in some engine at some point: each row's net is consistent
    // with its own two columns; the net column sums to the printed total; and
    // that total is the score the search would actually see, computed here
    // from a fresh Board through a separate EvalManager instance.
    const char* fen = GENERATE(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",   // Kiwipete, middlegame
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",               // startpos, symmetric
        "8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1",                                       // endgame, White to move
        "r3k3/8/8/8/8/8/8/4K3 b - - 0 1",                                         // Black to move, Black up a rook
        "8/8/8/4k3/8/8/8/3QK3 w - - 0 1");                                        // pawnless K+Q vs K — mop-up active
    CAPTURE(fen);

    UciHandlerTestFixture fix;
    fix.position(std::string("position fen ") + fen);

    CoutRedirect redirect;
    fix.eval();
    const std::string out = redirect.str();

    int net_sum = 0;
    for (const char* term : kBreakdownTerms) {
        CAPTURE(term);
        const EvalTermRow row = extract_term_row(out, term);
        REQUIRE(row.net == row.white - row.black);
        net_sum += row.net;
    }

    REQUIRE(net_sum == extract_sum_white_pov(out));
    REQUIRE(net_sum == extract_cp_score(out, "white pov:"));

    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);
    Board board(fen);
    const int side_to_move_score = eval->Evaluate(board);
    const int expected_white_pov = (board.GetCurrentColor() == WHITE)
        ? side_to_move_score : -side_to_move_score;

    REQUIRE(net_sum == expected_white_pov);
}

TEST_CASE("cmd_eval: breakdown reports the game phase the evaluator computed", "[uci]")
{
    // Phase is printed because it is not derivable from the rows, yet it sets
    // where between the mg and eg endpoints every tapered term landed — so a
    // reader debugging the pst row needs it to interpret that row at all
    // (issue #99, replacing the old middlegame/endgame stage name).
    UciHandlerTestFixture fix;

    SECTION("full starting material is the maximum phase") {
        fix.position("position startpos");

        CoutRedirect redirect;
        fix.eval();
        REQUIRE(redirect.str().find("phase: 24/24") != std::string::npos);
    }

    SECTION("bare kings plus a queen is deep in the endgame") {
        fix.position("position fen 4k3/8/8/8/8/8/8/3QK3 w - - 0 1");

        CoutRedirect redirect;
        fix.eval();
        REQUIRE(redirect.str().find("phase: 4/24") != std::string::npos);
    }
}

TEST_CASE("cmd_eval: a term that is active for exactly one side shows it in the per-color split", "[uci]")
{
    // The per-color columns are the reason for the table's shape (D10): a net
    // of -30 does not say whether the pawn term is penalising White or
    // rewarding Black. This pins that the columns carry that information
    // rather than both being derived from the net.
    //
    // Pawnless K+Q vs K, White winning decisively: eval_mopup is gated on a
    // 400 cp material lead, so it is active for White and inert for Black.
    //
    // The losing king is cornered on a8 rather than centralised, so both of
    // eval_mopup's components contribute. With the king on e5 its
    // center-manhattan-distance is 0 and the entire MOPUP_CMD_WEIGHT term
    // drops out — the assertion would then rest solely on the king-distance
    // component, and would still pass with MOPUP_CMD_WEIGHT zeroed.
    UciHandlerTestFixture fix;
    fix.position("position fen k7/8/8/8/8/8/8/3QK3 w - - 0 1");

    CoutRedirect redirect;
    fix.eval();
    const EvalTermRow mopup = extract_term_row(redirect.str(), "mopup");

    REQUIRE(mopup.white > 0);
    REQUIRE(mopup.black == 0);
}

// ---------------------------------------------------------------------------
// FenBatch::ClassifyLine — batch-mode FEN validation (issue #140)
// ---------------------------------------------------------------------------
//
// evalrunner() (StratChessEvolved.cpp, issue #129 phase 1) is untestable
// directly — it's a static function in a translation unit the test project
// does not link. FenBatch::ClassifyLine (StratEngine/Utils/FenBatch.h) is
// the extracted, header-only classification it now delegates to, so these
// cases exercise the same two-tier guard evalrunner() relies on: without
// it, a malformed line would silently score a fresh, empty Board as 0 —
// plausible-looking garbage in a tuning corpus rather than an obvious
// failure.

TEST_CASE("FenBatch::ClassifyLine: blank line is Skip", "[uci]")
{
    auto r = FenBatch::ClassifyLine("");
    REQUIRE(r.kind == FenBatch::LineKind::Skip);

    auto r_ws = FenBatch::ClassifyLine("   \t  ");
    REQUIRE(r_ws.kind == FenBatch::LineKind::Skip);
}

TEST_CASE("FenBatch::ClassifyLine: '#' comment line is Skip", "[uci]")
{
    auto r = FenBatch::ClassifyLine("# this is a comment");
    REQUIRE(r.kind == FenBatch::LineKind::Skip);
}

TEST_CASE("FenBatch::ClassifyLine: 2-field line is Malformed with the field-count message", "[uci]")
{
    auto r = FenBatch::ClassifyLine("8/8/8/8/8/8/8/8 w");
    REQUIRE(r.kind == FenBatch::LineKind::Malformed);
    REQUIRE_FALSE(r.error.empty());
    REQUIRE(r.error.find("too few fields") != std::string::npos);
}

TEST_CASE("FenBatch::ClassifyLine: 4-field line is Valid", "[uci]")
{
    // EPD corpora and hand-authored positions are overwhelmingly 4-field, so this is the form
    // #117's tuning work needs to ingest.
    auto r = FenBatch::ClassifyLine("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -");
    REQUIRE(r.kind == FenBatch::LineKind::Valid);
    REQUIRE(r.error.empty());
}

TEST_CASE("FenBatch::ClassifyLine: 5-field line is Valid", "[uci]")
{
    auto r = FenBatch::ClassifyLine("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 3");
    REQUIRE(r.kind == FenBatch::LineKind::Valid);
    REQUIRE(r.error.empty());
}

TEST_CASE("FenBatch::ClassifyLine: EPD operations are still rejected", "[uci]")
{
    // Accepting 4-6 fields is deliberately NOT the same as accepting EPD. Trailing operations
    // are out of scope for the FEN grammar; #117's corpus loader owns them.
    auto r = FenBatch::ClassifyLine(R"(rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - c9 "1-0";)");
    REQUIRE(r.kind == FenBatch::LineKind::Malformed);
    REQUIRE_FALSE(r.error.empty());
}

// ---------------------------------------------------------------------------
// FENParser::ParseFEN — optional halfmove/fullmove fields (issue #143)
// ---------------------------------------------------------------------------

TEST_CASE("FENParser::ParseFEN: 4-field FEN defaults halfmove to 0 and fullmove to 1", "[uci]")
{
    FENParser::FENGameState state;
    std::vector<std::tuple<ePiece, eSquare>> pieces;
    auto err = FENParser::ParseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -", state, pieces);

    REQUIRE_FALSE(err.has_value());
    CHECK(state.sideToMove == eColor::WHITE);
    CHECK(state.halfMoveClock == 0);
    CHECK(state.fullMoveCounter == 1);
}

TEST_CASE("FENParser::ParseFEN: 5-field FEN keeps the halfmove clock, defaults fullmove to 1", "[uci]")
{
    FENParser::FENGameState state;
    std::vector<std::tuple<ePiece, eSquare>> pieces;
    auto err = FENParser::ParseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 7", state, pieces);

    REQUIRE_FALSE(err.has_value());
    CHECK(state.sideToMove == eColor::BLACK);
    CHECK(state.halfMoveClock == 7);
    CHECK(state.fullMoveCounter == 1);
}

TEST_CASE("FENParser::ParseFEN: 6-field FEN is unaffected by the relaxation", "[uci]")
{
    FENParser::FENGameState state;
    std::vector<std::tuple<ePiece, eSquare>> pieces;
    auto err = FENParser::ParseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 12 34", state, pieces);

    REQUIRE_FALSE(err.has_value());
    CHECK(state.halfMoveClock == 12);
    CHECK(state.fullMoveCounter == 34);
}

TEST_CASE("FENParser::ParseFEN: fewer than 4 fields reports the field-count error", "[uci]")
{
    FENParser::FENGameState state;
    std::vector<std::tuple<ePiece, eSquare>> pieces;
    auto err = FENParser::ParseFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq", state, pieces);

    REQUIRE(err.has_value());
    CHECK(err->find("too few fields") != std::string::npos);
}

TEST_CASE("Board::SetupFromFEN: 4-field FEN yields fiftyCount 0", "[uci]")
{
    // The halfmove default is not inert: SetupFromFEN feeds it into gameInfo_.fiftyCount,
    // which drives 50-move draw detection. Pin the value rather than leave it implicit.
    Board board;
    board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -");

    // SetupFromFEN returns void and applies nothing on a parse error, so assert the position
    // actually landed -- otherwise a rejected FEN would leave an empty board whose fiftyCount
    // is also 0, and this test would pass for the wrong reason.
    REQUIRE(board.GetPiece(e1) == WHITE_KING);
    REQUIRE(board.GetPiece(e8) == BLACK_KING);
    CHECK(board.GetGameInfo().fiftyCount == 0);
}

TEST_CASE("FenBatch::ClassifyLine: valid 6-field White-to-move FEN is Valid", "[uci]")
{
    auto r = FenBatch::ClassifyLine("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    REQUIRE(r.kind == FenBatch::LineKind::Valid);
    REQUIRE(r.error.empty());
}

TEST_CASE("FenBatch::ClassifyLine: valid 6-field Black-to-move FEN is Valid", "[uci]")
{
    auto r = FenBatch::ClassifyLine("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R b KQkq - 0 1");
    REQUIRE(r.kind == FenBatch::LineKind::Valid);
    REQUIRE(r.error.empty());
}
