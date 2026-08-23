// UCITestFixture.h — shared test infrastructure for the UCI test files (UCITests.cpp,
// UCIReportingTests.cpp): the STRAT_ENABLE_TEST_ACCESS fixture, stdout/stdin capture, and the
// perft divide-line parser used as a board-state oracle by tests outside cmd_perft itself.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include "UCIHandler.h"
#include "AIPerplex.h"
#include "Board.h"
#include "TranspositionTable.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

	// Reads threads_ off UCI's concretely-owned AIPerplex instance — proves
	// the option reaches the search service rather than just the handler's
	// configured_threads_ bookkeeping.
	unsigned ai_threads() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return handler.ai_->threads_;
	}

	// Identity of the live ai_ instance, for proving cmd_ucinewgame() no
	// longer rebuilds it.
	const void* ai_identity() const { return handler.ai_.get(); }

	static constexpr uint64_t TT_MARKER_KEY = 0x7fff'ffff'ffff'fffeULL;

	void store_tt_marker() const
	{
		REQUIRE(handler.ai_ != nullptr);
		handler.ai_->_tt->store(TT_MARKER_KEY, 123, 1, 0, Move::EmptyMove(), BoundType::EXACT, NodeType::PV_NODE,
		                        SearchPhase::MAIN);
	}

	bool has_tt_marker() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return handler.ai_->_tt->probe(TT_MARKER_KEY, 0).has_value();
	}

	size_t ai_hash_requested_mb() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return handler.ai_->_tt->requested_memory_mb();
	}

	size_t ai_hash_memory_mb() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return handler.ai_->_tt->memory_mb();
	}

	size_t ai_hash_bucket_count() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return handler.ai_->_tt->bucket_count();
	}

	const void* tt_identity() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return handler.ai_->_tt.get();
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

	// Calls the concrete root-per-call service directly, bypassing cmd_go so no
	// observer is supplied. The returned result is the authoritative telemetry.
	SearchResult run_search_directly(int depth)
	{
		REQUIRE(handler.ai_ != nullptr);
		return handler.ai_->Search(handler.board_, SearchLimits::fixed_depth(depth));
	}
};

// Builds a legal UCI move sequence of at least `min_plies` plies from the
// starting position: knight shuffles (Ng1-f3-g1 / Ng8-f6-g8) with a
// queenside pawn push every 80 plies so the game stays "real" (the pawn
// moves also keep the sequence trivially verifiable by hand).
inline std::string long_game_moves(int min_plies)
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

// Redirects std::cout into an in-memory buffer for the lifetime of the
// object; restores the original streambuf on destruction (including when
// unwinding past a failed REQUIRE), so a single assertion failure can never
// leave std::cout silently rewired for the rest of the test binary.
class SynchronizedStringBuf final : public std::streambuf {
  public:
	std::string str() const
	{
		std::scoped_lock lock(mutex_);
		return contents_;
	}

	bool wait_for(std::string_view needle, std::chrono::milliseconds timeout) const
	{
		std::unique_lock lock(mutex_);
		return output_ready_.wait_for(lock, timeout, [&] { return contents_.find(needle) != std::string::npos; });
	}

  protected:
	std::streamsize xsputn(const char* text, std::streamsize count) override
	{
		{
			std::scoped_lock lock(mutex_);
			contents_.append(text, static_cast<std::size_t>(count));
		}
		output_ready_.notify_all();
		return count;
	}

	int_type overflow(int_type character) override
	{
		if (traits_type::eq_int_type(character, traits_type::eof()))
			return traits_type::not_eof(character);
		{
			std::scoped_lock lock(mutex_);
			contents_.push_back(traits_type::to_char_type(character));
		}
		output_ready_.notify_all();
		return character;
	}

  private:
	mutable std::mutex mutex_;
	mutable std::condition_variable output_ready_;
	std::string contents_;
};

class CoutRedirect {
  public:
	CoutRedirect() : old_(std::cout.rdbuf(&buffer_)) {}
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
	bool wait_for(std::string_view needle, std::chrono::milliseconds timeout) const
	{
		return buffer_.wait_for(needle, timeout);
	}

  private:
	SynchronizedStringBuf buffer_;
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

// The divide-line wire format external harnesses parse:
// ^\s*([a-h][1-8][a-h][1-8][rnbqRNBQ]?)\s*[:\s]\s*(\d+)$ (#196).
inline const std::regex kDivideLine{R"(^\s*([a-h][1-8][a-h][1-8][rnbqRNBQ]?)\s*[:\s]\s*(\d+)$)"};

// Every (move, nodes) pair the harness regex accepts out of `output`.
inline std::vector<std::pair<std::string, uint64_t>> parse_divide(const std::string& output)
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

inline uint64_t divide_total(const std::string& output)
{
	uint64_t sum = 0;
	for (const auto& entry : parse_divide(output))
		sum += entry.second;
	return sum;
}
