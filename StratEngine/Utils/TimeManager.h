#pragma once
#include <chrono>
#include <atomic>

namespace chess {

    /// Time management for search
    class TimeManager {
    public:
        void start(std::chrono::milliseconds allocated) noexcept {
            start_time_ = std::chrono::steady_clock::now();
            allocated_time_ = allocated;
            should_stop_.store(false, std::memory_order_relaxed);
        }

        void stop() noexcept {
            should_stop_.store(true, std::memory_order_relaxed);
        }

        [[nodiscard]] bool should_stop_search() const noexcept {
            if (should_stop_.load(std::memory_order_relaxed))
                return true;

            auto elapsed = std::chrono::steady_clock::now() - start_time_;
            return elapsed >= allocated_time_;
        }

        [[nodiscard]] auto elapsed() const noexcept {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time_);
        }

    private:
        std::chrono::steady_clock::time_point start_time_;
        std::chrono::milliseconds allocated_time_;
        std::atomic<bool> should_stop_{ false };
    };

} // namespace chess