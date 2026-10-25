// UCITestFixture.h — shared test infrastructure for the UCI test files (UCI*Tests.cpp):
// the STRAT_ENABLE_TEST_ACCESS fixture, its injected-writer output capture, the perft divide-line parser used as a board-state oracle by tests outside
// cmd_perft itself, and the output-line parsers the reporting files share.

#pragma once

#include <catch2/catch_test_macros.hpp>
#include "UCIHandler.h"
#include "AIPerplex.h"
#include "SearchTestFixture.h"
#include "Board.h"
#include "TranspositionTable.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Thread-safe capture of everything written through an injected UciWriter; reading it never
// touches std::cout. The writer's sink holds a shared_ptr to it, so it lives as long as the writer.
class CaptureSink {
  public:
	// Line and newline land as two separate writes, as they do on std::cout, so an unserialised
	// writer can tear lines here just as it would on stdout.
	void append(std::string_view line)
	{
		{
			std::scoped_lock lock(mutex_);
			text_ += line;
		}
		{
			std::scoped_lock lock(mutex_);
			text_ += '\n';
		}
		cv_.notify_all();
	}

	// Blocks until the captured text contains `needle`, or the timeout elapses.
	bool wait_for(std::string_view needle, std::chrono::milliseconds timeout) const
	{
		std::unique_lock lock(mutex_);
		return cv_.wait_for(lock, timeout, [&] { return text_.find(needle) != std::string::npos; });
	}

	// Every line captured so far, in emission order, without newlines.
	std::vector<std::string> lines() const
	{
		std::vector<std::string> out;
		std::istringstream iss{str()};
		for (std::string line; std::getline(iss, line);)
			out.push_back(line);
		return out;
	}

	// The captured text exactly as std::cout would have carried it.
	std::string str() const
	{
		std::scoped_lock lock(mutex_);
		return text_;
	}

	// A position in the captured text; str_since() reads back exactly what one action printed.
	size_t mark() const
	{
		std::scoped_lock lock(mutex_);
		return text_.size();
	}
	std::string str_since(size_t mark) const
	{
		std::scoped_lock lock(mutex_);
		return text_.substr(mark);
	}

  private:
	mutable std::mutex mutex_;
	mutable std::condition_variable cv_;
	std::string text_;
};

// A UciWriter that appends to a CaptureSink instead of writing std::cout, plus a handle to that
// sink for the test to read. `sink` outlives `writer`'s destruction (it is a separate shared_ptr),
// so a test can inspect the last captured lines after destroying the writer/handler.
struct CaptureWriter {
	std::shared_ptr<UciWriter> writer;
	std::shared_ptr<CaptureSink> sink;
};

inline CaptureWriter make_capture_writer()
{
	auto sink = std::make_shared<CaptureSink>();
	auto writer = std::make_shared<UciWriter>([sink](std::string_view line) { sink->append(line); });
	return {std::move(writer), std::move(sink)};
}

// Must be defined here — the name must match the friend declaration inside
// UCIHandler.h: friend class UciHandlerTestFixture;
class UciHandlerTestFixture {
  public:
	// A 1 MiB table (TestDesign.md → Test Isolation Rules). Pass UciHandler::DefaultSearchConfig()
	// for the real size.
	static AIPerplexConfig small_hash_config()
	{
		AIPerplexConfig config = UciHandler::DefaultSearchConfig();
		config.hash_mb = 1;
		return config;
	}

	UciHandlerTestFixture() : UciHandlerTestFixture(small_hash_config()) {}
	explicit UciHandlerTestFixture(const AIPerplexConfig& config) : UciHandlerTestFixture(config, make_capture_writer())
	{}
	// Injects a writer (see make_capture_writer) in place of the fixture's own default capture
	// writer -- for the [writer] tests, which manage the sink themselves outside the fixture.
	UciHandlerTestFixture(const AIPerplexConfig& config, std::shared_ptr<UciWriter> writer)
	    : handler(config, std::move(writer))
	{}

	UciHandler handler;

	void position(const std::string& line) { handler.cmd_position(line); }
	const Board& board() const { return handler.board_; }

	bool dispatch(const std::string& line) { return handler.dispatch(line); }

	void perft(const std::string& line) { handler.cmd_perft(line); }
	void setoption(const std::string& line) { handler.cmd_setoption(line); }
	void ucinewgame() { handler.cmd_ucinewgame(); }
	void uci() { handler.cmd_uci(); }
	void eval() { handler.cmd_eval(); }

	// A real infinite search on the handler's board with no observer wired, so it prints nothing
	// and a test can capture a refusal's output exactly. stop() ends it.
	void start_silent_search()
	{
		SearchLimits limits = SearchLimits::infinite_search();
		limits.depth = 50; // what cmd_go gives 'go infinite', so the depth cap never ends it
		handler.ai_->StartAsync(handler.board_, limits, {}, {});
		REQUIRE(handler.ai_->IsSearching());
	}
	void stop() { handler.ai_->StopAndWait(); }

	// Reads threads_ off UCI's concretely-owned AIPerplex instance — proves
	// the option reaches the search service.
	unsigned ai_threads() const { return AIPerlexTestFixture::configured_threads(*handler.ai_); }

	void set_late_move_pruning(bool enabled) const
	{
		auto tuning = ai_tuning();
		tuning.late_move_pruning_enabled = enabled;
		REQUIRE_FALSE(set_tuning(tuning).has_value());
	}

	const SearchTuning& ai_tuning() const { return handler.ai_->Tuning(); }
	std::optional<SearchTuningSchema::TuningError> set_tuning(const SearchTuning& tuning) const
	{
		return handler.ai_->SetTuning(tuning);
	}

	// Identity of the live ai_ instance, for proving cmd_ucinewgame() does not rebuild it.
	const void* ai_identity() const { return handler.ai_.get(); }

	static constexpr uint64_t TT_MARKER_KEY = 0x7fff'ffff'ffff'fffeULL;

	void store_tt_marker() const
	{
		REQUIRE(handler.ai_ != nullptr);
		AIPerlexTestFixture::tt(*handler.ai_)
		    .store(TT_MARKER_KEY, 123, 1, 0, Move::EmptyMove(), BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN,
		           kNoStaticEval);
	}

	bool has_tt_marker() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return AIPerlexTestFixture::tt(*handler.ai_).probe(TT_MARKER_KEY, 0).has_value();
	}

	size_t ai_hash_requested_mb() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return AIPerlexTestFixture::tt(*handler.ai_).requested_memory_mb();
	}

	size_t ai_hash_memory_mb() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return AIPerlexTestFixture::tt(*handler.ai_).memory_mb();
	}

	size_t ai_hash_bucket_count() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return AIPerlexTestFixture::tt(*handler.ai_).bucket_count();
	}

	const void* tt_identity() const
	{
		REQUIRE(handler.ai_ != nullptr);
		return &AIPerlexTestFixture::tt(*handler.ai_);
	}

	// cmd_go() returns as soon as the search is launched, so a test waits for the launch thread
	// (and its output) before inspecting captured output. Wait(), not StopAndWait(): these searches
	// are fixed-depth and expected to finish on their own.
	void join_search() { handler.ai_->Wait(); }

	// Calls the concrete root-per-call service directly, bypassing cmd_go so no
	// observer is supplied. The returned result is the authoritative telemetry.
	SearchResult run_search_directly(int depth)
	{
		return handler.ai_->Search(handler.board_, SearchLimits::fixed_depth(depth));
	}

	// Everything the handler has printed since construction. A fixture built with its own injected
	// writer has no capture of its own; its test reads the sink it injected.
	std::string output() const { return captured().str(); }

	// Blocks until output() contains `needle`, or the timeout elapses.
	bool wait_for_output(std::string_view needle, std::chrono::milliseconds timeout) const
	{
		return captured().wait_for(needle, timeout);
	}

	// Runs `action` and returns exactly what it printed, isolated from anything captured before or
	// after it.
	template <typename F> std::string capture(F&& action)
	{
		const size_t mark = captured().mark();
		std::forward<F>(action)();
		return captured().str_since(mark);
	}

  private:
	UciHandlerTestFixture(const AIPerplexConfig& config, CaptureWriter cw)
	    : handler(config, std::move(cw.writer)), output_(std::move(cw.sink))
	{}

	const CaptureSink& captured() const
	{
		REQUIRE(output_ != nullptr);
		return *output_;
	}

	std::shared_ptr<CaptureSink> output_;
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

// The divide-line wire format external harnesses parse:
// ^\s*([a-h][1-8][a-h][1-8][rnbqRNBQ]?)\s*[:\s]\s*(\d+)$.
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

// Output parsing shared by the UCI reporting test files.

inline std::vector<std::string> split_lines(const std::string& text)
{
	std::vector<std::string> lines;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line))
		lines.push_back(line);
	return lines;
}

// One parsed "info depth ..." line. `score` keeps the two-token form
// ("cp 34" / "mate 3") verbatim so tests can check the kind without
// re-deriving GameValues::Mate_Threshold.
struct ParsedInfoLine {
	int depth = 0;
	int seldepth = -1;
	std::string score;
	int64_t nodes = 0;
	int64_t nps = -1;
	int hashfull = -1;
	int time_ms = 0;
	std::vector<std::string> pv;
};

// Every "info depth ..." line in `output`, in emission order. Deliberately
// tolerant of any other 'info' line shape (e.g. 'info string ...') by only
// matching the "info depth" prefix — those are unrelated protocol output,
// not a parse failure.
inline std::vector<ParsedInfoLine> parse_info_depth_lines(const std::string& output)
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
			} else if (tok == "seldepth") {
				iss >> info.seldepth;
			} else if (tok == "score") {
				std::string kind;
				std::string value;
				iss >> kind >> value;
				info.score = kind + " " + value;
			} else if (tok == "nodes") {
				iss >> info.nodes;
			} else if (tok == "nps") {
				iss >> info.nps;
			} else if (tok == "hashfull") {
				iss >> info.hashfull;
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
inline std::string extract_bestmove(const std::string& output)
{
	for (const std::string& line : split_lines(output)) {
		if (line.starts_with("bestmove ")) {
			return line.substr(std::string("bestmove ").size());
		}
	}
	return {};
}
