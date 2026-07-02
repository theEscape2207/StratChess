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
    std::string description;
    std::string engine_move_uci;
    bool passed = false;
    int64_t time_ms = 0;
};

class TacticalTestRunner {
public:
    // Run all positions from the JSON file. Prints per-position results and summary.
    // Returns true if pass_rate >= required_pass_rate.
    [[nodiscard]] static bool run_test_suite(double required_pass_rate = 0.90, bool verbose = true);

    // Load positions from a JSON file.
    // Must be run from the Tests/ directory — resolves path as:
    //   current_path().parent_path() / "Tests" / json_filename
    // (same convention as Perft::load_perft_tests_modern)
    [[nodiscard]] static std::vector<TacticalPosition> load_test_cases(const std::string& json_filename);

    // Run a single position. Constructs its own local Board from pos.fen internally.
    static TacticalResult run_position(const TacticalPosition& pos);
};

} // namespace Testing
