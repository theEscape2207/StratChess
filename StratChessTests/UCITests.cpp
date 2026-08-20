// UCITests.cpp — Catch2 [uci] tests for UciHandler::parse_go() and
// cmd_position() (via UciHandlerTestFixture, STRAT_ENABLE_TEST_ACCESS).

#include <catch_amalgamated.hpp>
#include "UCIHandler.h"
#include "AIPerplex.h"
#include "Board.h"
#include "MoveFactory.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"
#include "Eval.h"
#include "TranspositionTable.h"
#include "Utils/FenBatch.h"
#include <sstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>
#include <regex>
#include <optional>
#include <utility>
#include <filesystem>
#include <fstream>
#include <random>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <spdlog/sinks/base_sink.h>

using P = UciHandler::GoParams;

// Must be defined here — the name must match the friend declaration inside
// UCIHandler.h: friend class UciHandlerTestFixture;
class UciHandlerTestFixture {
  public:
	UciHandler handler;

	void position(const std::string& line) { handler.cmd_position(line); }
	const Board& board() const { return handler.board_; }

	bool dispatch(const std::string& line) { return handler.dispatch(line); }

	void perft(const std::string& line) { handler.cmd_perft(line); }
	void setoption(const std::string& line) { handler.cmd_setoption(line); }
	void ucinewgame() { handler.cmd_ucinewgame(); }
	void uci() { handler.cmd_uci(); }
	void eval() { handler.cmd_eval(); }

	// Drives the mid-search guard without starting a real search: spawning one
	// and racing it would make these cases timing-dependent, and what is under
	// test is the guard's contract, not the scheduler. That the flag is
	// genuinely set for a real search is covered end-to-end by piping the
	// issue #178 reproduction through the built exe.
	void set_searching(bool value) { handler.searching_.store(value); }
	unsigned configured_threads() const { return handler.configured_threads_; }

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

	// Identity of the live ai_ instance, for proving cmd_ucinewgame() no
	// longer rebuilds it.
	const void* ai_identity() const { return handler.ai_.get(); }

	static constexpr uint64_t TT_MARKER_KEY = 0x7fff'ffff'ffff'fffeULL;

	void store_tt_marker() const
	{
		auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
		REQUIRE(perplex != nullptr);
		perplex->_tt->store(TT_MARKER_KEY, 123, 1, 0, Move::EmptyMove(), BoundType::EXACT, NodeType::PV_NODE,
		                    SearchPhase::MAIN);
	}

	bool has_tt_marker() const
	{
		auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
		REQUIRE(perplex != nullptr);
		return perplex->_tt->probe(TT_MARKER_KEY, 0).has_value();
	}

	size_t ai_hash_requested_mb() const
	{
		auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
		REQUIRE(perplex != nullptr);
		return perplex->_tt->requested_memory_mb();
	}

	size_t ai_hash_memory_mb() const
	{
		auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
		REQUIRE(perplex != nullptr);
		return perplex->_tt->memory_mb();
	}

	size_t ai_hash_bucket_count() const
	{
		auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
		REQUIRE(perplex != nullptr);
		return perplex->_tt->bucket_count();
	}

	const void* tt_identity() const
	{
		auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
		REQUIRE(perplex != nullptr);
		return perplex->_tt.get();
	}

	// cmd_go() runs the search on handler.search_thread_ and returns immediately;
	// dispatch("go ...") in a test therefore needs an explicit synchronous wait for
	// the thread to finish (and flush its output) before the captured cout buffer
	// can be inspected. Direct join rather than handler.stop_and_join(): the tests
	// using this drive a fixed-depth search that is expected to finish on its own,
	// so there is nothing to signal -- only completion to wait for.
	void join_search()
	{
		if (handler.search_thread_.joinable())
			handler.search_thread_.join();
	}

	// Calls AIPerplex::GetMove() directly, the way game mode (non-UCI) does --
	// bypassing cmd_go entirely, so no iteration observer is ever registered.
	// Used to pin that an unregistered observer means emit_iteration_info() does
	// no work and prints nothing (issue #237 stage 0, item 3).
	Move run_search_directly(int depth)
	{
		auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
		REQUIRE(perplex != nullptr);
		return perplex->GetMove(SearchLimits::fixed_depth(depth)).best_move;
	}

	// Nodes from the last search. A non-null bestmove alone does not prove a search ran:
	// the empty-move emergency path also returns one, having searched nothing.
	int64_t last_main_nodes() const
	{
		auto* perplex = dynamic_cast<AIPerplex*>(handler.ai_.get());
		REQUIRE(perplex != nullptr);
		return perplex->GetLastResult().nodes_searched;
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
			moves += pawn_moves[pawn_i];
			moves += ' ';
			moves += pawn_moves[pawn_i + 1];
			moves += ' ';
			pawn_i += 2;
			plies += 2;
		}
		moves += "g1f3 g8f6 f3g1 f6g8 ";
		plies += 4;
	}
	moves.pop_back(); // trailing space
	return moves;
}

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

// ---------------------------------------------------------------------------
// cmd_eval — static evaluation introspection (issue #129 phase 1)
// ---------------------------------------------------------------------------

// Redirects std::cout into an in-memory buffer for the lifetime of the
// object; restores the original streambuf on destruction (including when
// unwinding past a failed REQUIRE), so a single assertion failure can never
// leave std::cout silently rewired for the rest of the test binary.
class CoutRedirect {
  public:
	CoutRedirect() : old_(std::cout.rdbuf(buffer_.rdbuf())) {}
	~CoutRedirect()
	{
		try {
			std::cout.rdbuf(old_);
		} catch (...) { // NOLINT(bugprone-empty-catch) - restoring cout in a destructor
		}
	}

	CoutRedirect(const CoutRedirect&) = delete;
	CoutRedirect& operator=(const CoutRedirect&) = delete;

	std::string str() const { return buffer_.str(); }

  private:
	std::ostringstream buffer_;
	std::streambuf* old_;
};

// Runs `action` with std::cout captured and returns what it printed.
//
// A named helper rather than a brace scope around a CoutRedirect: the capture
// covers exactly one call, and the result is an expression rather than an
// out-of-scope variable assigned inside braces. Tests that capture a whole
// function body keep using CoutRedirect directly, which is equally fine.
template <typename F> static std::string capture_cout(F&& action)
{
	CoutRedirect redirect;
	std::forward<F>(action)();
	return redirect.str();
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
	const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"; // Kiwipete

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

	SECTION("White to move — white pov equals the side-to-move score")
	{
		fix.position("position fen 4k3/8/8/8/8/8/8/R3K3 w - - 0 1"); // White up a rook

		CoutRedirect redirect;
		fix.eval();
		const std::string out = redirect.str();

		const int side_to_move_score = extract_cp_score(out, "static eval:");
		const int white_pov = extract_cp_score(out, "white pov:");

		REQUIRE(side_to_move_score != 0);
		REQUIRE(white_pov == side_to_move_score);
	}

	SECTION("Black to move — white pov is the negation of the side-to-move score")
	{
		fix.position("position fen r3k3/8/8/8/8/8/8/4K3 b - - 0 1"); // Black up a rook

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

struct EvalTermRow {
	int white;
	int black;
	int net;
};

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
	const std::string value = (line_end != std::string::npos) ? output.substr(value_start, line_end - value_start)
	                                                          : output.substr(value_start);

	return std::stoi(value);
}

// Every term the breakdown prints, in table order. Material is a row like any
// other: it is the largest single contribution and a sign error there would be
// the easiest one to miss.
// Every row the breakdown prints. The list must stay complete: the sum check
// below compares these nets against the printed total, so a missing row makes
// the invariant vacuous rather than failing loudly -- it passed for a while
// with `bishops` and `castling` absent only because both were zero in the
// positions tested here.
static const char* const kBreakdownTerms[] = {"material", "pawns",   "rooks",    "pst",
                                              "mopup",    "bishops", "castling", "mobility"};

TEST_CASE("cmd_eval: printed breakdown nets are white-minus-black and sum to the evaluator's score", "[uci]")
{
	// The phase 2 honesty invariant (D9). Three separate claims, each of which
	// has failed in some engine at some point: each row's net is consistent
	// with its own two columns; the net column sums to the printed total; and
	// that total is the score the search would actually see, computed here
	// from a fresh Board through a separate EvalManager instance.
	const char* fen =
	    GENERATE("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", // Kiwipete, middlegame
	             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",             // startpos, symmetric
	             "8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1",                                     // endgame, White to move
	             "r3k3/8/8/8/8/8/8/4K3 b - - 0 1",  // Black to move, Black up a rook
	             "8/8/8/4k3/8/8/8/3QK3 w - - 0 1"); // pawnless K+Q vs K — mop-up active
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
	const int expected_white_pov = (board.GetCurrentColor() == WHITE) ? side_to_move_score : -side_to_move_score;

	REQUIRE(net_sum == expected_white_pov);
}

TEST_CASE("cmd_eval: breakdown reports the game phase the evaluator computed", "[uci]")
{
	// Phase is printed because it is not derivable from the rows, yet it sets
	// where between the mg and eg endpoints every tapered term landed — so a
	// reader debugging the pst row needs it to interpret that row at all
	// (issue #99, replacing the old middlegame/endgame stage name).
	UciHandlerTestFixture fix;

	SECTION("full starting material is the maximum phase")
	{
		fix.position("position startpos");

		CoutRedirect redirect;
		fix.eval();
		REQUIRE(redirect.str().find("phase: 24/24") != std::string::npos);
	}

	SECTION("bare kings plus a queen is deep in the endgame")
	{
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

namespace {
	// Throws on every log call to verify metadata correction does not depend on logging.
	class ThrowingSink final : public spdlog::sinks::base_sink<std::mutex> {
	  protected:
		void sink_it_(const spdlog::details::log_msg&) override { throw std::runtime_error("sink failure (test)"); }
		void flush_() override {}
	};

	// Restores the default logger's sink list on every scope exit.
	class ScopedThrowingSink {
	  public:
		ScopedThrowingSink() : sink_(std::make_shared<ThrowingSink>())
		{
			spdlog::default_logger()->sinks().push_back(sink_);
		}
		~ScopedThrowingSink()
		{
			try {
				auto& sinks = spdlog::default_logger()->sinks();
				sinks.erase(std::remove(sinks.begin(), sinks.end(), sink_), sinks.end());
			} catch (...) { // NOLINT(bugprone-empty-catch) - cleanup in a destructor
			}
		}
		ScopedThrowingSink(const ScopedThrowingSink&) = delete;
		ScopedThrowingSink& operator=(const ScopedThrowingSink&) = delete;

	  private:
		std::shared_ptr<ThrowingSink> sink_;
	};
} // namespace

TEST_CASE("FENParser::ValidatePositionAgainstFENMetadata: every correction still applies when "
          "logging throws",
          "[uci]")
{
	// Two independent corrections ensure a logging failure cannot stop validation early.
	const std::string fen = "4k3/8/8/8/4P3/8/8/3K4 w Q e6 0 1";

	Board board;
	REQUIRE(board.SetupFromFEN(fen));

	FENParser::FENGameState state;
	std::vector<std::tuple<ePiece, eSquare>> pieces;
	REQUIRE_FALSE(FENParser::ParseFEN(fen, state, pieces));
	REQUIRE(state.castlingRights == CastlingRights::WHITE_QUEENSIDE);
	REQUIRE(state.epSquare == e6);

	bool ok = false;
	{
		ScopedThrowingSink throwing;
		REQUIRE_NOTHROW(ok = FENParser::ValidatePositionAgainstFENMetadata(board, state));
	}

	CHECK(ok);
	CHECK(state.castlingRights == CastlingRights::NONE);
	CHECK(state.epSquare == NO_SQUARE);
}

TEST_CASE("Board::SetupFromFEN: 4-field FEN yields halfmoveClock 0", "[uci]")
{
	// The halfmove default is not inert: SetupFromFEN feeds it into gameInfo_.halfmoveClock,
	// which drives 50-move draw detection. Pin the value rather than leave it implicit.
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -"));

	// The kings are checked as well as the return value: an empty board's halfmoveClock is also 0,
	// so without them a position that never landed would pass this test for the wrong reason.
	REQUIRE(board.GetPiece(e1) == WHITE_KING);
	REQUIRE(board.GetPiece(e8) == BLACK_KING);
	CHECK(board.halfmove_clock() == 0);
}

// ---------------------------------------------------------------------------
// Board::SetupFromFEN — halfmove/fullmove counter bounds (GameState.h's
// MAX_FEN_HALFMOVE_CLOCK / MAX_FEN_FULLMOVE_COUNT). A value past these
// describes a game that cannot be played, so it is rejected, not repaired.
// ---------------------------------------------------------------------------

TEST_CASE("Board::SetupFromFEN: halfmove clock past MAX_FEN_HALFMOVE_CLOCK is rejected", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	REQUIRE_FALSE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 151 1"));

	// Untouched means the previous position, not a reset and not an empty board.
	CHECK(board.GetPiece(e1) == WHITE_KING);
	CHECK(board.halfmove_clock() == 0);
}

TEST_CASE("Board::SetupFromFEN: halfmove clock at MAX_FEN_HALFMOVE_CLOCK loads", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 150 1"));

	std::istringstream fss(board.ExtractFEN());
	std::vector<std::string> fenParts{std::istream_iterator<std::string>(fss), std::istream_iterator<std::string>()};
	REQUIRE(fenParts.size() == 6);
	CHECK(fenParts[4] == "150");
}

TEST_CASE("Board::SetupFromFEN: fullmove counter past MAX_FEN_FULLMOVE_COUNT is rejected", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	REQUIRE_FALSE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 5900"));

	CHECK(board.GetPiece(e1) == WHITE_KING);
	CHECK(board.fullmove_count() == 1);
}

TEST_CASE("Board::SetupFromFEN: fullmove counter at MAX_FEN_FULLMOVE_COUNT loads", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 5899"));

	std::istringstream fss(board.ExtractFEN());
	std::vector<std::string> fenParts{std::istream_iterator<std::string>(fss), std::istream_iterator<std::string>()};
	REQUIRE(fenParts.size() == 6);
	CHECK(fenParts[5] == "5899");
}

// Pins the deliberate repair: a FEN fullmove number is 1-based, so 0 is not out-of-range input,
// it is a hand-authored position that meant "move one" -- std::max(1, full) fixes it up rather
// than rejecting it.
TEST_CASE("Board::SetupFromFEN: fullmove counter 0 is repaired to 1", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 0"));

	std::istringstream fss(board.ExtractFEN());
	std::vector<std::string> fenParts{std::istream_iterator<std::string>(fss), std::istream_iterator<std::string>()};
	REQUIRE(fenParts.size() == 6);
	CHECK(fenParts[5] == "1");
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

	const Move best = fx.run_search_directly(4);

	CHECK_FALSE(best.is_null());
	CHECK(fx.last_main_nodes() > 0);
}

// ---------------------------------------------------------------------------
// Board::SetupFromFEN failure reporting, and cmd_position's response to it
// (issues #155, #46)
// ---------------------------------------------------------------------------

TEST_CASE("Board::SetupFromFEN: reports failure and leaves the board untouched", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	// Missing the side-to-move, castling and en-passant fields: rejected by the field-count floor.
	REQUIRE_FALSE(board.SetupFromFEN("6k1/5ppp/8/8/8/8/5PPP/R5K1"));

	// Untouched means the previous position, not a reset and not an empty board.
	CHECK(board.GetPiece(e1) == WHITE_KING);
	CHECK(board.GetPiece(d8) == BLACK_QUEEN);
	CHECK(board.GetCurrentColor() == WHITE);
}

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

// ---------------------------------------------------------------------------
// Piece placement sanity: exactly one king per color (issue #163)
//
// A board missing a king, or holding two of one color, is what let a generated
// king-capturing move reach Board::DoMove and read one entry past
// g_bbKingMoves' 64-entry table before #45 closed the only known route in. The
// initial-position invariant was already enforced by FENParser::ParsePiecePlacementField.
// UCI replay separately resolves every client token against generated moves before DoMove,
// so malformed protocol input cannot remove a king after setup either.
// ---------------------------------------------------------------------------

TEST_CASE("Board::SetupFromFEN: rejects a FEN with no black king", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	// Black king square (e8) left empty; every other black piece unchanged.
	REQUIRE_FALSE(board.SetupFromFEN("rnbq1bnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	CHECK(board.GetPiece(e8) == BLACK_KING);
}

TEST_CASE("Board::SetupFromFEN: rejects a FEN with two white kings", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	// White queen (d1) replaced by a second white king.
	REQUIRE_FALSE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBKKBNR w KQkq - 0 1"));

	CHECK(board.GetPiece(d1) == WHITE_QUEEN);
}

// Board(fen) documents an assert-in-Debug / empty-board-in-Release contract for a malformed
// FEN (Board.h: "every caller passes a literal, so a malformed one is a bug in the caller") --
// it is not meant to validate untrusted input, unlike SetupFromFEN. Gated to Release only:
// under Debug the constructor's own assert would abort the process, which is correct behavior
// for a caller bug but not something a REQUIRE-based test can observe without crashing the
// Debug/sanitizer CI leg along with it.
#ifdef NDEBUG
TEST_CASE("Board(fen): a missing king leaves the board empty, matching SetupFromFEN's contract", "[uci]")
{
	Board board("rnbq1bnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

	CHECK(board.GetPiece(e1) == NO_PIECE);
	CHECK(board.GetPiece(a1) == NO_PIECE);
}
#endif

// ---------------------------------------------------------------------------
// FEN metadata repair: clear and report, don't reject the whole position
// (issue #221)
// ---------------------------------------------------------------------------

TEST_CASE("Board::SetupFromFEN: repairs overload replaces previous diagnostics", "[uci]")
{
	Board board;
	std::vector<std::string> repairs;

	REQUIRE(board.SetupFromFEN("4k3/8/8/8/4P3/8/8/3K4 w Q e6 0 1", repairs));
	REQUIRE_FALSE(repairs.empty());

	REQUIRE(board.SetupFromFEN("4k3/8/8/8/8/8/8/4K3 w - - 0 1", repairs));
	CHECK(repairs.empty());
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

// ---------------------------------------------------------------------------
// Position legality: the side NOT to move may not be in check (issue #45)
// ---------------------------------------------------------------------------

// The issue's exact repro. White's rook on e1 attacks the black king on e8 with an empty file
// between them, so Black — the waiting side — is in check. Searching such a position used to reach
// a board with one king removed, where attack generation reads past its move table.
TEST_CASE("Board::SetupFromFEN: rejects a position with the waiting side in check", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));

	REQUIRE_FALSE(board.SetupFromFEN("4k3/8/8/8/8/5b2/8/4RK2 w - - 0 1"));

	// Rejected means nothing was applied, illegality included.
	CHECK(board.GetPiece(d8) == BLACK_QUEEN);
	CHECK(board.GetCurrentColor() == WHITE);
}

// The control for the test above: the same position with the other side to move is perfectly legal
// — Black is in check and must answer it. If the rule tested the wrong king this would fail.
TEST_CASE("Board::SetupFromFEN: accepts the same position with the checked side to move", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("4k3/8/8/8/8/5b2/8/4RK2 b - - 0 1"));

	CHECK(board.GetCurrentColor() == BLACK);
	CHECK(board.InCheck());
	CHECK_FALSE(board.WaitingSideInCheck());
}

// Adjacent kings need no rule of their own: each king attacks the other, so the waiting king is
// attacked and the same test rejects the position.
TEST_CASE("Board::SetupFromFEN: rejects adjacent kings", "[uci]")
{
	Board board;
	REQUIRE_FALSE(board.SetupFromFEN("8/8/8/3kK3/8/8/8/8 w - - 0 1"));
	REQUIRE_FALSE(board.SetupFromFEN("8/8/8/3kK3/8/8/8/8 b - - 0 1"));
}

// Guards against over-rejection: an ordinary position where the side to move is in check has to keep
// loading, since that is what most tactical test positions are.
TEST_CASE("Board::SetupFromFEN: a normal check position still loads", "[uci]")
{
	Board board;
	REQUIRE(board.SetupFromFEN("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3"));

	CHECK(board.InCheck());
	CHECK_FALSE(board.WaitingSideInCheck());
}

TEST_CASE("cmd_position: an illegal position is declined", "[uci]")
{
	UciHandlerTestFixture fx;
	fx.position("position startpos");

	const std::string before = fx.board().ExtractFEN();

	capture_cout([&] { fx.position("position fen 4k3/8/8/8/8/5b2/8/4RK2 w - - 0 1"); });

	CHECK(fx.board().ExtractFEN() == before);
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

// ---------------------------------------------------------------------------
// cmd_perft — "perft <depth>" / "go perft <depth>"
//
// The divide lines are a wire format, not diagnostics: external harnesses parse
// them with ^\s*([a-h][1-8][a-h][1-8][rnbqRNBQ]?)\s*[:\s]\s*(\d+)$ (#196), so
// these tests assert against that regex rather than against a substring.
// ---------------------------------------------------------------------------

namespace {

	const std::regex kDivideLine{R"(^\s*([a-h][1-8][a-h][1-8][rnbqRNBQ]?)\s*[:\s]\s*(\d+)$)"};

	// Every (move, nodes) pair the harness regex accepts out of `output`.
	std::vector<std::pair<std::string, uint64_t>> parse_divide(const std::string& output)
	{
		std::vector<std::pair<std::string, uint64_t>> out;
		std::istringstream iss{output};
		std::string line;
		while (std::getline(iss, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			std::smatch m;
			if (std::regex_match(line, m, kDivideLine)) {
				out.emplace_back(m[1].str(), std::stoull(m[2].str()));
			}
		}
		return out;
	}

	uint64_t divide_total(const std::string& output)
	{
		uint64_t sum = 0;
		for (const auto& entry : parse_divide(output))
			sum += entry.second;
		return sum;
	}

} // namespace

TEST_CASE("cmd_perft: startpos depth 1 emits 20 harness-parseable divide lines", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.perft("perft 1");
		output = redirect.str();
	}

	const auto divides = parse_divide(output);
	REQUIRE(divides.size() == 20);
	for (const auto& entry : divides) {
		REQUIRE(entry.first.size() == 4);
		REQUIRE(entry.second == 1);
	}
	REQUIRE(divide_total(output) == 20);
}

TEST_CASE("cmd_perft: 'go perft' is not parsed as a search", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.perft("go perft 2");
		output = redirect.str();
	}

	REQUIRE(divide_total(output) == 400);
	REQUIRE(output.find("bestmove") == std::string::npos);
}

TEST_CASE("cmd_perft: honours the position set by cmd_position", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	std::string output;
	{
		CoutRedirect redirect;
		fix.perft("perft 3");
		output = redirect.str();
	}

	REQUIRE(parse_divide(output).size() == 48);
	REQUIRE(divide_total(output) == 97862);
}

TEST_CASE("cmd_perft: malformed depth is ignored, not guessed at", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	for (const auto* line : {"perft", "perft abc", "perft -1", "perft 11", "go perft"}) {
		std::string output;
		{
			CoutRedirect redirect;
			fix.perft(line);
			output = redirect.str();
		}
		INFO("input: " << line);
		REQUIRE(parse_divide(output).empty());
	}
}

TEST_CASE("cmd_perft: leaves the board unchanged", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	{
		CoutRedirect redirect;
		fix.perft("perft 3");
	}

	REQUIRE(fix.board().GetCurrentColor() == WHITE);

	std::string output;
	{
		CoutRedirect redirect;
		fix.perft("perft 1");
		output = redirect.str();
	}
	REQUIRE(divide_total(output) == 20);
}

// run() is where "go perft" could be swallowed by the "go" branch, and the
// tests above bypass it by calling cmd_perft directly. This one drives the real
// command loop over stdin, which is what an external harness does.
class CinRedirect {
  public:
	explicit CinRedirect(std::string input) : buffer_(std::move(input)), old_(std::cin.rdbuf(buffer_.rdbuf())) {}
	~CinRedirect()
	{
		try {
			std::cin.rdbuf(old_);
		} catch (...) { // NOLINT(bugprone-empty-catch) - restoring cin in a destructor
		}
	}

	CinRedirect(const CinRedirect&) = delete;
	CinRedirect& operator=(const CinRedirect&) = delete;

  private:
	std::istringstream buffer_;
	std::streambuf* old_;
};

TEST_CASE("run(): dispatches 'go perft' to perft, not to the search", "[uci][perft]")
{
	UciHandler handler;

	std::string output;
	{
		CinRedirect input("position startpos\ngo perft 2\nquit\n");
		CoutRedirect redirect;
		handler.run();
		output = redirect.str();
	}

	REQUIRE(divide_total(output) == 400);
	REQUIRE(output.find("bestmove") == std::string::npos);
}

TEST_CASE("run(): a bare 'go' still searches after the perft branch was added", "[uci][perft]")
{
	UciHandler handler;

	std::string output;
	{
		CinRedirect input("position startpos\ngo depth 3\nquit\n");
		CoutRedirect redirect;
		handler.run();
		output = redirect.str();
	}

	REQUIRE(output.find("bestmove") != std::string::npos);
	REQUIRE(parse_divide(output).empty());
}

// ---------------------------------------------------------------------------
// A rejected FEN must not leave the previous position on the board (#200).
//
// The load-bearing property is that the answer does not depend on what was
// loaded before: the same rejected FEN from two different prior positions must
// leave the same board.
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

// ---------------------------------------------------------------------------
// cmd_go — per-iteration 'info' reporting (issue #237 stage 0)
// ---------------------------------------------------------------------------

namespace {

	// One parsed "info depth ..." line. `score` keeps the two-token form
	// ("cp 34" / "mate 3") verbatim so tests can check the kind without
	// re-deriving GameValues::Mate_Threshold.
	struct ParsedInfoLine {
		int depth = 0;
		std::string score;
		int64_t nodes = 0;
		int time_ms = 0;
		std::vector<std::string> pv;
	};

	// Every "info depth ..." line in `output`, in emission order. Deliberately
	// tolerant of any other 'info' line shape (e.g. 'info string ...') by only
	// matching the "info depth" prefix — those are unrelated protocol output,
	// not a parse failure.
	std::vector<ParsedInfoLine> parse_info_depth_lines(const std::string& output)
	{
		std::vector<ParsedInfoLine> result;
		for (const std::string& line : split_lines(output)) {
			if (!line.starts_with("info depth "))
				continue;

			std::istringstream iss(line);
			std::string tok;
			ParsedInfoLine info;
			while (iss >> tok) {
				if (tok == "depth") {
					iss >> info.depth;
				} else if (tok == "score") {
					std::string kind;
					std::string value;
					iss >> kind >> value;
					info.score = kind + " " + value;
				} else if (tok == "nodes") {
					iss >> info.nodes;
				} else if (tok == "time") {
					iss >> info.time_ms;
				} else if (tok == "pv") {
					std::string move;
					while (iss >> move)
						info.pv.push_back(move);
				}
			}
			result.push_back(std::move(info));
		}
		return result;
	}

	// The move token on a "bestmove <move>" line, or empty if none is present.
	std::string extract_bestmove(const std::string& output)
	{
		for (const std::string& line : split_lines(output)) {
			if (line.starts_with("bestmove ")) {
				return line.substr(std::string("bestmove ").size());
			}
		}
		return {};
	}

	// Replays a full PV (as UCI tokens) from `board`, resolving each token
	// against the position's own legal move list at that point -- mirrors
	// UciHandler's private find_replay_move() (UCIHandler.cpp), which is not
	// reachable from here. Returns false at the first token that is not a
	// legal move in the position it is played from.
	bool replay_pv_is_legal(Board board, const std::vector<std::string>& pv_tokens)
	{
		for (const std::string& token : pv_tokens) {
			MoveList moves;
			MoveGenerator::ComputeLegalMoves(board, moves);

			Move found;
			bool matched = false;
			for (const Move& candidate : moves) {
				if (MoveFormatter::ToUCI(candidate) == token) {
					found = candidate;
					matched = true;
					break;
				}
			}
			if (!matched || !board.DoMove(found))
				return false;
			board.ResetSearchDepth();
		}
		return true;
	}

} // namespace

TEST_CASE("cmd_go: 'go depth 4' emits per-iteration info lines with strictly increasing depth", "[uci]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	// At least the 4 per-iteration lines (depths 1-4) plus the unchanged final
	// summary line -- whose depth repeats the last iteration's, so only the
	// first 4 are asserted to be strictly increasing.
	REQUIRE(info_lines.size() >= 4);
	for (size_t i = 1; i < 4; ++i) {
		REQUIRE(info_lines[i].depth > info_lines[i - 1].depth);
	}
	REQUIRE(info_lines[0].depth == 1);

	int bestmove_lines = 0;
	for (const std::string& line : split_lines(output)) {
		if (line.starts_with("bestmove "))
			++bestmove_lines;
	}
	REQUIRE(bestmove_lines == 1);
}

TEST_CASE("cmd_go: the last info line's pv and score agree with bestmove", "[uci]")
{
	// This is the guarantee item 6 (issue #237 stage 0) exists for: the final
	// info line and 'bestmove' are built from the same shared score formatter
	// and the same SearchResult, so they cannot drift from each other.
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	const ParsedInfoLine& last = info_lines.back();
	REQUIRE_FALSE(last.pv.empty());

	const std::string bestmove = extract_bestmove(output);
	REQUIRE_FALSE(bestmove.empty());
	REQUIRE(last.pv.front() == bestmove);
}

TEST_CASE("cmd_go: 'go nodes N' stops on the node budget", "[uci]")
{
	// Driven past move 1 deliberately: from startpos the first search also pays a
	// cold transposition table, which is a different measurement.
	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go nodes 20000");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	// At or past the budget, because the limit is only observed at a poll boundary.
	REQUIRE(info_lines.back().nodes >= 20000);
	REQUIRE_FALSE(extract_bestmove(output).empty());
}

TEST_CASE("cmd_go: 'go nodes 1' still returns a move", "[uci]")
{
	// The tightest budget expressible. The abort fires at the first poll, so no
	// iteration can complete normally and the answer has to come out of the
	// interrupted-iteration handling rather than a finished search.
	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go nodes 1");
		fix.join_search();
		output = redirect.str();
	}

	REQUIRE_FALSE(extract_bestmove(output).empty());
}

TEST_CASE("cmd_go: a node-limited search never reports a spliced pv", "[uci]")
{
	// #310: fastchess rejected moves in our pv lines at about 0.24 per game. The cause was
	// an aborted frame going on to splice an earlier sibling's line onto its own move, so
	// from the ply where the two positions diverge the line describes a different board.
	//
	// The node limit is what makes that reproducible: at Threads=1 the abort lands on the
	// same node every run. 10'000 from this position is where it was reproduced before the
	// unwind guard — depth 5 reported "pv d7d5 e4d5 d5e4 ...", a black move from a square
	// that by then holds a white pawn. The neighbouring budgets are not free coverage of a
	// second defect; they are the same check at other abort points, which is where a future
	// regression would just as likely land.
	const int budget = GENERATE(5'000, 10'000, 20'000, 50'000);

	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go nodes " + std::to_string(budget));
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());

	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2"));

	for (const ParsedInfoLine& line : info_lines) {
		INFO("budget " << budget << ", depth " << line.depth);
		REQUIRE(replay_pv_is_legal(board, line.pv));
	}

	// The interrupted iteration is kept rather than discarded, so the move played has to be
	// the one its own partial line names.
	REQUIRE_FALSE(info_lines.back().pv.empty());
	REQUIRE(info_lines.back().pv.front() == extract_bestmove(output));
}

TEST_CASE("cmd_go: a node limit bounds a multi-threaded search too", "[uci][smp]")
{
	// Only thread 0 polls, so the budget bounds thread 0's node count and the reported
	// total — which sums every helper — lands near Threads times it. The property worth
	// pinning is that the search still stops and still answers: helpers exit on the same
	// latched flag, and their aborted frames unwind through the same guard.
	UciHandlerTestFixture fix;
	fix.setoption("setoption name Threads value 4");
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go nodes 20000");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	REQUIRE_FALSE(extract_bestmove(output).empty());

	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2"));
	REQUIRE(replay_pv_is_legal(board, info_lines.back().pv));
}

TEST_CASE("cmd_go: an explicit depth still caps a node-bounded search", "[uci]")
{
	// A node budget lifts the default depth cap so it cannot silently truncate a
	// large budget, but an explicit 'depth' outranks both. Without that ordering a
	// node-limited match could not also be depth-limited.
	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 5 nodes 100000000");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	for (const ParsedInfoLine& line : info_lines) {
		CHECK(line.depth <= 5);
	}
	REQUIRE_FALSE(extract_bestmove(output).empty());
}

TEST_CASE("cmd_go: at depth >= 3 the pv carries more than one move and replays legally", "[uci]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE(info_lines.size() >= 4);

	// info_lines[3] is the fourth emitted line, i.e. the depth-4 per-iteration
	// line (see the strictly-increasing-depth test above for why depths 1-4
	// occupy the first four slots).
	const ParsedInfoLine& depth4 = info_lines[3];
	REQUIRE(depth4.depth >= 3);
	REQUIRE(depth4.pv.size() > 1);

	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
	REQUIRE(replay_pv_is_legal(board, depth4.pv));
}

TEST_CASE("cmd_go: a forced mate reports 'mate N', not 'cp', in the score field", "[uci]")
{
	// Ra1-a8# -- verified elsewhere in this codebase's tactical suite
	// (TacticalTests.cpp: "M1: rook back rank", depth 4) as an actual mate-in-1
	// the search finds, so this FEN is not a fresh, unverified claim.
	UciHandlerTestFixture fix;
	fix.position("position fen 6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	REQUIRE(info_lines.back().score.starts_with("mate "));
	REQUIRE(extract_bestmove(output) == "a1a8");
}

TEST_CASE("AIPerplex::GetMove: emits no per-iteration output when no observer is registered", "[uci]")
{
	// Game mode and non-UCI callers never call SetIterationObserver -- this pins
	// that emit_iteration_info() is a true no-op without one, so the default
	// (non-UCI) search path is unaffected by this stage's addition.
	UciHandlerTestFixture fix;
	fix.ucinewgame();
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.run_search_directly(4);
		output = redirect.str();
	}

	REQUIRE(output.empty());
}

TEST_CASE("cmd_go: 'go movetime 300' final info line's nodes are >= the last per-iteration line's", "[uci]")
{
	// This is currently the only automated coverage of the ACCEPT_AND_STOP /
	// REJECT_AND_STOP paths through AIPerplex::iterative_deepening(): every
	// other 'go' test in this file is fixed-depth, which always finishes via
	// ACCEPT_AND_CONTINUE reaching max_depth rather than being interrupted.
	UciHandlerTestFixture fix;
	// Kiwipete: complex enough that 300ms will not reach UCI_DEFAULT_DEPTH (20).
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go movetime 300");
		fix.join_search();
		output = redirect.str();
	}

	const auto info_lines = parse_info_depth_lines(output);
	// At least one accepted per-iteration line plus the final summary line.
	REQUIRE(info_lines.size() >= 2);

	const ParsedInfoLine& last_iteration = info_lines[info_lines.size() - 2];
	const ParsedInfoLine& final_line = info_lines.back();

	REQUIRE(last_iteration.depth == final_line.depth);
	REQUIRE(last_iteration.score == final_line.score);
	// <=, not ==: a clocked search typically starts one more iteration that gets
	// interrupted and then rejected by assess_iteration_quality() -- REJECT_AND_STOP
	// emits no per-iteration line for it (iterative_deepening(), AIPerplex.cpp), but
	// that rejected iteration's nodes are already folded into td.nodes_searched by
	// the time GetMove() reports the final total (AIPerplex.h's IterationInfo doc).
	REQUIRE(last_iteration.nodes <= final_line.nodes);
}

TEST_CASE("cmd_go: 'stop' during 'go infinite' produces well-formed output under concurrent isready", "[uci]")
{
	// Exercises the send() mutex added when per-iteration output widened the
	// interleaving window between the search thread's info lines and the command
	// loop's own send() calls (UCIHandler.cpp send(), issue #237 stage 0 finding).
	// 'go infinite' returns as soon as the search thread is spawned, so the
	// isready calls below run on this (calling) thread genuinely concurrently
	// with the search thread's output -- Kiwipete at unbounded depth cannot
	// finish on its own before 'stop' arrives.
	//
	// The sleep before issuing isready/stop is not about search progress -- it
	// closes a separate, pre-existing race documented in TimeManager.h's own
	// threading contract: StopSearch() (callable from any thread) and
	// ApplyLimits()'s time_manager_.start() (called from inside GetMove(), on
	// the search thread, only after it is scheduled) are unsynchronized, so a
	// 'stop' arriving before the search thread reaches start() is silently
	// overwritten and the search becomes unstoppable for the rest of this
	// process. Fixing that race is outside this change's scope; the sleep keeps
	// this test about the send() mutex, not about that unrelated gap.
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	constexpr int kIsReadyCount = 20;
	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go infinite");
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		for (int i = 0; i < kIsReadyCount; ++i) {
			fix.dispatch("isready");
		}
		fix.dispatch("stop"); // joins the search thread; only returns once it has
		output = redirect.str();
	}

	const std::regex line_shape{R"(^(info|bestmove|readyok)\b)"};
	int readyok_count = 0;
	for (const std::string& line : split_lines(output)) {
		if (line.empty())
			continue;
		REQUIRE(std::regex_search(line, line_shape));
		if (line == "readyok")
			++readyok_count;
	}
	REQUIRE(readyok_count == kIsReadyCount);
}

TEST_CASE("cmd_go: two back-to-back 'go depth 4' searches each produce one bestmove and restart depths at 1", "[uci]")
{
	// Pins the observer register/clear lifecycle (UCIHandler.cpp cmd_go / search
	// thread lambda) against a future refactor: the second search's per-iteration
	// depths must restart at 1, not continue from wherever the first search's
	// observer left off.
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 4");
		fix.join_search();
		fix.dispatch("go depth 4");
		fix.join_search();
		output = redirect.str();
	}

	const auto lines = split_lines(output);
	int bestmove_count = 0;
	size_t first_bestmove_index = lines.size();
	for (size_t i = 0; i < lines.size(); ++i) {
		if (lines[i].starts_with("bestmove ")) {
			++bestmove_count;
			if (first_bestmove_index == lines.size())
				first_bestmove_index = i;
		}
	}
	REQUIRE(bestmove_count == 2);

	// Info lines strictly after the first bestmove belong to the second search.
	std::string tail;
	for (size_t i = first_bestmove_index + 1; i < lines.size(); ++i) {
		tail += lines[i];
		tail += '\n';
	}
	const auto second_search_info = parse_info_depth_lines(tail);
	REQUIRE_FALSE(second_search_info.empty());
	REQUIRE(second_search_info.front().depth == 1);
}

// ---------------------------------------------------------------------------
// Measurement contract over the wire (issue #312)
// ---------------------------------------------------------------------------
// Between the counters (covered in SearchTests) and the bench parser sits the
// protocol text itself, which nothing else asserts on.

namespace {

	// The 'main' and 'qs' operands of the last "info string treenodes ..." line,
	// or nullopt if the output carries none.
	std::optional<std::pair<int64_t, int64_t>> parse_treenodes(const std::string& output)
	{
		std::optional<std::pair<int64_t, int64_t>> found;
		for (const std::string& line : split_lines(output)) {
			if (!line.starts_with("info string treenodes "))
				continue;

			std::istringstream iss(line);
			std::string info;
			std::string string_tok;
			std::string treenodes;
			std::string main_tok;
			int64_t main_nodes = 0;
			std::string qs_tok;
			int64_t qs_nodes = 0;
			if (!(iss >> info >> string_tok >> treenodes >> main_tok >> main_nodes >> qs_tok >> qs_nodes))
				continue;
			if (main_tok != "main" || qs_tok != "qs")
				continue;
			found = std::make_pair(main_nodes, qs_nodes);
		}
		return found;
	}

} // namespace

TEST_CASE("cmd_uci: the handshake advertises a measurement contract version", "[uci][nodes]")
{
	// Run-Bench.ps1 reads absence as "pre-#312 build", so losing this line would not
	// error out — it would silently relabel every future run as an old one.
	UciHandlerTestFixture fix;
	const std::string output = capture_cout([&] { fix.uci(); });

	const std::regex contract_line(R"(^info string benchcontract [1-9]\d*$)");
	bool matched = false;
	for (const std::string& line : split_lines(output)) {
		if (std::regex_match(line, contract_line))
			matched = true;
	}
	REQUIRE(matched);
}

TEST_CASE("cmd_go: 'nodes' equals the reported main/quiescence split", "[uci][nodes]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	std::string output;
	{
		CoutRedirect redirect;
		fix.dispatch("go depth 5");
		fix.join_search();
		output = redirect.str();
	}

	const auto split = parse_treenodes(output);
	REQUIRE(split.has_value());
	const auto [main_nodes, qs_nodes] = *split;

	// A zero on either side is what a counter that stopped being incremented looks like
	// from outside the engine.
	CHECK(main_nodes > 0);
	CHECK(qs_nodes > 0);

	// The same invariant Run-Bench.ps1 enforces on every position it measures.
	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	CHECK(info_lines.back().nodes == main_nodes + qs_nodes);
}
