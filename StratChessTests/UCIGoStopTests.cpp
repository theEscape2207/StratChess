// UCIGoStopTests.cpp — Catch2 [uci] tests for cmd_go stop and output integrity, and UciWriter injection.
// Shared infrastructure is in UCITestFixture.h; session/administrative commands are in UCITests.cpp.

#include <catch2/catch_test_macros.hpp>
#include "UCITestFixture.h"
#include "Board.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("cmd_go: immediate stop cannot be lost before Search arms", "[uci][immediate_stop]")
{
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	const std::string stopped_output = fix.capture([&] {
		fix.dispatch("go infinite");
		fix.dispatch("stop"); // immediate: may arrive before Search arms its control
	});

	int bestmove_count = 0;
	for (const std::string& line : split_lines(stopped_output)) {
		if (line.starts_with("bestmove "))
			++bestmove_count;
	}
	REQUIRE(bestmove_count == 1);

	// A pending stop from the just-finished search must not poison the next
	// command. This search has no stop command and must complete its fixed depth.
	const std::string next_output = fix.capture([&] {
		fix.dispatch("go depth 2");
		fix.join_search();
	});
	const auto next_info = parse_info_depth_lines(next_output);
	REQUIRE_FALSE(next_info.empty());
	REQUIRE(next_info.back().depth == 2);
	REQUIRE_FALSE(extract_bestmove(next_output).empty());
}

TEST_CASE("cmd_go: stop during go infinite preserves output lines under concurrent isready", "[uci][output_integrity]")
{
	// This is separate from the no-sleep immediate-stop regression above. Wait for observed search
	// output before issuing ready replies so this case proves real output overlap without a blind
	// scheduler delay.
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	constexpr int kIsReadyCount = 20;
	fix.dispatch("go infinite");
	const bool info_observed = fix.wait_for_output("info depth ", std::chrono::seconds(5));
	if (info_observed) {
		for (int i = 0; i < kIsReadyCount; ++i)
			fix.dispatch("isready");
	}
	fix.dispatch("stop");
	const std::string output = fix.output();

	REQUIRE(info_observed);
	const std::regex line_shape{R"(^(info|bestmove|readyok)\b)"};
	int info_count = 0;
	int readyok_count = 0;
	for (const std::string& line : split_lines(output)) {
		if (line.empty())
			continue;
		REQUIRE(std::regex_search(line, line_shape));
		if (line.starts_with("info "))
			++info_count;
		if (line == "readyok")
			++readyok_count;
	}
	REQUIRE(info_count >= 1);
	REQUIRE(readyok_count == kIsReadyCount);
}

TEST_CASE("cmd_go: back-to-back searches emit only their own per-call iterations", "[uci][back_to_back]")
{
	// The observer is passed into exactly one Search call. The second command must
	// therefore start a fresh stream rather than retaining the first call's closure.
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	const std::string first_output = fix.capture([&] {
		fix.dispatch("go depth 4");
		fix.join_search();
	});
	const auto first_search_info = parse_info_depth_lines(first_output);
	REQUIRE_FALSE(first_search_info.empty());
	REQUIRE(first_search_info.front().depth == 1);
	REQUIRE_FALSE(extract_bestmove(first_output).empty());

	const std::string second_output = fix.capture([&] {
		fix.dispatch("go depth 4");
		fix.join_search();
	});
	const auto second_search_info = parse_info_depth_lines(second_output);
	REQUIRE_FALSE(second_search_info.empty());
	REQUIRE(second_search_info.front().depth == 1);
	REQUIRE_FALSE(extract_bestmove(second_output).empty());
}

// ---------------------------------------------------------------------------
// UciWriter injection — these capture through an injected writer, never std::cout, so they can
// run alongside a printing search.
// ---------------------------------------------------------------------------

TEST_CASE("UciWriter: a handler destroyed right after bestmove leaves a clean capture", "[uci][writer]")
{
	auto [writer, sink] = make_capture_writer();

	// Handed over, not shared: once the fixture is gone only the search callbacks can own the writer.
	auto fixture =
	    std::make_unique<UciHandlerTestFixture>(UciHandlerTestFixture::small_hash_config(), std::move(writer));
	fixture->position("position startpos");
	fixture->dispatch("go depth 6");

	REQUIRE(sink->wait_for("bestmove", std::chrono::seconds(10)));

	// The search thread may still be inside the completion callback; a use-after-free of the
	// writer here is what the sanitizer build catches.
	fixture.reset();

	int bestmove_count = 0;
	std::string last_line;
	for (const std::string& line : sink->lines()) {
		if (line.starts_with("bestmove "))
			++bestmove_count;
		last_line = line;
	}
	CHECK(bestmove_count == 1);
	CHECK(last_line.starts_with("bestmove "));
}

TEST_CASE("UciWriter: perft stops a running search before writing its divide transcript", "[uci][writer]")
{
	auto [writer, sink] = make_capture_writer();

	UciHandlerTestFixture fix(UciHandlerTestFixture::small_hash_config(), writer);
	fix.position("position startpos");
	fix.dispatch("go infinite");

	REQUIRE(sink->wait_for("info depth", std::chrono::seconds(10)));

	fix.perft("perft 2");

	const std::vector<std::string> lines = sink->lines();

	// The divide block runs from the first divide-format line to the end.
	size_t divide_start = lines.size();
	for (size_t i = 0; i < lines.size(); ++i) {
		if (std::regex_match(lines[i], kDivideLine)) {
			divide_start = i;
			break;
		}
	}
	REQUIRE(divide_start < lines.size());

	int bestmove_count = 0;
	size_t bestmove_index = lines.size();
	for (size_t i = 0; i < divide_start; ++i) {
		if (lines[i].starts_with("bestmove ")) {
			++bestmove_count;
			bestmove_index = i;
		}
	}
	CHECK(bestmove_count == 1);
	CHECK(bestmove_index < divide_start);

	// No search output lands inside the divide block: cmd_perft stops and joins the search first.
	for (size_t i = divide_start; i < lines.size(); ++i) {
		INFO("line " << i << ": " << lines[i]);
		CHECK_FALSE(lines[i].starts_with("info "));
		CHECK_FALSE(lines[i].starts_with("bestmove "));
	}

	const std::vector<std::string> divide_block(lines.begin() + static_cast<std::ptrdiff_t>(divide_start), lines.end());
	REQUIRE(divide_block.size() == 22); // 20 root moves + "" + "Total nodes: N"

	int move_lines = 0;
	uint64_t total = 0;
	for (size_t i = 0; i + 2 < divide_block.size(); ++i) {
		std::smatch m;
		REQUIRE(std::regex_match(divide_block[i], m, kDivideLine));
		++move_lines;
		total += std::stoull(m[2].str());
	}
	CHECK(move_lines == 20);
	CHECK(divide_block[divide_block.size() - 2].empty());
	CHECK(divide_block.back() == "Total nodes: " + std::to_string(total));
	CHECK(total == 400);
}
