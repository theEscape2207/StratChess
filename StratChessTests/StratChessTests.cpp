// StratChessTests.cpp
// main() is provided by Catch2::Catch2WithMain.
// Test suites: one .cpp per area under StratChessTests/ (see Docs/TestDesign.md for the map).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_run_info.hpp>
#include <catch2/interfaces/catch_interfaces_reporter.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include "Utils/Logger.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {
	// Holds one test case's log lines instead of printing them, so a passing run's
	// console stays clean. Never discarded blind, though: FlushToStderr() exists for
	// exactly the case a bare mute would break -- a failing test's diagnostics (what a
	// rejected FEN was rejected for, an EMERGENCY move's cause) are otherwise stranded
	// in logs/multisink.txt, which CI never uploads (PR #290 review).
	class BufferingSink : public spdlog::sinks::base_sink<std::mutex> {
	  public:
		void Clear() { lines_.clear(); }

		void FlushToStderr()
		{
			for (const auto& line : lines_) {
				std::cerr << line;
			}
			std::cerr.flush();
		}

	  protected:
		void sink_it_(const spdlog::details::log_msg& msg) override
		{
			spdlog::memory_buf_t formatted;
			formatter_->format(msg, formatted);
			lines_.emplace_back(formatted.data(), formatted.size());
		}

		void flush_() override {}

	  private:
		std::vector<std::string> lines_;
	};

	// Several suites deliberately exercise error paths (rejected FENs, degenerate search
	// positions producing an EMERGENCY move) as part of passing behaviour. Those are
	// genuine spdlog log statements at debug/critical level, and printing them to console
	// on a green run makes [critical] something everyone learns to ignore. Muting the
	// console sink and capturing into BufferingSink instead -- a listener runs before any
	// TEST_CASE, winning the Logger::InitDefault() race -- keeps a passing run's console
	// clean while still surfacing a failing test's own log lines on stderr.
	class QuietConsoleLogListener : public Catch::EventListenerBase {
	  public:
		using Catch::EventListenerBase::EventListenerBase;

		void testRunStarting(Catch::TestRunInfo const&) override
		{
			Engine::Logger::InitDefault();
			auto logger = spdlog::default_logger();
			for (auto& sink : logger->sinks()) {
				if (dynamic_cast<spdlog::sinks::stdout_color_sink_mt*>(sink.get())) {
					sink->set_level(spdlog::level::off);
				}
			}
			buffer_ = std::make_shared<BufferingSink>();
			logger->sinks().push_back(buffer_);
		}

		void testCaseStarting(Catch::TestCaseInfo const&) override
		{
			if (buffer_) {
				buffer_->Clear();
			}
		}

		void testCaseEnded(Catch::TestCaseStats const& testCaseStats) override
		{
			if (buffer_ && testCaseStats.totals.assertions.failed > 0) {
				buffer_->FlushToStderr();
			}
		}

	  private:
		std::shared_ptr<BufferingSink> buffer_;
	};
} // namespace

CATCH_REGISTER_LISTENER(QuietConsoleLogListener)
