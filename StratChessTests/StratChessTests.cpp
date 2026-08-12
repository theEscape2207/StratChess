// StratChessTests.cpp
// main() is provided by catch_amalgamated.cpp (Catch2 v3).
// Test suites: one .cpp per area under StratChessTests/ (see Docs/TestDesign.md for the map).

#include <catch_amalgamated.hpp>
#include "Utils/Logger.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {
	// Several suites deliberately exercise error paths (rejected FENs, degenerate search
	// positions producing an EMERGENCY move) as part of passing behaviour. Those are
	// genuine spdlog log statements at debug/critical level, and printing them to console
	// on a green run makes [critical] something everyone learns to ignore. Muting the
	// console sink here -- a listener runs before any TEST_CASE, winning the
	// Logger::InitDefault() race -- keeps a passing run's console clean while the file
	// sink (logs/multisink.txt) still gets everything, for a failure that needs the detail.
	class QuietConsoleLogListener : public Catch::EventListenerBase {
	  public:
		using Catch::EventListenerBase::EventListenerBase;

		void testRunStarting(Catch::TestRunInfo const&) override
		{
			Engine::Logger::InitDefault();
			for (auto& sink : spdlog::default_logger()->sinks()) {
				if (dynamic_cast<spdlog::sinks::stdout_color_sink_mt*>(sink.get())) {
					sink->set_level(spdlog::level::off);
				}
			}
		}
	};
} // namespace

CATCH_REGISTER_LISTENER(QuietConsoleLogListener)
