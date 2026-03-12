#include "../StdAfx.h"
#include "TacticalTestRunner.h"
#include "../Board.h"
#include "../AIPerplex.h"
#include "../PlayerBase.h"
#include "../Eval.h"
#include "../MoveFormatter.h"
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace Testing {

using json = nlohmann::json;

std::vector<TacticalPosition> TacticalTestRunner::load_test_cases(const std::string& json_filename)
{
    std::filesystem::path path = std::filesystem::current_path().parent_path();
    path /= "Tests/";
    path.append(json_filename);

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "ERROR: Cannot open " << json_filename << "\n";
        std::cerr << "Working directory: " << std::filesystem::current_path() << "\n";
        throw std::runtime_error("Failed to load tactical test cases");
    }

    json data = json::parse(file);
    std::vector<TacticalPosition> cases;
    cases.reserve(data["tactical_test_cases"].size());

    for (const auto& tc : data["tactical_test_cases"]) {
        TacticalPosition pos;
        pos.id          = tc["id"].get<std::string>();
        pos.category    = tc["category"].get<std::string>();
        pos.description = tc["description"].get<std::string>();
        pos.fen         = tc["fen"].get<std::string>();
        pos.depth       = tc["depth"].get<int>();
        for (const auto& m : tc["best_moves"])
            pos.best_moves.push_back(m.get<std::string>());
        cases.push_back(std::move(pos));
    }
    return cases;
}

TacticalResult TacticalTestRunner::run_position(const TacticalPosition& pos)
{
    TacticalResult result;
    result.id          = pos.id;
    result.description = pos.description;

    auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX,
                                 static_cast<unsigned>(pos.depth));
    AIPerplex::SetVerboseLogging(false);
    ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
    Board::Instance().SetupFromFEN(pos.fen);
    GameInfo info = Board::Instance().GetGameInfo();

    const auto t0 = std::chrono::steady_clock::now();
    Move m = ai->GetMove(info);
    const auto t1 = std::chrono::steady_clock::now();

    result.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    result.engine_move_uci = MoveFormatter::ToUCI(m);

    for (const auto& accepted : pos.best_moves) {
        if (result.engine_move_uci == accepted) {
            result.passed = true;
            break;
        }
    }
    return result;
}

bool TacticalTestRunner::run_test_suite(double required_pass_rate, bool verbose)
{
    auto positions = load_test_cases("tactical_test_cases.json");

    std::cout << "\n========================================\n";
    std::cout << "Tactical Test Suite (" << positions.size() << " positions)\n";
    std::cout << "========================================\n\n";

    int passed = 0;
    int failed = 0;

    for (const auto& pos : positions) {
        if (verbose) {
            std::cout << "[" << pos.id << "] " << pos.description << "\n";
            std::cout << "  FEN:   " << pos.fen << "\n";
            std::cout << "  Depth: " << pos.depth << "\n";
            std::cout.flush();
        }

        TacticalResult result = run_position(pos);

        const char* verdict = result.passed ? "PASS" : "FAIL";
        if (verbose) {
            std::cout << "  Engine: " << result.engine_move_uci
                      << "  Expected: [";
            for (size_t i = 0; i < pos.best_moves.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << pos.best_moves[i];
            }
            std::cout << "]  " << verdict
                      << "  (" << result.time_ms << " ms)\n\n";
        }

        if (result.passed) ++passed; else ++failed;
    }

    const int total = passed + failed;
    const double pass_rate = (total > 0) ? static_cast<double>(passed) / total : 0.0;
    const bool ok = pass_rate >= required_pass_rate;

    std::cout << "========================================\n";
    std::cout << "Results: " << passed << "/" << total << " passed"
              << " (" << static_cast<int>(pass_rate * 100) << "%)\n";
    std::cout << "Required: " << static_cast<int>(required_pass_rate * 100) << "%\n";
    std::cout << (ok ? "PASS" : "FAIL") << "\n";
    std::cout << "========================================\n\n";

    return ok;
}

} // namespace Testing
