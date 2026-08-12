#pragma once
#include <chrono>
#include <atomic>

namespace chess {

	/// Time management for search — tracks both a soft (iteration) and hard (abort) limit.
	///
	/// Soft limit (`should_stop_iteration`): finish the current depth, then stop.
	/// Hard limit (`should_stop_search`):    abort the search immediately mid-node.
	///
	/// Backward compat: the one-arg `start(allocated)` sets soft == hard (no early-stop).
	///
	/// Threading contract: `start()` must be called from the **same thread** that drives the
	/// search (i.e. the thread that later calls `should_stop_search()` / `should_stop_iteration()`).
	/// `stop()` may be called from any thread (it uses a relaxed atomic store).
	/// When a UCI `stop` handler on a separate thread needs to pre-arm the timer, promote
	/// `start_time_`, `soft_limit_`, and `allocated_time_` to `std::atomic` or guard with a mutex.
	class TimeManager {
	  public:
		/// Start timer with separate soft and hard budgets.
		/// @param soft  Target stop time: stop after current depth completes.
		/// @param hard  Emergency cutoff: abort mid-search when exceeded.
		void start(std::chrono::milliseconds soft, std::chrono::milliseconds hard) noexcept
		{
			start_time_ = std::chrono::steady_clock::now();
			soft_limit_ = soft;
			allocated_time_ = hard;
			should_stop_.store(false, std::memory_order_relaxed);
		}

		/// Start timer with a single budget (soft == hard).  Preserves existing behaviour
		/// for all callers that use a fixed time limit via SetTimeLimit().
		void start(std::chrono::milliseconds allocated) noexcept { start(allocated, allocated); }

		/// Signal immediate stop (e.g. from UCI 'stop' command).
		void stop() noexcept { should_stop_.store(true, std::memory_order_relaxed); }

		/// Fast check: reads only the latched atomic — no clock call.
		/// Returns true once should_stop_search() has fired at least once, or after stop().
		/// Use this for per-node early-exit guards where chrono::now() overhead matters.
		[[nodiscard]] bool is_aborted() const noexcept { return should_stop_.load(std::memory_order_relaxed); }

		/// Hard limit check — abort the search immediately.
		/// On first expiry, latches should_stop_ so that subsequent is_aborted() calls
		/// use the fast atomic path (single load, no clock) for O(depth) stack collapse.
		[[nodiscard]] bool should_stop_search() const noexcept
		{
			if (should_stop_.load(std::memory_order_relaxed))
				return true;
			auto el = std::chrono::steady_clock::now() - start_time_;
			if (el >= allocated_time_) {
				should_stop_.store(true, std::memory_order_relaxed); // latch
				return true;
			}
			return false;
		}

		/// Soft limit check — stop after the current depth completes.
		/// Only meaningful when start(soft, hard) was called with soft < hard.
		[[nodiscard]] bool should_stop_iteration() const noexcept
		{
			if (should_stop_.load(std::memory_order_relaxed))
				return true;
			auto el = std::chrono::steady_clock::now() - start_time_;
			return el >= soft_limit_;
		}

		[[nodiscard]] auto elapsed() const noexcept
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
			                                                             start_time_);
		}

	  private:
		std::chrono::steady_clock::time_point start_time_;
		std::chrono::milliseconds soft_limit_{1000};
		std::chrono::milliseconds allocated_time_{1000};
		mutable std::atomic<bool> should_stop_{false}; // mutable: latched in should_stop_search() const
	};

} // namespace chess
