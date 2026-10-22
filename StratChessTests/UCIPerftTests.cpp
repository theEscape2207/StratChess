// UCIPerftTests.cpp — Catch2 [uci] tests for cmd_perft and the run() dispatch of 'go perft'.
// Shared infrastructure is in UCITestFixture.h; session/administrative commands are in UCITests.cpp.

#include <catch2/catch_test_macros.hpp>
#include "UCITestFixture.h"
#include "Board.h"

#include <iostream>
#include <sstream>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// cmd_perft — "perft <depth>" / "go perft <depth>"
//
// The divide lines are a wire format, not diagnostics: external harnesses parse
// them with ^\s*([a-h][1-8][a-h][1-8][rnbqRNBQ]?)\s*[:\s]\s*(\d+)$, so
// these tests assert against that regex rather than against a substring.
// kDivideLine/parse_divide/divide_total live in UCITestFixture.h, since
// UCITests.cpp uses divide_total as a board-state oracle too.
// ---------------------------------------------------------------------------

TEST_CASE("cmd_perft: startpos depth 1 emits 20 harness-parseable divide lines", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	const std::string output = fix.capture([&] { fix.perft("perft 1"); });

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

	const std::string output = fix.capture([&] { fix.perft("go perft 2"); });

	REQUIRE(divide_total(output) == 400);
	REQUIRE(output.find("bestmove") == std::string::npos);
}

TEST_CASE("cmd_perft: honours the position set by cmd_position", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

	const std::string output = fix.capture([&] { fix.perft("perft 3"); });

	REQUIRE(parse_divide(output).size() == 48);
	REQUIRE(divide_total(output) == 97862);
}

TEST_CASE("cmd_perft: malformed depth is ignored, not guessed at", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	for (const auto* line : {"perft", "perft abc", "perft -1", "perft 11", "go perft"}) {
		const std::string output = fix.capture([&] { fix.perft(line); });
		INFO("input: " << line);
		REQUIRE(parse_divide(output).empty());
	}
}

TEST_CASE("cmd_perft: leaves the board unchanged", "[uci][perft]")
{
	UciHandlerTestFixture fix;
	fix.position("position startpos");

	fix.perft("perft 3");

	REQUIRE(fix.board().GetCurrentColor() == WHITE);

	const std::string output = fix.capture([&] { fix.perft("perft 1"); });
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
	auto [writer, sink] = make_capture_writer();
	UciHandler handler(UciHandlerTestFixture::small_hash_config(), writer);

	CinRedirect input("position startpos\ngo perft 2\nquit\n");
	handler.run();
	const std::string output = sink->str();

	REQUIRE(divide_total(output) == 400);
	REQUIRE(output.find("bestmove") == std::string::npos);
}

TEST_CASE("run(): a bare 'go' still searches after the perft branch was added", "[uci][perft]")
{
	auto [writer, sink] = make_capture_writer();
	UciHandler handler(UciHandlerTestFixture::small_hash_config(), writer);

	CinRedirect input("position startpos\ngo depth 3\nquit\n");
	handler.run();
	const std::string output = sink->str();

	REQUIRE(output.find("bestmove") != std::string::npos);
	REQUIRE(parse_divide(output).empty());
}
