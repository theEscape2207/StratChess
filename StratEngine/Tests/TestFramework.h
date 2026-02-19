#pragma once

// Simple assertion macros for testing
// Replace with Google Test or Catch2 later

#include <iostream>
#include <sstream>

namespace Testing {

static int g_test_count = 0;
static int g_pass_count = 0;
static int g_fail_count = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        g_test_count++; \
        if (!(condition)) { \
            g_fail_count++; \
            std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << " - " << message << "\n"; \
            std::cerr << "  Condition: " << #condition << "\n"; \
        } else { \
            g_pass_count++; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL(expected, actual) \
    do { \
        g_test_count++; \
        if ((expected) != (actual)) { \
            g_fail_count++; \
            std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::cerr << "  Expected: " << (expected) << "\n"; \
            std::cerr << "  Actual:   " << (actual) << "\n"; \
        } else { \
            g_pass_count++; \
        } \
    } while(0)

inline void print_test_summary() {
    std::cout << "\n========================================\n";
    std::cout << "Test Summary\n";
    std::cout << "========================================\n";
    std::cout << "Total:  " << g_test_count << "\n";
    std::cout << "Passed: " << g_pass_count << "\n";
    std::cout << "Failed: " << g_fail_count << "\n";
    std::cout << "========================================\n";
}

inline bool all_tests_passed() {
    return g_fail_count == 0;
}

inline void reset_test_counters() {
    g_test_count = 0;
    g_pass_count = 0;
    g_fail_count = 0;
}

} // namespace Testing
