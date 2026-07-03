#pragma once

#include <string>
#include <vector>

namespace Testing {

struct TacticalPosition {
    std::string id;
    std::string category;
    std::string description;
    std::string fen;
    std::vector<std::string> best_moves;  // accepted first moves in UCI format (e.g. "e2e4")
    int depth = 5;
};

struct TacticalResult {
    std::string id;
    std::string category;
    std::string description;
    std::string engine_move_uci;
    bool passed = false;
    int64_t time_ms = 0;
};

class TacticalTestRunner {
public:
    struct SuiteVerdict {
        bool ok = false;
        int passed = 0;
        int total = 0;
        double pass_rate = 0.0;
        std::vector<std::string> failed_mate_ids;
    };

    // Pure verdict policy (unit-tested in SuitePolicyTests.cpp):
    // ok iff total > 0, pass_rate >= required_pass_rate, and no failures
    // in any category whose name starts with "mate".
    [[nodiscard]] static SuiteVerdict evaluate_results(
        const std::vector<TacticalResult>& results, double required_pass_rate);

    struct StabilityVerdict {
        bool ok = false;
        bool comparable = true;              // false if runs differ in size (structural error)
        int runs = 0;
        std::vector<std::string> flipped_ids;    // positions whose pass flag differed across runs
        std::vector<int> failed_run_indices;     // 1-based indices of runs failing evaluate_results
    };

    // Pure stability policy (unit-tested in SuitePolicyTests.cpp):
    // ok iff at least one run, all runs the same size, every run individually
    // satisfies evaluate_results(), and no position's pass flag differs
    // between runs. A flip = nondeterminism (the SMP race-bug signal).
    [[nodiscard]] static StabilityVerdict evaluate_stability(
        const std::vector<std::vector<TacticalResult>>& runs, double required_pass_rate);

    // Run all positions from the JSON file. Prints per-position results and summary.
    // Returns true if pass_rate >= required_pass_rate.
    [[nodiscard]] static bool run_test_suite(double required_pass_rate = 0.90,
                                             bool verbose = true,
                                             const std::string& json_filename = "tactical_test_cases.json");

    // Load positions from a JSON file.
    // Must be run from the Tests/ directory — resolves path as:
    //   current_path().parent_path() / "Tests" / json_filename
    // (same convention as Perft::load_perft_tests_modern)
    [[nodiscard]] static std::vector<TacticalPosition> load_test_cases(const std::string& json_filename);

    // Run a single position. Constructs its own local Board from pos.fen internally.
    static TacticalResult run_position(const TacticalPosition& pos);
};

} // namespace Testing
