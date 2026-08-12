#pragma once

#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace Engine::Logger {
	// Initialize engine default logger (console + file sink) — idempotent.
	void InitDefault();

	// Create or get the file logger used for performance stats. Truncates file on creation.
	// The parent directory is created by spdlog. Returns nullptr on failure.
	//
	// No default filename by design: a bare relative name lands in whatever the process CWD
	// happens to be. Callers must say where the file goes.
	//
	// Only Game::Init() calls this. Everything else uses GetPerfLogger() and writes only if a
	// logger already exists, so non-game contexts (tests, tactical runner, UCI) produce no file.
	std::shared_ptr<spdlog::logger> EnsurePerfLogger(const std::string& filename);

	// Create the logger UciHandler writes received commands to. Truncates the file on creation;
	// the parent directory is created by spdlog. Returns nullptr on failure.
	//
	// File sink only, and NEVER the default logger. In UCI mode nothing calls InitDefault(), so
	// spdlog's default logger is still its built-in stdout console sink — silent only because
	// main() sets the global level to off. Logging through it, or raising that level, would write
	// onto stdout, which IS the protocol channel. InitDefault() is no better: it installs a
	// stdout_color_sink_mt as the default. stderr was rejected too — it is unbuffered, so a client
	// merging the two streams sees the lines out of order.
	//
	// Unlike EnsurePerfLogger, the returned logger is NOT registered with spdlog and is NOT
	// guarded by a once_flag: the caller owns the only reference, and the file is released when
	// that reference drops. A registry entry under a fixed name would pin the first filename for
	// the process lifetime and hold its handle open — which makes two tests with different
	// temporary files share the first one, and leaves Windows unable to delete either.
	std::shared_ptr<spdlog::logger> CreateUciCommandLogger(const std::string& filename);

	// Convenience: get PerfStats logger if created
	std::shared_ptr<spdlog::logger> DefaultLogger() noexcept;

	// Convenience: get a logger by name (may return nullptr)
	std::shared_ptr<spdlog::logger> GetLogger(const std::string& name) noexcept;

	// Convenience: get PerfStats logger if created
	std::shared_ptr<spdlog::logger> GetPerfLogger() noexcept;
} // namespace Engine::Logger