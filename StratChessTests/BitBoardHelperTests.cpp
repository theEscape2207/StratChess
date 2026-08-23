// BitBoardHelperTests.cpp — Catch2 [bitboard] tests for BitBoardHelper (StratEngine/BitBoardHelper.h).
//
// Bits:: (StratEngine/Utils/BitTools.h) already carries compile-time static_assert coverage for
// its bitwise primitives. BitBoardHelper is the thin, mutating wrapper the rest of the engine
// actually calls (set_bits/clear_bits assert their preconditions; print_bitboard formats a board
// for debugging), so these cases exercise it at runtime instead of duplicating those static_asserts.

#include <catch_amalgamated.hpp>
#include "BitBoardHelper.h"
#include <sstream>
#include <string>
#include <vector>

TEST_CASE("BitBoardHelper::set_bits: sets the requested bits without disturbing others", "[bitboard]")
{
	BITBOARD board = 0b1010;
	BitBoardHelper::set_bits(board, 0b0101);
	CHECK(board == 0b1111);
}

TEST_CASE("BitBoardHelper::clear_bits: clears the requested bits and returns true", "[bitboard]")
{
	BITBOARD board = 0b1011;
	CHECK(BitBoardHelper::clear_bits(board, 0b0001));
	CHECK(board == 0b1010);
}

TEST_CASE("BitBoardHelper::clear_bits: a mask overlapping only some set bits still clears the whole mask", "[bitboard]")
{
	// isAnyBitSet only requires the mask to overlap the board somewhere, not to match it exactly --
	// bit 0 of the mask below is already clear, so this pins that clear_bits does not reject on that
	// basis, and that the bit stays clear rather than getting toggled back on.
	BITBOARD board = 0b1010;
	CHECK(BitBoardHelper::clear_bits(board, 0b0011));
	CHECK(board == 0b1000);
}

#ifdef NDEBUG
// clear_bits/set_bits assert their preconditions; violating one aborts a Debug/sanitizer build
// (correct behavior for a caller bug), so the precondition-violation cases below only run in
// Release, matching the Board(fen) pattern in UCITests.cpp for the same reason.

TEST_CASE("BitBoardHelper::clear_bits: no overlap with the mask returns false and leaves the board untouched",
          "[bitboard]")
{
	BITBOARD board = 0b1000;
	CHECK_FALSE(BitBoardHelper::clear_bits(board, 0b0001));
	CHECK(board == 0b1000);
}

TEST_CASE("BitBoardHelper::set_bits: setting an already-set bit is idempotent", "[bitboard]")
{
	BITBOARD board = 0b1011;
	BitBoardHelper::set_bits(board, 0b0001);
	CHECK(board == 0b1011);
}
#endif

namespace {
	// Splits print_bitboard's output into its 8 rank lines, stripping the trailing blank line.
	std::vector<std::string> rank_lines(const std::string& output)
	{
		std::vector<std::string> lines;
		std::istringstream iss(output);
		std::string line;
		while (std::getline(iss, line))
			lines.push_back(line);

		// print_bitboard emits an extra blank line after the 8 rows -- drop it.
		if (!lines.empty() && lines.back().empty())
			lines.pop_back();
		return lines;
	}
} // namespace

TEST_CASE("BitBoardHelper::print_bitboard: empty board prints 8 all-zero rows plus a trailing blank line", "[bitboard]")
{
	std::ostringstream out;
	BitBoardHelper::print_bitboard(out, EMPTY);

	const std::string output = out.str();
	CHECK(output.back() == '\n');

	const auto lines = rank_lines(output);
	REQUIRE(lines.size() == 8);
	for (const auto& line : lines)
		CHECK(line == "00000000");
}

TEST_CASE("BitBoardHelper::print_bitboard: fully-set board prints 8 all-one rows", "[bitboard]")
{
	std::ostringstream out;
	BitBoardHelper::print_bitboard(out, ~EMPTY);

	const auto lines = rank_lines(out.str());
	REQUIRE(lines.size() == 8);
	for (const auto& line : lines)
		CHECK(line == "11111111");
}

TEST_CASE("BitBoardHelper::print_bitboard: a single set bit lands on its own row and column", "[bitboard]")
{
	// Bit index i is printed at row i/8, column i%8 -- pin that mapping directly rather than via
	// square names, since it is print_bitboard's own row/column arithmetic under test here.
	const int bit = GENERATE(0, 9, 27, 63);
	INFO("bit index: " << bit);

	std::ostringstream out;
	BitBoardHelper::print_bitboard(out, UNIT << bit);

	const auto lines = rank_lines(out.str());
	REQUIRE(lines.size() == 8);

	const auto row = static_cast<std::size_t>(bit / 8);
	const auto col = static_cast<std::size_t>(bit % 8);
	for (std::size_t r = 0; r < 8; ++r) {
		for (std::size_t c = 0; c < 8; ++c) {
			const char expected = (r == row && c == col) ? '1' : '0';
			CHECK(lines[r][c] == expected);
		}
	}
}
