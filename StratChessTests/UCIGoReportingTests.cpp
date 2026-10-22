// UCIGoReportingTests.cpp — Catch2 [uci] tests for cmd_go reporting: info lines, pv, score, node limits, timing
// and the measurement-contract lines.
// Shared infrastructure is in UCITestFixture.h; session/administrative commands are in UCITests.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "UCITestFixture.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// cmd_go — per-iteration 'info' reporting
// ---------------------------------------------------------------------------

namespace {

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

TEST_CASE("cmd_go: before position, 'go depth 1' completes with a legal starting move", "[uci]")
{
	UciHandlerTestFixture fix;

	const std::string output = fix.capture([&] {
		fix.dispatch("go depth 1");
		fix.join_search();
	});

	const std::string bestmove = extract_bestmove(output);
	REQUIRE_FALSE(bestmove.empty());
	REQUIRE(replay_pv_is_legal(Board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"), {bestmove}));
}

TEST_CASE("cmd_go: 'go depth 4' emits per-iteration info lines with strictly increasing depth", "[uci]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	const std::string output = fix.capture([&] {
		fix.dispatch("go depth 4");
		fix.join_search();
	});

	const auto info_lines = parse_info_depth_lines(output);
	// At least the 4 per-iteration lines (depths 1-4) plus the unchanged final
	// summary line -- whose depth repeats the last iteration's, so only the
	// first 4 are asserted to be strictly increasing.
	REQUIRE(info_lines.size() >= 4);
	for (size_t i = 1; i < 4; ++i) {
		REQUIRE(info_lines[i].depth > info_lines[i - 1].depth);
	}
	REQUIRE(info_lines[0].depth == 1);
	for (const auto& info : info_lines) {
		CHECK(info.depth <= 4);
		CHECK(info.hashfull >= 0);
		CHECK(info.hashfull <= 1000);
	}

	int bestmove_lines = 0;
	for (const std::string& line : split_lines(output)) {
		if (line.starts_with("bestmove "))
			++bestmove_lines;
	}
	REQUIRE(bestmove_lines == 1);
}

TEST_CASE("cmd_go: hashfull falls when the same workload uses a larger table", "[uci][hashfull]")
{
	auto final_hashfull = [](unsigned hash_mb) {
		UciHandlerTestFixture fix;
		const std::string output = fix.capture([&] {
			fix.ucinewgame();
			fix.setoption("setoption name Hash value " + std::to_string(hash_mb));
			fix.position("position startpos");
			fix.dispatch("go depth 10");
			fix.join_search();
		});

		const auto info_lines = parse_info_depth_lines(output);
		REQUIRE_FALSE(info_lines.empty());
		return info_lines.back().hashfull;
	};

	const int small = final_hashfull(1);
	const int large = final_hashfull(64);
	CHECK(small > 0);
	CHECK(large < small);
}

TEST_CASE("cmd_go: iteration and final info times share one monotonic origin", "[uci][timing]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	const std::string output = fix.capture([&] {
		fix.dispatch("go depth 5");
		fix.join_search();
	});

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE(info_lines.size() >= 2);
	for (size_t i = 1; i < info_lines.size(); ++i)
		CHECK(info_lines[i].time_ms >= info_lines[i - 1].time_ms);
	CHECK(info_lines.back().time_ms >= info_lines[info_lines.size() - 2].time_ms);
}

TEST_CASE("cmd_go: the last info line's pv and score agree with bestmove", "[uci]")
{
	// The final info line and bestmove use the same SearchResult.
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	const std::string output = fix.capture([&] {
		fix.dispatch("go depth 4");
		fix.join_search();
	});

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

	const std::string output = fix.capture([&] {
		fix.dispatch("go nodes 20000");
		fix.join_search();
	});

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

	const std::string output = fix.capture([&] {
		fix.dispatch("go nodes 1");
		fix.join_search();
	});

	REQUIRE_FALSE(extract_bestmove(output).empty());
}

TEST_CASE("cmd_go: a node-limited search never reports a spliced pv", "[uci]")
{
	// Fixed node budgets make the abort point repeatable at Threads=1.
	// Check several abort points because each may expose an invalid spliced PV.
	const int budget = GENERATE(5'000, 10'000, 20'000, 50'000);

	UciHandlerTestFixture fix;
	fix.position("position startpos moves e2e4 e7e5 g1f3");

	const std::string output = fix.capture([&] {
		fix.dispatch("go nodes " + std::to_string(budget));
		fix.join_search();
	});

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

	const std::string output = fix.capture([&] {
		fix.dispatch("go nodes 20000");
		fix.join_search();
	});

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	REQUIRE_FALSE(extract_bestmove(output).empty());

	Board board;
	REQUIRE(board.SetupFromFEN("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2"));
	REQUIRE(replay_pv_is_legal(board, info_lines.back().pv));
}

TEST_CASE("cmd_go: at depth >= 3 the pv carries more than one move and replays legally", "[uci]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	const std::string output = fix.capture([&] {
		fix.dispatch("go depth 4");
		fix.join_search();
	});

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

	const std::string output = fix.capture([&] {
		fix.dispatch("go depth 4");
		fix.join_search();
	});

	const auto info_lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(info_lines.empty());
	REQUIRE(info_lines.back().score.starts_with("mate "));
	REQUIRE(extract_bestmove(output) == "a1a8");
}

TEST_CASE("AIPerplex::Search: emits no per-iteration output without a per-call observer", "[uci]")
{
	// A direct concrete call has no observer argument. This pins that observer
	// state lives only in that call, rather than on the AIPerplex service.
	UciHandlerTestFixture fix;
	fix.ucinewgame();
	fix.position("position startpos");

	const std::string output = fix.capture([&] { fix.run_search_directly(4); });

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

	const std::string output = fix.capture([&] {
		fix.dispatch("go movetime 300");
		fix.join_search();
	});

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
	// the time Search() reports the final total (AIPerplex.h's IterationInfo doc).
	REQUIRE(last_iteration.nodes <= final_line.nodes);
}

// ---------------------------------------------------------------------------
// Measurement contract over the wire
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
	// Run-Bench.ps1 reads absence as "an older build", so losing this line would not
	// error out — it would silently relabel every future run as an old one.
	UciHandlerTestFixture fix;
	const std::string output = fix.capture([&] { fix.uci(); });

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

	const std::string output = fix.capture([&] {
		fix.dispatch("go depth 5");
		fix.join_search();
	});

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

TEST_CASE("cmd_go: 'lmp skips' is reported only when late move pruning skipped a move", "[uci][lmp]")
{
	// The timing screen and the strength lab read this exact spelling.
	const std::regex lmp_line(R"(^info string lmp skips [1-9]\d*$)");
	const bool enabled = GENERATE(true, false);

	UciHandlerTestFixture fix;
	fix.ucinewgame();
	fix.set_late_move_pruning(enabled);
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	const std::string output = fix.capture([&] {
		fix.dispatch("go depth 6");
		fix.join_search();
	});

	int matching = 0;
	int any_lmp = 0;
	for (const std::string& line : split_lines(output)) {
		if (std::regex_match(line, lmp_line))
			++matching;
		if (line.starts_with("info string lmp"))
			++any_lmp;
	}
	CAPTURE(enabled);
	CHECK(any_lmp == matching);
	CHECK(matching == (enabled ? 1 : 0));
}
