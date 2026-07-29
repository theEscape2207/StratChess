#include "Logger.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <mutex>

using namespace Engine::Logger;

namespace {
    std::once_flag g_default_init_flag;
    std::once_flag g_perf_init_flag;
    static constexpr char PERF_LOGGER_NAME[] = "PerfStats";
}

void Engine::Logger::InitDefault()
{
    std::call_once(g_default_init_flag, []() {
        try {
            // create console sink
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(spdlog::level::info);
            console_sink->set_pattern(("%T.%e %^%l%$: %v"));

            // create file sink for general logs
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/multisink.txt", true);
            file_sink->set_level(spdlog::level::trace);
			file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

            spdlog::sinks_init_list sink_list = { file_sink, console_sink };

            auto multi = std::make_shared<spdlog::logger>("multi_sink", sink_list.begin(), sink_list.end());
            multi->set_level(spdlog::level::debug);

            // set as default logger so existing code that uses spdlog::info() continues to work
            spdlog::set_default_logger(multi);
            spdlog::flush_on(spdlog::level::info);
        }
        catch (const spdlog::spdlog_ex&) {
            // ignore — caller code may fallback to std::cout if needed
        }
    });
}

std::shared_ptr<spdlog::logger> Engine::Logger::EnsurePerfLogger(const std::string& filename)
{
    // idempotent; create PerfStats logger if not present
    auto existing = spdlog::get(PERF_LOGGER_NAME);
    if (existing) return existing;

    try {
        std::call_once(g_perf_init_flag, [&]() {
            // spdlog's file_helper::open creates the parent directory itself, so the caller
            // does not need to pre-create logs/.
            // Create a synchronous file sink that truncates the file on startup (match previous behavior)
            // If you want async perf logging later, switch to init_thread_pool + async logger here.
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, true);
            file_sink->set_level(spdlog::level::info);
            auto perf_logger = std::make_shared<spdlog::logger>(PERF_LOGGER_NAME, file_sink);
            perf_logger->set_level(spdlog::level::info);
            perf_logger->flush_on(spdlog::level::info);
            spdlog::register_logger(perf_logger);
        });
    }
    catch (...) {
        return nullptr;
    }

    return spdlog::get(PERF_LOGGER_NAME);
}

std::shared_ptr<spdlog::logger> Engine::Logger::GetLogger(const std::string& name) noexcept
{
    return spdlog::get(name);
}

std::shared_ptr<spdlog::logger> Engine::Logger::DefaultLogger() noexcept
{
	return spdlog::default_logger();
}

std::shared_ptr<spdlog::logger> Engine::Logger::GetPerfLogger() noexcept
{
    return spdlog::get(PERF_LOGGER_NAME);
}