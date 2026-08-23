// UCITests.cpp — Catch2 [uci] tests for the session/administrative commands:
// parse_go() parameter parsing, cmd_position, cmd_setoption, cmd_ucinewgame,
// dispatch() and the received-command log. cmd_go/cmd_eval/cmd_perft (the
// commands that run search and report on it) are in UCIReportingTests.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "UCITestFixture.h"
#include "AIPerplex.h"
#include "Board.h"
#include "MoveFactory.h"
#include "MoveFormatter.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using P = UciHandler::GoParams;

// ---------------------------------------------------------------------------
// parse_go — standard clock params
// ---------------------------------------------------------------------------

TEST_CASE("parse_go: wtime/btime/winc/binc", "[uci]")
{
	auto p = UciHandler::parse_go("go wtime 120000 btime 90000 winc 2000 binc 1000");
	REQUIRE(p.wtime == 120000);
	REQUIRE(p.btime == 90000);
	REQUIRE(p.winc == 2000);
	REQUIRE(p.binc == 1000);
	REQUIRE(p.movestogo == 0);
	REQUIRE(p.depth == 0);
	REQUIRE(p.movetime == 0);
	REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: movestogo", "[uci]")
{
	auto p = UciHandler::parse_go("go wtime 60000 btime 60000 movestogo 20");
	REQUIRE(p.wtime == 60000);
	REQUIRE(p.movestogo == 20);
}

TEST_CASE("parse_go: movetime", "[uci]")
{
	auto p = UciHandler::parse_go("go movetime 5000");
	REQUIRE(p.movetime == 5000);
	REQUIRE(p.wtime == 0);
	REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: depth", "[uci]")
{
	auto p = UciHandler::parse_go("go depth 8");
	REQUIRE(p.depth == 8);
	REQUIRE(p.movetime == 0);
	REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: infinite", "[uci]")
{
	auto p = UciHandler::parse_go("go infinite");
	REQUIRE(p.infinite == true);
	REQUIRE(p.wtime == 0);
	REQUIRE(p.movetime == 0);
	REQUIRE(p.depth == 0);
}

TEST_CASE("parse_go: depth + infinite", "[uci]")
{
	// Analysis mode: fixed depth, no time pressure
	auto p = UciHandler::parse_go("go infinite depth 10");
	REQUIRE(p.infinite == true);
	REQUIRE(p.depth == 10);
}

TEST_CASE("parse_go: nodes", "[uci]")
{
	auto p = UciHandler::parse_go("go nodes 20000");
	REQUIRE(p.nodes == 20000);
	REQUIRE(p.movetime == 0);
	REQUIRE(p.depth == 0);
	REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: no nodes token — p.nodes stays 0", "[uci]")
{
	auto p = UciHandler::parse_go("go wtime 60000 btime 60000");
	REQUIRE(p.nodes == 0);
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
	REQUIRE(p.depth == 0);
	REQUIRE(p.movetime == 0);
	REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: bare 'go' with no params — all fields default", "[uci]")
{
	auto p = UciHandler::parse_go("go");
	REQUIRE(p.wtime == 0);
	REQUIRE(p.btime == 0);
	REQUIRE(p.winc == 0);
	REQUIRE(p.binc == 0);
	REQUIRE(p.movestogo == 0);
	REQUIRE(p.depth == 0);
	REQUIRE(p.movetime == 0);
	REQUIRE(p.infinite == false);
}

TEST_CASE("parse_go: params in non-standard order", "[uci]")
{
	// UCI spec does not guarantee ordering — engine must handle any order.
	auto p = UciHandler::parse_go("go binc 500 btime 30000 movestogo 10 winc 1000 wtime 45000");
	REQUIRE(p.wtime == 45000);
	REQUIRE(p.btime == 30000);
	REQUIRE(p.winc == 1000);
	REQUIRE(p.binc == 500);
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
	REQUIRE(plies >= 300); // the scenario really is longer than MAX_PLY
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

	fix.ucinewgame(); // rebuilds ai_ via init_ai()

	REQUIRE(fix.ai_threads() == 4);
}

TEST_CASE("cmd_setoption: Threads takes effect immediately on the live ai_", "[uci][smp]")
{
	// A client may send setoption mid-session without an intervening
	// ucinewgame — the option must take effect right away, not only be
	// queued for the next init_ai().
	UciHandlerTestFixture fix;
	fix.ucinewgame(); // construct the initial ai_ (threads_ defaults to 1)
	REQUIRE(fix.ai_threads() == 1);

	fix.setoption("setoption name Threads value 4");
	REQUIRE(fix.ai_threads() == 4); // applied to the existing ai_

	fix.ucinewgame(); // rebuild ai_ — must still restore 4, not reset to 1
	REQUIRE(fix.ai_threads() == 4);
}

// ---------------------------------------------------------------------------
// cmd_ucinewgame — StartNewGame() lifecycle, not a rebuild
// ---------------------------------------------------------------------------

TEST_CASE("cmd_ucinewgame: does not rebuild the AIPerplex instance", "[uci]")
{
	UciHandlerTestFixture fix;
	fix.ucinewgame(); // first call still constructs ai_
	const void* first = fix.ai_identity();

	fix.ucinewgame();
	fix.ucinewgame();

	REQUIRE(fix.ai_identity() == first);
}

TEST_CASE("cmd_ucinewgame: a TT entry does not survive into the next game", "[uci][tt]")
{
	UciHandlerTestFixture fix;
	fix.ucinewgame();
	fix.store_tt_marker();
	REQUIRE(fix.has_tt_marker());

	fix.ucinewgame();

	REQUIRE_FALSE(fix.has_tt_marker());
}

TEST_CASE("cmd_uci: advertises Hash exact-fit default and policy bounds", "[uci][tt]")
{
	UciHandlerTestFixture fix;
	const std::string output = capture_cout([&] { fix.uci(); });

	REQUIRE(output.find("option name Hash type spin default 192 min 1 max 1536\n") != std::string::npos);
}

TEST_CASE("AIPerplex default Hash has the documented exact-fit geometry", "[uci][tt]")
{
	UciHandlerTestFixture fix;
	fix.ucinewgame();

	REQUIRE(fix.ai_hash_requested_mb() == AIPerplex::DEFAULT_HASH_MB);
	REQUIRE(fix.ai_hash_bucket_count() == 2097152u);
	REQUIRE(fix.ai_hash_memory_mb() == 192);
}

TEST_CASE("cmd_setoption: Hash replaces and reports the live table, then survives ucinewgame", "[uci][tt]")
{
	UciHandlerTestFixture fix;
	fix.ucinewgame();
	const void* original = fix.tt_identity();
	fix.store_tt_marker();

	const std::string output = capture_cout([&] { fix.setoption("setoption name Hash value 6"); });

	REQUIRE(output == "info string hash 6 MiB (65536 buckets)\n");
	REQUIRE(fix.tt_identity() != original);
	REQUIRE_FALSE(fix.has_tt_marker());
	REQUIRE(fix.ai_hash_requested_mb() == 6);
	REQUIRE(fix.ai_hash_memory_mb() == 6);
	REQUIRE(fix.ai_hash_bucket_count() == 65536u);

	const void* configured = fix.tt_identity();
	fix.ucinewgame();
	REQUIRE(fix.tt_identity() == configured);
	REQUIRE(fix.ai_hash_requested_mb() == 6);
	REQUIRE(fix.ai_hash_memory_mb() == 6);
}

TEST_CASE("cmd_setoption: Hash reports round-down and the sub-MiB minimum", "[uci][tt]")
{
	UciHandlerTestFixture fix;
	fix.ucinewgame();

	const std::string rounded = capture_cout([&] { fix.setoption("setoption name Hash value 5"); });
	REQUIRE(rounded == "info string hash 3 MiB (32768 buckets)\n");
	REQUIRE(fix.ai_hash_requested_mb() == 5);
	REQUIRE(fix.ai_hash_memory_mb() == 3);

	const std::string minimum = capture_cout([&] { fix.setoption("setoption name Hash value 0"); });
	REQUIRE(minimum == "info string hash 0 MiB (8192 buckets)\n");
	REQUIRE(fix.ai_hash_requested_mb() == 1);
	REQUIRE(fix.ai_hash_memory_mb() == 0);
	REQUIRE(fix.ai_hash_bucket_count() == 8192u);
}

TEST_CASE("cmd_setoption: malformed Hash leaves the live table unchanged", "[uci][tt]")
{
	UciHandlerTestFixture fix;
	fix.ucinewgame();
	const void* original = fix.tt_identity();

	const std::string output = capture_cout([&] { fix.setoption("setoption name Hash value nope"); });

	REQUIRE(output.empty());
	REQUIRE(fix.tt_identity() == original);
}

TEST_CASE("cmd_setoption: Hash replacement is refused while a search is running", "[uci][tt]")
{
	UciHandlerTestFixture fix;
	fix.ucinewgame();
	capture_cout([&] { fix.setoption("setoption name Hash value 6"); });
	const void* configured = fix.tt_identity();

	fix.set_searching(true);
	const std::string output = capture_cout([&] { fix.setoption("setoption name Hash value 12"); });
	fix.set_searching(false);

	REQUIRE(output == "info string setoption: ignored, a search is in progress -- send 'stop' first\n");
	REQUIRE(fix.tt_identity() == configured);
	REQUIRE(fix.ai_hash_requested_mb() == 6);
}

// The board never carries DRAW_50_MOVES: Game::Run adjudicates the fifty-move rule.
// A high clock therefore has to be handled by the search itself, not by the seed (#345).
TEST_CASE("UCI: a high halfmove clock still yields a searched move", "[uci]")
{
	const int clock = GENERATE(50, 99, 100);
	INFO("halfmove clock: " << clock);

	UciHandlerTestFixture fx;
	fx.ucinewgame(); // constructs ai_, which run_search_directly needs
	fx.position("position fen r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - " + std::to_string(clock) +
	            " 4");
	REQUIRE(fx.board().halfmove_clock() == clock);

	const SearchResult result = fx.run_search_directly(4);

	CHECK_FALSE(result.best_move.is_null());
	CHECK(result.nodes_searched > 0);
}

// ---------------------------------------------------------------------------
// Board::SetupFromFEN failure reporting, and cmd_position's response to it
// (issues #155, #46)
// ---------------------------------------------------------------------------

TEST_CASE("cmd_position: malformed FEN resets to the start position and reports it", "[uci]")
{
	UciHandlerTestFixture fx;
	fx.position("position startpos moves e2e4");

	REQUIRE(fx.board().GetPiece(e4) == WHITE_PAWN);
	REQUIRE(fx.board().GetCurrentColor() == BLACK);

	const std::string output = capture_cout([&] { fx.position("position fen this-is-not-a-fen"); });

	// The e2e4 position is gone: keeping it would make the engine answer for a
	// position the caller never sent, and the answer would depend on session
	// history (#200).
	CHECK(fx.board().GetPiece(e4) == NO_PIECE);
	CHECK(fx.board().GetPiece(e2) == WHITE_PAWN);
	CHECK(fx.board().GetCurrentColor() == WHITE);
	CHECK(output.find("info string") != std::string::npos);
}

// Issue #46: a FEN with the side-to-move field omitted. Since #143 added the field-count floor the
// parser rejects it outright, so the engine can no longer silently decide it is Black's move. The
// board is reset to the start position (#200); here the prior position was already the start
// position, so "reset" and "unchanged" coincide.
TEST_CASE("cmd_position: FEN missing the side-to-move field is declined", "[uci]")
{
	UciHandlerTestFixture fx;
	fx.position("position startpos");

	const std::string before = fx.board().ExtractFEN();

	capture_cout([&] { fx.position("position fen 6k1/5ppp/8/8/8/8/5PPP/R5K1"); });

	CHECK(fx.board().ExtractFEN() == before);
	CHECK(fx.board().GetCurrentColor() == WHITE);
}

// The move list is parsed after the position block, so a declined FEN must abandon the whole
// command — the moves describe a position that was never established.
TEST_CASE("cmd_position: malformed FEN does not replay its move list", "[uci]")
{
	UciHandlerTestFixture fx;
	fx.position("position startpos");

	const std::string before = fx.board().ExtractFEN();

	capture_cout([&] { fx.position("position fen 6k1/5ppp/8/8/8/8/5PPP/R5K1 moves e2e4 e7e5"); });

	CHECK(fx.board().ExtractFEN() == before);
	CHECK(fx.board().GetPiece(e2) == WHITE_PAWN);
	CHECK(fx.board().GetPiece(e4) == NO_PIECE);
}

TEST_CASE("cmd_position: an illegal move rejects the entire replay", "[uci]")
{
	UciHandlerTestFixture fx;
	const std::string output = capture_cout([&] { fx.position("position startpos moves e2e4 e7e8"); });

	// e7e8 is coordinate-shaped but illegal: it targets Black's own king. The valid
	// prefix must not remain applied when a later token invalidates the whole replay.
	CHECK(output.find("illegal move 'e7e8'") != std::string::npos);
	CHECK(fx.board().GetPiece(e2) == WHITE_PAWN);
	CHECK(fx.board().GetPiece(e4) == NO_PIECE);
	CHECK(fx.board().GetPiece(e7) == BLACK_PAWN);
	CHECK(fx.board().GetPiece(e8) == BLACK_KING);
}

TEST_CASE("cmd_position: an oversized move token rejects the entire replay", "[uci]")
{
	UciHandlerTestFixture fx;
	// "e2e4xx" is a well-formed 4-char prefix with trailing garbage — FromUCI must
	// reject the whole token rather than silently parsing just the prefix.
	const std::string output = capture_cout([&] { fx.position("position startpos moves e2e4xx"); });

	CHECK(output.find("illegal move 'e2e4xx'") != std::string::npos);
	CHECK(fx.board().GetPiece(e2) == WHITE_PAWN);
	CHECK(fx.board().GetPiece(e4) == NO_PIECE);
}

TEST_CASE("cmd_position: legal promotion replay preserves the requested piece", "[uci]")
{
	UciHandlerTestFixture fx;
	const std::string output =
	    capture_cout([&] { fx.position("position fen 4k3/1P6/8/8/8/8/8/4K3 w - - 0 1 moves b7b8n"); });

	CHECK(output.find("illegal move") == std::string::npos);
	CHECK(fx.board().GetPiece(b7) == NO_PIECE);
	CHECK(fx.board().GetPiece(b8) == WHITE_KNIGHT);
}

// A move list past the longest possible game (GameState.h's MAX_UCI_REPLAY_PLIES) describes no
// real game and is refused whole, exactly like an illegal token: "position startpos" itself still
// applies (same as the oversized-token and illegal-move cases above), but none of the move list
// does -- the board is left at plain startpos, not partway through the (uncountably long) list.
TEST_CASE("cmd_position: a move list longer than MAX_UCI_REPLAY_PLIES is refused whole", "[uci]")
{
	UciHandlerTestFixture fx;

	const std::string tooLong = long_game_moves(static_cast<int>(MAX_UCI_REPLAY_PLIES) + 4);
	const std::string output = capture_cout([&] { fx.position("position startpos moves " + tooLong); });

	CHECK(output.find("move list too long") != std::string::npos);
	CHECK(fx.board().GetPiece(e2) == WHITE_PAWN);
	CHECK(fx.board().GetPiece(g1) == WHITE_KNIGHT);
	CHECK(fx.board().GetCurrentColor() == WHITE);
}

// Previously the most destructive response (whole FEN rejected) was reserved for the
// least-destructive-looking input: an en-passant square whose rank isn't 3 or 6 at all.
// Now it is repaired like every other inconsistent ep square, and the position is kept.
TEST_CASE("cmd_position: en-passant square on a non-3/6 rank is repaired, not rejected", "[uci]")
{
	UciHandlerTestFixture fx;
	const std::string output = capture_cout([&] { fx.position("position fen 4k3/8/8/8/8/8/8/4K3 w - d5 0 1"); });

	CHECK(fx.board().GetPiece(e1) == WHITE_KING);
	CHECK(fx.board().GetPiece(e8) == BLACK_KING);
	CHECK(fx.board().ep_square() == NO_SQUARE);
	CHECK(output.find("rejected FEN") == std::string::npos);
	CHECK(output.find("info string position:") != std::string::npos);
}

// This repair already happened before #221; what was missing is that spdlog is off in UCI
// mode, so the client had no way to see it. a3 is right-rank-shaped but wrong for White to
// move (needs rank 6, not 3).
TEST_CASE("cmd_position: en-passant square inconsistent with side to move is repaired and reported", "[uci]")
{
	UciHandlerTestFixture fx;
	const std::string output = capture_cout([&] { fx.position("position fen 4k3/8/8/8/8/8/8/4K3 w - a3 0 1"); });

	CHECK(fx.board().ep_square() == NO_SQUARE);
	CHECK(output.find("rank inconsistent") != std::string::npos);
}

// Right rank for the side to move, but no pawn on the square the capture would remove.
TEST_CASE("cmd_position: en-passant square with no pawn to capture is repaired and reported", "[uci]")
{
	UciHandlerTestFixture fx;
	const std::string output = capture_cout([&] { fx.position("position fen 4k3/8/8/8/8/8/8/4K3 b - e3 0 1"); });

	CHECK(fx.board().ep_square() == NO_SQUARE);
	CHECK(output.find("no pawn") != std::string::npos);
}

// The control: a fix that cleared en-passant unconditionally would pass every test above for
// the wrong reason. This one must keep it, and the capture must still be playable.
TEST_CASE("cmd_position: a legal en-passant square is preserved, not cleared", "[uci]")
{
	UciHandlerTestFixture fx;
	const std::string output = capture_cout([&] { fx.position("position fen 8/8/8/3Pp3/8/8/8/4K2k w - e6 0 1"); });

	CHECK(fx.board().ep_square() == e6);
	CHECK(output.find("info string") == std::string::npos);

	// fx.board() is const; the capture itself is checked on an independently loaded board.
	Board board("8/8/8/3Pp3/8/8/8/4K2k w - e6 0 1");
	auto ep = MoveFactory::MakeEnPassant(d5, e6);
	CHECK(board.DoMove(ep));
}

// Castling repair (king or rook missing from where the rights claim) already worked; it was
// equally invisible over UCI. Reuses the two-corrections-at-once FEN from
// FENParser::ValidatePositionAgainstFENMetadata's own throwing-sink test.
TEST_CASE("cmd_position: castling repair is reported via UCI (spdlog is off there)", "[uci]")
{
	UciHandlerTestFixture fx;
	const std::string output = capture_cout([&] { fx.position("position fen 4k3/8/8/8/4P3/8/8/3K4 w Q e6 0 1"); });

	CHECK(fx.board().castling_rights() == CastlingRights::NONE);
	CHECK(fx.board().ep_square() == NO_SQUARE);
	CHECK(output.find("king not on") != std::string::npos);
	CHECK(output.find("no pawn") != std::string::npos);
}

TEST_CASE("cmd_position: an illegal position is declined", "[uci]")
{
	UciHandlerTestFixture fx;
	fx.position("position startpos");

	const std::string before = fx.board().ExtractFEN();

	capture_cout([&] { fx.position("position fen 4k3/8/8/8/8/5b2/8/4RK2 w - - 0 1"); });

	CHECK(fx.board().ExtractFEN() == before);
}

// ---------------------------------------------------------------------------
// A rejected FEN must not leave the previous position on the board (#200).
//
// The load-bearing property is that the answer does not depend on what was
// loaded before: the same rejected FEN from two different prior positions must
// leave the same board. divide_total (UCITestFixture.h) is used here purely as
// a convenient UCI-level oracle for "what position is the board actually in".
// ---------------------------------------------------------------------------

namespace {

	// Nine white pawns — rejected by FENParser's "too many pawns" rule.
	constexpr const char* kRejectedFen = "4k3/8/P7/8/8/8/PPPPPPPP/4K3 w - - 0 1";
	constexpr const char* kKiwipeteFen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

} // namespace

TEST_CASE("cmd_position: a rejected FEN gives the same board whatever preceded it", "[uci]")
{
	auto perft_after = [](const std::string& prior) {
		UciHandlerTestFixture fix;
		fix.position(prior);
		capture_cout([&] { fix.position(std::string("position fen ") + kRejectedFen); });
		return divide_total(capture_cout([&] { fix.perft("perft 1"); }));
	};

	const auto after_startpos = perft_after("position startpos");
	const auto after_kiwipete = perft_after(std::string("position fen ") + kKiwipeteFen);

	// Before #200 these were 20 and 48: the engine reported on the stale board.
	REQUIRE(after_startpos == after_kiwipete);
	REQUIRE(after_startpos == 20);
}

TEST_CASE("cmd_position: an unparseable move rejects the entire replay and reports it", "[uci]")
{
	UciHandlerTestFixture fix;

	const std::string output = capture_cout([&] { fix.position("position startpos moves e2e4 zzzz e7e5"); });

	REQUIRE(output.find("info string") != std::string::npos);
	REQUIRE(output.find("zzzz") != std::string::npos);

	// The valid prefix is not committed when a later token invalidates the replay.
	REQUIRE(divide_total(capture_cout([&] { fix.perft("perft 1"); })) == 20);
	REQUIRE(fix.board().GetCurrentColor() == WHITE);
	REQUIRE(fix.board().GetPiece(e2) == WHITE_PAWN);
	REQUIRE(fix.board().GetPiece(e4) == NO_PIECE);
}

// ---------------------------------------------------------------------------
// Commands that mutate state a running search reads are refused (issue #178)
// ---------------------------------------------------------------------------

TEST_CASE("cmd_position: refused while a search is running, board untouched", "[uci]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	fix.set_searching(true);
	const std::string output = capture_cout([&] { fix.position("position fen 4k3/8/8/8/8/8/8/4K2R w K - 0 1"); });
	fix.set_searching(false);

	REQUIRE(output.find("info string") != std::string::npos);
	REQUIRE(output.find("position") != std::string::npos);

	// The refusal must be a refusal: a partially applied position would be
	// worse than either honouring or rejecting the command outright.
	REQUIRE(divide_total(capture_cout([&] { fix.perft("perft 1"); })) == 20);
	REQUIRE(fix.board().GetCurrentColor() == WHITE);
}

TEST_CASE("cmd_setoption: refused while a search is running", "[uci][smp]")
{
	UciHandlerTestFixture fix;
	fix.ucinewgame(); // construct the initial ai_
	fix.setoption("setoption name Threads value 2");
	REQUIRE(fix.ai_threads() == 2);

	fix.set_searching(true);
	const std::string output = capture_cout([&] { fix.setoption("setoption name Threads value 8"); });
	fix.set_searching(false);

	REQUIRE(output.find("info string") != std::string::npos);

	// SetThreads on a live AI is the dangerous half of the pair, so the guard
	// must run before any state changes -- including the bookkeeping copy.
	REQUIRE(fix.ai_threads() == 2);
	REQUIRE(fix.configured_threads() == 2);
}

TEST_CASE("Both commands work normally once the search is over", "[uci]")
{
	// The guard must key off an explicit flag, not search_thread_.joinable():
	// a std::thread stays joinable after its function returns, so a
	// joinable()-based guard would refuse the 'position' of every normal
	// go -> bestmove -> position cycle.
	UciHandlerTestFixture fix;
	fix.ucinewgame(); // construct the initial ai_

	fix.set_searching(true);
	capture_cout([&] { fix.position("position startpos moves e2e4"); });
	fix.set_searching(false);

	capture_cout([&] { fix.position("position startpos moves e2e4"); });
	REQUIRE(fix.board().GetCurrentColor() == BLACK);
	REQUIRE(divide_total(capture_cout([&] { fix.perft("perft 1"); })) == 20);

	fix.setoption("setoption name Threads value 3");
	REQUIRE(fix.ai_threads() == 3);
}

// ---------------------------------------------------------------------------
// dispatch() and the received-command log (issue #269)
// ---------------------------------------------------------------------------

// A path in the system temp directory, unique per test, so two tests never
// share a log file -- which is the property CreateUciCommandLogger's
// unregistered, handler-owned logger exists to provide.
static std::filesystem::path temp_log_path(const std::string& stem)
{
	return std::filesystem::temp_directory_path() /
	       ("strat_uci_log_" + stem + "_" + std::to_string(std::random_device{}()) + ".log");
}

static std::string read_file(const std::filesystem::path& path)
{
	std::ifstream in(path);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST_CASE("dispatch: returns false for quit and true for everything else", "[uci]")
{
	UciHandlerTestFixture fix;

	REQUIRE(capture_cout([&] { REQUIRE(fix.dispatch("isready")); }) == "readyok\n");
	REQUIRE(fix.dispatch("not a uci command"));
	REQUIRE(fix.dispatch("position startpos"));
	REQUIRE_FALSE(fix.dispatch("quit"));
}

TEST_CASE("dispatch: 'go' before any ucinewgame constructs the AI instead of crashing", "[uci]")
{
	// Unreachable through run(), which calls init_ai() before reading a command -- but reachable
	// through dispatch(), and what used to happen was an access violation on the search thread
	// that took the whole test binary down with no failing assertion to point at it.
	//
	// 'stop' inside the captured scope is what makes this deterministic: it joins the search
	// thread, so everything the thread prints has been printed before the capture ends.
	UciHandlerTestFixture fix;

	// A default-constructed Board is EMPTY, and cmd_position is what fills it -- searching without
	// this trips assert(mask != 0) in Board::GetFirstPiece, which aborts a Debug build and is
	// invisible in Release. That is a different defect from the one under test here (#279).
	//
	// cmd_position does not construct ai_, so the guard is still what this exercises; the
	// assertion below is what keeps that true if init_ai() ever moves.
	capture_cout([&] { fix.dispatch("position startpos"); });
	REQUIRE(fix.ai_identity() == nullptr);

	const std::string out = capture_cout([&] {
		fix.dispatch("go depth 1");
		fix.dispatch("stop");
	});

	REQUIRE(out.find("bestmove") != std::string::npos);
}

TEST_CASE("command log: nothing is written unless it is enabled", "[uci]")
{
	// The default a match run gets. There is no path to check for absence here
	// by design -- with no logger there is no filename either, so the assertion
	// is that dispatching is inert.
	UciHandlerTestFixture fix;
	const auto before = std::filesystem::current_path() / "logs";
	const bool logs_existed = std::filesystem::exists(before);

	capture_cout([&] { fix.dispatch("isready"); });

	if (!logs_existed) {
		REQUIRE_FALSE(std::filesystem::exists(before));
	}
}

TEST_CASE("command log: records every received command, including ignored ones", "[uci]")
{
	const auto path = temp_log_path("received");

	{
		UciHandlerTestFixture fix;
		REQUIRE(fix.handler.EnableCommandLog(path.string()));

		capture_cout([&] {
			fix.dispatch("isready");
			// Silently ignored by the command loop, and still logged: "the GUI
			// sent something the engine did not act on" is exactly the question
			// this answers.
			fix.dispatch("ponderhit");
			fix.dispatch("position startpos moves e2e4");
		});
	} // handler destroyed -> sink released

	const std::string contents = read_file(path);
	REQUIRE(contents.find(">> isready") != std::string::npos);
	REQUIRE(contents.find(">> ponderhit") != std::string::npos);
	REQUIRE(contents.find(">> position startpos moves e2e4") != std::string::npos);

	// Released with the handler, so the file can be removed while the process
	// lives on. A registered logger under a fixed name would still hold it.
	REQUIRE(std::filesystem::remove(path));
}

TEST_CASE("command log: two handlers log to their own files", "[uci]")
{
	// The regression that a spdlog-registry logger would cause: the second
	// handler would silently inherit the first one's file, and this test would
	// find the second command in the first file.
	const auto first_path = temp_log_path("first");
	const auto second_path = temp_log_path("second");

	{
		UciHandlerTestFixture first;
		REQUIRE(first.handler.EnableCommandLog(first_path.string()));
		capture_cout([&] { first.dispatch("isready"); });

		UciHandlerTestFixture second;
		REQUIRE(second.handler.EnableCommandLog(second_path.string()));
		capture_cout([&] { second.dispatch("ucinewgame"); });
	}

	const std::string first_contents = read_file(first_path);
	const std::string second_contents = read_file(second_path);

	REQUIRE(first_contents.find(">> isready") != std::string::npos);
	REQUIRE(first_contents.find(">> ucinewgame") == std::string::npos);
	REQUIRE(second_contents.find(">> ucinewgame") != std::string::npos);
	REQUIRE(second_contents.find(">> isready") == std::string::npos);

	REQUIRE(std::filesystem::remove(first_path));
	REQUIRE(std::filesystem::remove(second_path));
}

TEST_CASE("command log: an unopenable path is reported, not silently ignored", "[uci]")
{
	// The parent has to be impossible to create on EVERY platform, which "a path that does not
	// exist" is not: spdlog's file_helper::open creates missing directories, so an absent path is
	// opened rather than refused. A drive letter is no help either — 'Z:/...' is an absent drive
	// on Windows but an ordinary relative directory name on Linux, which is how the first version
	// of this test passed locally and failed all three Linux jobs.
	//
	// A regular FILE used as a directory component cannot be created through anywhere.
	const auto blocker = temp_log_path("blocker");
	{
		std::ofstream create(blocker);
		create << "not a directory\n";
	}
	REQUIRE(std::filesystem::is_regular_file(blocker));

	UciHandlerTestFixture fix;
	REQUIRE_FALSE(fix.handler.EnableCommandLog((blocker / "uci.log").string()));

	REQUIRE(std::filesystem::remove(blocker));
}

TEST_CASE("DefaultCommandLogPath: carries the process id", "[uci]")
{
	// Six engines share one working directory at -Concurrency 6, and the file
	// sink is not process-safe.
	const std::string path = UciHandler::DefaultCommandLogPath();
	REQUIRE(path.starts_with("logs/uci_commands_"));
	REQUIRE(path.ends_with(".log"));

	const auto digits_begin = path.find_last_of('_') + 1;
	const std::string pid = path.substr(digits_begin, path.size() - digits_begin - 4);
	REQUIRE_FALSE(pid.empty());
	REQUIRE(std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; }));
}
