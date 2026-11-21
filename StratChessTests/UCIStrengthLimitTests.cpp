// UCIStrengthLimitTests.cpp — Catch2 [uci][strength] tests for UCI_LimitStrength / UCI_Elo: the options,
// what prints while limiting, the depth cap and the final block's swap. The pick itself is
// StrengthLimitTests.cpp's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "UCITestFixture.h"
#include "StrengthLimit.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {
	constexpr const char* kKiwipete = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";

	// Level 4.0094 under the provisional table: the pick depth is 5.
	constexpr int kElo = 1590;
	constexpr int kPickDepth = 5;

	std::string go(UciHandlerTestFixture& fix, const std::string& command)
	{
		return fix.capture([&] {
			fix.dispatch(command);
			fix.join_search();
		});
	}

	void limit(UciHandlerTestFixture& fix, int elo, unsigned multipv)
	{
		fix.setoption("setoption name MultiPV value " + std::to_string(multipv));
		fix.setoption("setoption name UCI_Elo value " + std::to_string(elo));
		fix.setoption("setoption name UCI_LimitStrength value true");
	}

	// Every 'info depth' line grouped by depth, in emission order; the last group is the final block.
	std::vector<std::vector<ParsedInfoLine>> blocks_by_depth(const std::string& output)
	{
		std::vector<std::vector<ParsedInfoLine>> blocks;
		for (ParsedInfoLine& line : parse_info_depth_lines(output)) {
			if (line.multipv <= 1)
				blocks.emplace_back();
			REQUIRE_FALSE(blocks.empty());
			blocks.back().push_back(std::move(line));
		}
		return blocks;
	}

	int max_depth(const std::string& output)
	{
		int depth = 0;
		for (const ParsedInfoLine& line : parse_info_depth_lines(output))
			depth = std::max(depth, line.depth);
		return depth;
	}

	int cp_of(const std::string& score)
	{
		REQUIRE(score.starts_with("cp "));
		return std::stoi(score.substr(3));
	}

	void check_same_line(const ParsedInfoLine& actual, const ParsedInfoLine& expected)
	{
		CHECK(actual.depth == expected.depth);
		CHECK(actual.score == expected.score);
		CHECK(actual.pv == expected.pv);
	}

	// The final block of an unlimited MultiPV 4 search at the pick depth, from fresh state: the ranked
	// set a limited search of the same position picks from.
	std::vector<ParsedInfoLine> control_set(const std::string& position)
	{
		UciHandlerTestFixture fix;
		fix.setoption("setoption name MultiPV value 4");
		fix.position(position);
		const auto blocks = blocks_by_depth(go(fix, "go depth " + std::to_string(kPickDepth)));
		REQUIRE(blocks.size() >= 2);
		const std::vector<ParsedInfoLine>& set = blocks.back();
		REQUIRE(set.size() == 4);
		REQUIRE(set[0].depth == kPickDepth);
		return set;
	}

	// The first handler seed whose drawn search seed makes the pick choose rank `index` of `set`.
	uint64_t seed_picking(const std::vector<ParsedInfoLine>& set, size_t index)
	{
		std::vector<int> scores;
		for (const ParsedInfoLine& line : set)
			scores.push_back(cp_of(line.score));
		const double level = StrengthLimit::level_for_elo(kElo);
		for (uint64_t seed = 0; seed < 100'000; ++seed) {
			if (StrengthLimit::pick(scores, level, std::mt19937_64(seed)()) == index)
				return seed;
		}
		FAIL("no seed picks rank " << index + 1);
		return 0;
	}
} // namespace

// ---------------------------------------------------------------------------
// The options
// ---------------------------------------------------------------------------

TEST_CASE("Strength options: advertised after MultiPV", "[uci][strength]")
{
	UciHandlerTestFixture fix;
	const std::string output = fix.capture([&] { fix.uci(); });
	const size_t check = output.find("option name UCI_LimitStrength type check default false\n");
	const size_t spin = output.find("option name UCI_Elo type spin default 1320 min 1320 max 2600\n");
	REQUIRE(check != std::string::npos);
	REQUIRE(spin != std::string::npos);
	CHECK(output.find("option name MultiPV ") < check);
	CHECK(check < spin);
	CHECK(spin < output.find("uciok"));
	CHECK_FALSE(fix.limit_strength());
	CHECK(fix.uci_elo() == StrengthLimit::kMinElo);
}

TEST_CASE("Strength options: UCI_Elo clamps, saturates and ignores like MultiPV", "[uci][strength]")
{
	UciHandlerTestFixture fix;
	const auto set = [&](const std::string& value) {
		return fix.capture([&] { fix.setoption("setoption name UCI_Elo value " + value); });
	};

	CHECK(set("2000").empty()); // no echo
	CHECK(fix.uci_elo() == 2000);
	set("abc");
	set("-1");
	set("");
	CHECK(fix.uci_elo() == 2000);
	set("1000");
	CHECK(fix.uci_elo() == StrengthLimit::kMinElo);
	set("9999");
	CHECK(fix.uci_elo() == StrengthLimit::kMaxElo);
	set("1500");
	set("4294967296"); // past unsigned: saturates, never wraps
	CHECK(fix.uci_elo() == StrengthLimit::kMaxElo);
	set("0");
	CHECK(fix.uci_elo() == StrengthLimit::kMinElo);
}

TEST_CASE("Strength options: UCI_LimitStrength takes true or false and raises MultiPV", "[uci][strength]")
{
	UciHandlerTestFixture fix;
	const auto set = [&](const std::string& value) {
		return fix.capture([&] { fix.setoption("setoption name UCI_LimitStrength value " + value); });
	};

	CHECK(set("true").empty());
	CHECK(fix.limit_strength());
	CHECK(fix.ai_multipv() == StrengthLimit::kMinMultiPV);
	for (const char* bad : {"maybe", "TRUE", "1", ""}) {
		INFO("value '" << bad << "'");
		CHECK(set(bad).find("info string UCI_LimitStrength: value must be true or false") != std::string::npos);
		CHECK(fix.limit_strength());
	}
	CHECK(fix.capture([&] { fix.setoption("setoption name UCI_LimitStrength"); }).find("value must be") !=
	      std::string::npos);

	// The search's MultiPV follows the user's N, raised while limiting.
	fix.setoption("setoption name MultiPV value 2");
	CHECK(fix.ai_multipv() == 4);
	fix.setoption("setoption name MultiPV value 6");
	CHECK(fix.ai_multipv() == 6);
	fix.setoption("setoption name MultiPV value 2");
	set("false");
	CHECK_FALSE(fix.limit_strength());
	CHECK(fix.ai_multipv() == 2);

	// Both kept across ucinewgame.
	set("true");
	fix.setoption("setoption name UCI_Elo value 2000");
	fix.ucinewgame();
	CHECK(fix.limit_strength());
	CHECK(fix.uci_elo() == 2000);
	CHECK(fix.ai_multipv() == 4);

	// Idle-only, like every option.
	fix.start_silent_search();
	const std::string refused = set("false");
	fix.stop();
	CHECK(refused.find("setoption: ignored") != std::string::npos);
	CHECK(fix.limit_strength());
}

// ---------------------------------------------------------------------------
// What prints
// ---------------------------------------------------------------------------

TEST_CASE("Strength output: the user's N decides the visible ranks", "[uci][strength]")
{
	UciHandlerTestFixture fix;
	limit(fix, kElo, 2);
	fix.position("position startpos");
	for (const auto& block : blocks_by_depth(go(fix, "go depth 9"))) {
		REQUIRE(block.size() == 2);
		CHECK(block[0].multipv == 1);
		CHECK(block[1].multipv == 2);
	}

	fix.setoption("setoption name MultiPV value 1");
	const std::string single = go(fix, "go depth 9");
	CHECK(single.find("multipv") == std::string::npos);
	CHECK_FALSE(extract_bestmove(single).empty());
}

TEST_CASE("Strength output: turning limiting off restores the user's MultiPV output", "[uci][strength]")
{
	const std::string position = std::string("position fen ") + kKiwipete;
	UciHandlerTestFixture plain;
	plain.setoption("setoption name MultiPV value 3");
	plain.position(position);
	const auto expected = blocks_by_depth(go(plain, "go depth 5"));

	UciHandlerTestFixture fix;
	limit(fix, kElo, 3);
	fix.position(position);
	go(fix, "go depth 3");
	fix.setoption("setoption name UCI_LimitStrength value false");
	fix.ucinewgame();
	fix.position(position);
	const std::string output = go(fix, "go depth 5");
	const auto actual = blocks_by_depth(output);

	REQUIRE(actual.size() == expected.size());
	for (size_t b = 0; b < actual.size(); ++b) {
		REQUIRE(actual[b].size() == 3);
		REQUIRE(actual[b].size() == expected[b].size());
		for (size_t r = 0; r < actual[b].size(); ++r) {
			INFO("block " << b << " rank " << r + 1);
			CHECK(actual[b][r].multipv == expected[b][r].multipv);
			check_same_line(actual[b][r], expected[b][r]);
		}
	}
	CHECK(extract_bestmove(output) == expected.back()[0].pv.front());
}

// ---------------------------------------------------------------------------
// The depth cap
// ---------------------------------------------------------------------------

TEST_CASE("Strength depth: a limited search stops at the pick depth", "[uci][strength]")
{
	const auto [elo, command, depth] = GENERATE(table<int, const char*, int>({
	    {kElo, "go depth 9", kPickDepth},
	    {kElo, "go depth 3", 3},
	    {kElo, "go nodes 2000000", kPickDepth},
	    {kElo, "go movetime 3000", kPickDepth},
	    {kElo, "go wtime 60000 btime 60000", kPickDepth},
	    {StrengthLimit::kMinElo, "go depth 9", 1},
	}));
	INFO(command << " at UCI_Elo " << elo);
	UciHandlerTestFixture fix;
	limit(fix, elo, 1);
	fix.position("position startpos");
	const std::string output = go(fix, command);
	CHECK(max_depth(output) == depth);
	CHECK_FALSE(extract_bestmove(output).empty());
}

TEST_CASE("Strength depth: go infinite is not capped", "[uci][strength]")
{
	UciHandlerTestFixture fix;
	limit(fix, StrengthLimit::kMinElo, 1); // pick depth 1
	fix.position("position startpos");
	const std::string output = fix.capture([&] {
		fix.dispatch("go infinite");
		REQUIRE(fix.wait_for_output("info depth 3 ", std::chrono::seconds(30)));
		fix.dispatch("stop");
	});
	CHECK(max_depth(output) >= 3);
	const auto lines = parse_info_depth_lines(output);
	REQUIRE_FALSE(lines.empty());
	CHECK(extract_bestmove(output) == lines.back().pv.front());
}

// ---------------------------------------------------------------------------
// The pick and the final block
// ---------------------------------------------------------------------------

TEST_CASE("Strength pick: a pick below rank 1 swaps into the final block's multipv 1", "[uci][strength]")
{
	const std::vector<ParsedInfoLine> set = control_set("position startpos");
	const uint64_t seed = seed_picking(set, 3);

	SECTION("user MultiPV 3: the hidden rank 4 is played, ranks 2 and 3 stay, rank 1 drops out")
	{
		UciHandlerTestFixture fix;
		limit(fix, kElo, 3);
		fix.reseed_strength(seed);
		fix.position("position startpos");
		const std::string output = go(fix, "go depth 9");
		const auto blocks = blocks_by_depth(output);
		REQUIRE(blocks.size() == kPickDepth + 1);
		for (const auto& block : blocks)
			REQUIRE(block.size() == 3);

		// The last iteration block is the real analysis: ranks 1..3 of the set.
		const auto& last = blocks[kPickDepth - 1];
		for (size_t r = 0; r < 3; ++r)
			check_same_line(last[r], set[r]);

		const auto& final_block = blocks.back();
		check_same_line(final_block[0], set[3]);
		check_same_line(final_block[1], set[1]);
		check_same_line(final_block[2], set[2]);
		for (const ParsedInfoLine& line : final_block)
			CHECK(line.pv != set[0].pv);
		CHECK(extract_bestmove(output) == set[3].pv.front());
	}

	SECTION("user MultiPV 1: the final line is the picked line, untagged")
	{
		UciHandlerTestFixture fix;
		limit(fix, kElo, 1);
		fix.reseed_strength(seed);
		fix.position("position startpos");
		const std::string output = go(fix, "go depth 9");
		CHECK(output.find("multipv") == std::string::npos);
		const auto lines = parse_info_depth_lines(output);
		REQUIRE(lines.size() == kPickDepth + 1);
		check_same_line(lines[kPickDepth - 1], set[0]);
		check_same_line(lines.back(), set[3]);
		CHECK(extract_bestmove(output) == set[3].pv.front());
	}
}

TEST_CASE("Strength pick: bestmove is a ranked move and leads the final block", "[uci][strength]")
{
	const std::string position = std::string("position fen ") + kKiwipete;
	const std::vector<ParsedInfoLine> set = control_set(position);
	for (uint64_t seed = 0; seed < 12; ++seed) {
		INFO("seed " << seed);
		UciHandlerTestFixture fix;
		limit(fix, kElo, 1);
		fix.reseed_strength(seed);
		fix.position(position);
		const std::string output = go(fix, "go depth 9");
		const ParsedInfoLine final_line = parse_info_depth_lines(output).back();
		const auto it = std::find_if(set.begin(), set.end(),
		                             [&](const ParsedInfoLine& l) { return l.pv.front() == extract_bestmove(output); });
		REQUIRE(it != set.end());
		check_same_line(final_line, *it);
		CHECK(replay_pv_is_legal(Board(kKiwipete), final_line.pv));
	}
}

TEST_CASE("Strength pick: no alternatives means no pick", "[uci][strength]")
{
	// The checked king's only move is Kxb2; the mated king has none; at MultiPV 218 one node stops the
	// fallback position's depth-1 set after its rank 1 (UCIMultiPVTests.cpp's positions).
	const auto [fen, command, best, multipv] = GENERATE(table<const char*, const char*, const char*, unsigned>({
	    {"k7/8/8/8/8/8/1q6/K7 w - - 0 1", "go depth 9", "a1b2", 3},
	    {"rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3", "go depth 9", "0000", 3},
	    {"4k3/8/8/8/8/8/8/RN1QKBNR w KQ - 0 1", "go nodes 1", "", 218},
	}));
	INFO(fen);
	std::string expected = best;
	if (expected.empty()) {
		UciHandlerTestFixture control;
		control.setoption("setoption name MultiPV value " + std::to_string(multipv));
		control.position(std::string("position fen ") + fen);
		expected = extract_bestmove(go(control, command));
	}
	for (uint64_t seed = 0; seed < 8; ++seed) {
		INFO("seed " << seed);
		UciHandlerTestFixture fix;
		limit(fix, StrengthLimit::kMinElo, multipv);
		fix.reseed_strength(seed);
		fix.position(std::string("position fen ") + fen);
		const std::string output = go(fix, command);
		CHECK(extract_bestmove(output) == expected);
		const auto blocks = blocks_by_depth(output);
		REQUIRE_FALSE(blocks.empty());
		CHECK(blocks.back().size() == 1);
		CHECK(blocks.back()[0].pv.front() == expected);
	}
}
