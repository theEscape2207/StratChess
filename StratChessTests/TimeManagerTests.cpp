#include <catch_amalgamated.hpp>
#include "Utils/TimeUtils.h"
#include "Utils/TimeManager.h"
#include <thread>

using namespace std::chrono_literals;

// ============================================================
// Formula tests — deterministic, no sleeps, < 1 ms each
// ============================================================
TEST_CASE("compute_budget: blitz midgame (3+2, 150 s remaining)", "[time_mgr]")
{
    // Example A from design doc: base = 149950/30 + 1600 ≈ 6598 ms
    auto b = Engine::compute_budget(150'000ms, 2'000ms, 0);
    REQUIRE(b.soft.count() >= 6'000);
    REQUIRE(b.soft.count() <= 7'500);
    // hard = min(base*3, usable*0.5) ≈ 19800
    REQUIRE(b.hard.count() >= b.soft.count());
    REQUIRE(b.hard.count() <= 75'000);
}

TEST_CASE("compute_budget: classical with moves_to_go (60 min, 20 moves left)", "[time_mgr]")
{
    // Example B from design doc: base = 3599950/20 = 180000 ms = 3 min
    auto b = Engine::compute_budget(3'600'000ms, 0ms, 20);
    REQUIRE(b.soft.count() >= 179'000);
    REQUIRE(b.soft.count() <= 181'000);
    // hard = min(180000*3, 3599950*0.5) = 540000 ms
    REQUIRE(b.hard.count() >= 539'000);
    REQUIRE(b.hard.count() <= 541'000);
}

TEST_CASE("compute_budget: increment-heavy time trouble (200 ms remaining, 5 s increment)", "[time_mgr]")
{
    auto b = Engine::compute_budget(200ms, 5'000ms, 0);
    // soft must be >= 100 ms (floor clamp)
    REQUIRE(b.soft.count() >= 100);
    REQUIRE(b.hard.count() >= b.soft.count());
}

TEST_CASE("compute_budget: time trouble floor (remaining < overhead)", "[time_mgr]")
{
    // Less than overhead (50 ms) — must not crash or return zero/negative
    auto b = Engine::compute_budget(30ms, 0ms, 0);
    REQUIRE(b.soft.count() >= 100);
    REQUIRE(b.hard.count() >= b.soft.count());
}

TEST_CASE("compute_budget: zero increment, no moves_to_go", "[time_mgr]")
{
    auto b = Engine::compute_budget(60'000ms, 0ms, 0);
    // base = 59950/30 ≈ 1998 ms ≈ 2 s
    REQUIRE(b.soft.count() >= 1'800);
    REQUIRE(b.soft.count() <= 2'200);
    REQUIRE(b.hard.count() >= b.soft.count());
}

TEST_CASE("compute_budget: hard is always >= soft invariant", "[time_mgr]")
{
    struct Case { int remaining_ms, inc_ms, mtg; };
    for (auto [r, i, m] : std::initializer_list<Case>{
            {5000, 0, 0}, {500, 500, 5}, {100000, 1000, 15}, {50, 0, 0}})
    {
        auto b = Engine::compute_budget(
            std::chrono::milliseconds(r),
            std::chrono::milliseconds(i),
            m);
        CAPTURE(r, i, m);
        REQUIRE(b.hard.count() >= b.soft.count());
        REQUIRE(b.soft.count() >= 100);
    }
}

// ============================================================
// TimeManager timing tests — short sleeps, total < 300 ms
// ============================================================
TEST_CASE("TimeManager: two-arg start — soft fires before hard", "[time_mgr]")
{
    chess::TimeManager tm;
    tm.start(20ms, 60ms);

    REQUIRE_FALSE(tm.should_stop_search());
    REQUIRE_FALSE(tm.should_stop_iteration());

    std::this_thread::sleep_for(25ms);
    // soft should have fired, hard should not yet
    REQUIRE(tm.should_stop_iteration());
    REQUIRE_FALSE(tm.should_stop_search());

    std::this_thread::sleep_for(40ms);
    // now hard fires too
    REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: one-arg start — soft == hard (backward compat)", "[time_mgr]")
{
    chess::TimeManager tm;
    tm.start(20ms);

    REQUIRE_FALSE(tm.should_stop_search());
    std::this_thread::sleep_for(25ms);
    REQUIRE(tm.should_stop_iteration());
    REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: stop() fires should_stop_search immediately", "[time_mgr]")
{
    chess::TimeManager tm;
    tm.start(10'000ms, 20'000ms);
    REQUIRE_FALSE(tm.should_stop_search());
    tm.stop();
    REQUIRE(tm.should_stop_search());
}

TEST_CASE("TimeManager: elapsed() is monotone", "[time_mgr]")
{
    chess::TimeManager tm;
    tm.start(5'000ms);
    auto t1 = tm.elapsed();
    std::this_thread::sleep_for(10ms);
    auto t2 = tm.elapsed();
    REQUIRE(t2 > t1);
}
