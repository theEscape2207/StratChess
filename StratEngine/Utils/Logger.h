#pragma once

#include <memory>
#include <string>
#include <spdlog/spdlog.h>

namespace Engine::Logger
{
    // Initialize engine default logger (console + file sink) — idempotent.
    void InitDefault();

    // Create or get an async file logger used for performance stats. Truncates file on creation.
    // Returns nullptr on failure.
    std::shared_ptr<spdlog::logger> EnsurePerfLogger(const std::string& filename = "SimplePerfStats.txt");

    // Convenience: get PerfStats logger if created
    std::shared_ptr<spdlog::logger> DefaultLogger() noexcept;

    // Convenience: get a logger by name (may return nullptr)
    std::shared_ptr<spdlog::logger> GetLogger(const std::string& name) noexcept;

    // Convenience: get PerfStats logger if created
    std::shared_ptr<spdlog::logger> GetPerfLogger() noexcept;
}