#pragma once

#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace Engine::Logger
{
    // Initialize engine default logger (console + file sink) — idempotent.
    void InitDefault();

    // Create or get the file logger used for performance stats. Truncates file on creation.
    // The parent directory is created by spdlog. Returns nullptr on failure.
    //
    // No default filename by design: a bare relative name lands in whatever the process CWD
    // happens to be, which is how stray SimplePerfStats.txt files ended up at the repo root
    // and in Tests/ (issue #135). Callers must say where the file goes.
    //
    // Only Game::Init() calls this. Everything else uses GetPerfLogger() and writes only if a
    // logger already exists, so non-game contexts (tests, tactical runner, UCI) produce no file.
    std::shared_ptr<spdlog::logger> EnsurePerfLogger(const std::string& filename);

    // Convenience: get PerfStats logger if created
    std::shared_ptr<spdlog::logger> DefaultLogger() noexcept;

    // Convenience: get a logger by name (may return nullptr)
    std::shared_ptr<spdlog::logger> GetLogger(const std::string& name) noexcept;

    // Convenience: get PerfStats logger if created
    std::shared_ptr<spdlog::logger> GetPerfLogger() noexcept;
}