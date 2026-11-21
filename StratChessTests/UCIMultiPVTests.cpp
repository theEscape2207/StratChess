// UCIMultiPVTests.cpp — Catch2 [uci][multipv] tests for the MultiPV option and the ranked info lines it
// produces. The ranking itself is SearchMultiPVTests.cpp's; these pin what reaches the wire.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "UCITestFixture.h"
#include "Board.h"
#include "MoveFormatter.h"
#include "MoveGenerator.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {
	constexpr const char* kStart = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
	constexpr const char* kKiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	// A score token pair as one ordered number: any mate for the side to move beats any cp, a shorter
	// mate beats a longer one, and being mated sooner is worst.
	int64_t score_order(const std::string& score)
	{
		const auto space = score.find(' ');
		const std::string kind = score.substr(0, space);
		const int64_t value = std::stoll(score.substr(space + 1));
		if (kind == "cp")
			return value;
		return value > 0 ? 1'000'000 - value : -1'000'000 - value;
	}

	// Every 'info depth' line, grouped into blocks: a block starts at multipv 1.
	std::vector<std::vector<ParsedInfoLine>> ranked_blocks(const std::string& output)
	{
		std::vector<std::vector<ParsedInfoLine>> blocks;
		for (ParsedInfoLine& line : parse_info_depth_lines(output)) {
			REQUIRE(line.multipv >= 1);
			if (line.multipv == 1)
				blocks.emplace_back();
			REQUIRE_FALSE(blocks.empty());
			blocks.back().push_back(std::move(line));
		}
		return blocks;
	}

	// One block: ranks 1..k in order, one depth and one set of counts, distinct first moves, scores
	// that never rise, and every line legal from the root.
	void check_block(const std::vector<ParsedInfoLine>& block, size_t ranks, const char* fen)
	{
		REQUIRE(block.size() == ranks);
		std::set<std::string> first_moves;
		for (size_t i = 0; i < block.size(); ++i) {
			INFO("rank " << i + 1);
			CHECK(block[i].multipv == static_cast<int>(i) + 1);
			CHECK(block[i].depth == block[0].depth);
			CHECK(block[i].seldepth == block[0].seldepth);
			CHECK(block[i].hashfull == block[0].hashfull);
			CHECK(block[i].nodes == block[0].nodes);
			CHECK(block[i].time_ms == block[0].time_ms);
			REQUIRE_FALSE(block[i].pv.empty());
			first_moves.insert(block[i].pv.front());
			CHECK(replay_pv_is_legal(Board(fen), block[i].pv));
			if (i > 0)
				CHECK(score_order(block[i].score) <= score_order(block[i - 1].score));
		}
		CHECK(first_moves.size() == ranks);
	}

	// The final block repeats the last iteration block; only the counts and time may differ.
	void check_final_repeats_last(const std::vector<ParsedInfoLine>& last,
	                              const std::vector<ParsedInfoLine>& final_block)
	{
		REQUIRE(final_block.size() == last.size());
		for (size_t i = 0; i < last.size(); ++i) {
			INFO("rank " << i + 1);
			CHECK(final_block[i].multipv == last[i].multipv);
			CHECK(final_block[i].depth == last[i].depth);
			CHECK(final_block[i].seldepth == last[i].seldepth);
			CHECK(final_block[i].score == last[i].score);
			CHECK(final_block[i].pv == last[i].pv);
		}
	}

	std::string go(UciHandlerTestFixture& fix, const std::string& command)
	{
		return fix.capture([&] {
			fix.dispatch(command);
			fix.join_search();
		});
	}
} // namespace

// ---------------------------------------------------------------------------
// The option
// ---------------------------------------------------------------------------

TEST_CASE("MultiPV option: advertised as a spin from 1 to the legal-move maximum", "[uci][multipv]")
{
	UciHandlerTestFixture fix;
	const std::string output = fix.capture([&] { fix.uci(); });
	REQUIRE(output.find("option name Hash ") < output.find("option name MultiPV "));
	REQUIRE(output.find("option name MultiPV type spin default 1 min 1 max 218\n") != std::string::npos);
	REQUIRE(fix.ai_multipv() == 1);
}

TEST_CASE("MultiPV option: setoption applies, clamps and ignores like Threads", "[uci][multipv]")
{
	UciHandlerTestFixture fix;
	const auto set = [&](const std::string& value) {
		return fix.capture([&] { fix.setoption("setoption name MultiPV value " + value); });
	};

	CHECK(set("3").empty()); // no echo
	CHECK(fix.ai_multipv() == 3);
	set("abc");
	CHECK(fix.ai_multipv() == 3);
	set("-1");
	CHECK(fix.ai_multipv() == 3);
	set("");
	CHECK(fix.ai_multipv() == 3);
	set("4294967296"); // past unsigned: saturates, never wraps to 0
	CHECK(fix.ai_multipv() == 218);
	set("3");
	set("999");
	CHECK(fix.ai_multipv() == 218);
	set("0");
	CHECK(fix.ai_multipv() == 1);

	set("4");
	fix.ucinewgame();
	CHECK(fix.ai_multipv() == 4);

	fix.start_silent_search();
	const std::string refused = set("2");
	fix.stop();
	CHECK(refused.find("setoption: ignored") != std::string::npos);
	CHECK(fix.ai_multipv() == 4);
}

// ---------------------------------------------------------------------------
// The ranked lines
// ---------------------------------------------------------------------------

TEST_CASE("MultiPV lines: N = 1 output carries no multipv field", "[uci][multipv]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");
	const std::string output = go(fix, "go depth 4");
	REQUIRE(extract_bestmove(output).size() >= 4);
	CHECK(output.find("multipv") == std::string::npos);
}

TEST_CASE("MultiPV lines: three ranks from the start position form a coherent set", "[uci][multipv]")
{
	constexpr int kDepth = 5;
	UciHandlerTestFixture fix;
	fix.setoption("setoption name MultiPV value 3");
	fix.position("position startpos");
	const std::string output = go(fix, "go depth " + std::to_string(kDepth));

	const auto blocks = ranked_blocks(output);
	REQUIRE(blocks.size() == kDepth + 1); // one per depth, then the final block
	for (size_t d = 0; d < blocks.size(); ++d) {
		INFO("block " << d);
		check_block(blocks[d], 3, kStart);
		CHECK(blocks[d][0].depth == std::min<int>(static_cast<int>(d) + 1, kDepth));
	}
	check_final_repeats_last(blocks[kDepth - 1], blocks.back());
	CHECK(extract_bestmove(output) == blocks.back()[0].pv.front());

	// Each rank shows its own RootLine: the same search through the service, on a fresh fixture with
	// the same options, has the same scores and full lines.
	UciHandlerTestFixture direct;
	direct.setoption("setoption name MultiPV value 3");
	const SearchResult result = direct.run_search_directly(kDepth);
	REQUIRE(result.alternatives.size() == 2);
	const auto& final_block = blocks.back();
	CHECK(final_block[0].score == "cp " + std::to_string(result.best_score));
	for (size_t i = 0; i < result.alternatives.size(); ++i) {
		INFO("rank " << i + 2);
		const RootLine& line = result.alternatives[i];
		CHECK(final_block[i + 1].score == "cp " + std::to_string(line.score));
		std::vector<std::string> pv;
		for (const Move& move : line.pv)
			pv.push_back(MoveFormatter::ToUCI(move));
		CHECK(final_block[i + 1].pv == pv);
	}
}

TEST_CASE("MultiPV lines: a node budget that cuts a set repeats the last complete set", "[uci][multipv]")
{
	constexpr int kMaxDepth = 8;
	const std::string position = std::string("position fen ") + kKiwipete;

	std::vector<std::vector<ParsedInfoLine>> full;
	{
		UciHandlerTestFixture fix;
		fix.setoption("setoption name MultiPV value 3");
		fix.position(position);
		full = ranked_blocks(go(fix, "go depth " + std::to_string(kMaxDepth)));
	}
	REQUIRE(full.size() == kMaxDepth + 1);
	// A budget just short of the last iteration's end stops inside its set, so that depth is rejected
	// and the previous complete set is what the search returns. Iteration nodes are the main thread's
	// cumulative count, the quantity the budget is polled against at Threads = 1.
	const std::vector<ParsedInfoLine>& kept = full[kMaxDepth - 2];
	const int64_t limit = full[kMaxDepth - 1][0].nodes - 2048;
	REQUIRE(limit - kept[0].nodes > 100'000);

	UciHandlerTestFixture fix;
	fix.setoption("setoption name MultiPV value 3");
	fix.position(position);
	const std::string output = go(fix, "go nodes " + std::to_string(limit));
	const auto blocks = ranked_blocks(output);
	REQUIRE(blocks.size() == kMaxDepth); // depths 1..kMaxDepth-1, then the final block
	check_block(blocks.back(), 3, kKiwipete);
	CHECK(blocks.back()[0].depth == kMaxDepth - 1);
	check_final_repeats_last(kept, blocks.back());
	check_final_repeats_last(blocks[blocks.size() - 2], blocks.back());
	CHECK(extract_bestmove(output) == blocks.back()[0].pv.front());
}

TEST_CASE("MultiPV lines: N above the legal move count prints every legal move", "[uci][multipv]")
{
	// The cornered king has three moves (SearchMultiPVTests.cpp's position).
	constexpr const char* kFen = "7k/8/8/8/8/8/8/K7 w - - 0 1";
	UciHandlerTestFixture fix;
	fix.setoption("setoption name MultiPV value 5");
	fix.position(std::string("position fen ") + kFen);
	const auto blocks = ranked_blocks(go(fix, "go depth 4"));
	REQUIRE(blocks.size() == 5);
	for (const auto& block : blocks)
		check_block(block, 3, kFen);
}

TEST_CASE("MultiPV lines: a lone rank is still tagged multipv 1", "[uci][multipv]")
{
	// The checked king's only move is Kxb2 (verified against python-chess); the mated king has none.
	const auto [fen, best] = GENERATE(
	    table<const char*, const char*>({{"k7/8/8/8/8/8/1q6/K7 w - - 0 1", "a1b2"},
	                                     {"rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3", "0000"}}));
	INFO(fen);
	UciHandlerTestFixture fix;
	fix.setoption("setoption name MultiPV value 3");
	fix.position(std::string("position fen ") + fen);
	const std::string output = go(fix, "go depth 4");

	const auto blocks = ranked_blocks(output);
	REQUIRE(blocks.size() >= 2);
	for (const auto& block : blocks)
		REQUIRE(block.size() == 1);
	check_final_repeats_last(blocks[blocks.size() - 2], blocks.back());
	const std::vector<std::string>& pv = blocks.back()[0].pv;
	REQUIRE_FALSE(pv.empty());
	CHECK(pv.front() == best);
	CHECK(extract_bestmove(output) == best);
	if (std::string_view(best) == "0000")
		CHECK(pv.size() == 1);
}

TEST_CASE("MultiPV lines: the first-depth fallback prints rank 1 alone", "[uci][multipv]")
{
	// SearchMultiPVTests.cpp's fallback position: one node stops depth 1's set after its rank 1.
	UciHandlerTestFixture fix;
	fix.setoption("setoption name MultiPV value 218");
	fix.position("position fen 4k3/8/8/8/8/8/8/RN1QKBNR w KQ - 0 1");
	const std::string output = go(fix, "go nodes 1");

	const auto blocks = ranked_blocks(output);
	REQUIRE(blocks.size() == 2);
	REQUIRE(blocks[0].size() == 1);
	REQUIRE(blocks[1].size() == 1);
	CHECK(blocks[0][0].depth == 1);
	check_final_repeats_last(blocks[0], blocks[1]);
	CHECK(extract_bestmove(output) == blocks[1][0].pv.front());
}
