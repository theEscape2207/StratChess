#include "../StdAfx.h"
#include "TacticalTestRunner.h"
#include "../Board.h"
#include "../AIPerplex.h"
#include "../PlayerAI.h"
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
			pos.id = tc["id"].get<std::string>();
			pos.category = tc["category"].get<std::string>();
			pos.description = tc["description"].get<std::string>();
			pos.fen = tc["fen"].get<std::string>();
			pos.depth = tc["depth"].get<int>();
			for (const auto& m : tc["best_moves"])
				pos.best_moves.push_back(m.get<std::string>());
			cases.push_back(std::move(pos));
		}
		return cases;
	}

	TacticalResult TacticalTestRunner::run_position(const TacticalPosition& pos, unsigned threads)
	{
		TacticalResult result;
		result.id = pos.id;
		result.category = pos.category;
		result.description = pos.description;

		const Board board(pos.fen);
		AIPerplex ai(AIPerplexConfig{.evaluator = EvalManager::EvalTypes::COMPLEX,
		                             .default_depth = static_cast<unsigned>(pos.depth),
		                             .threads = threads,
		                             .verbose_logging = false});
		ai.StartNewGame();

		const auto t0 = std::chrono::steady_clock::now();
		const Move m = ai.Search(board, SearchLimits::fixed_depth(pos.depth)).best_move;
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

	TacticalTestRunner::SuiteVerdict TacticalTestRunner::evaluate_results(const std::vector<TacticalResult>& results,
	                                                                      double required_pass_rate)
	{
		SuiteVerdict v;
		v.total = static_cast<int>(results.size());
		for (const auto& r : results) {
			if (r.passed)
				++v.passed;
			else if (r.category.rfind("mate", 0) == 0)
				v.failed_mate_ids.push_back(r.id);
		}
		v.pass_rate = (v.total > 0) ? static_cast<double>(v.passed) / v.total : 0.0;
		v.ok = v.total > 0 && v.pass_rate >= required_pass_rate && v.failed_mate_ids.empty();
		return v;
	}

	TacticalTestRunner::StabilityVerdict
	TacticalTestRunner::evaluate_stability(const std::vector<std::vector<TacticalResult>>& runs,
	                                       double required_pass_rate)
	{
		StabilityVerdict v;
		v.runs = static_cast<int>(runs.size());
		if (runs.empty())
			return v; // ok=false: fail safe on empty input

		for (size_t r = 0; r < runs.size(); ++r) {
			if (!evaluate_results(runs[r], required_pass_rate).ok)
				v.failed_run_indices.push_back(static_cast<int>(r) + 1);
		}

		const auto& first = runs.front();
		for (const auto& run : runs) {
			if (run.size() != first.size()) {
				v.comparable = false;
				return v; // ok=false: runs are not position-by-position comparable
			}
		}

		for (size_t i = 0; i < first.size(); ++i) {
			for (size_t r = 1; r < runs.size(); ++r) {
				if (runs[r][i].passed != first[i].passed) {
					v.flipped_ids.push_back(first[i].id);
					break;
				}
			}
		}

		v.ok = v.failed_run_indices.empty() && v.flipped_ids.empty();
		return v;
	}

	bool TacticalTestRunner::run_test_suite(double required_pass_rate, bool verbose, const std::string& json_filename,
	                                        unsigned threads)
	{
		auto positions = load_test_cases(json_filename);

		std::cout << "\n========================================\n";
		std::cout << "Tactical Test Suite (" << positions.size() << " positions, " << json_filename << ")\n";
		std::cout << "========================================\n\n";

		std::vector<TacticalResult> results;

		for (const auto& pos : positions) {
			if (verbose) {
				std::cout << "[" << pos.id << "] " << pos.description << "\n";
				std::cout << "  FEN:   " << pos.fen << "\n";
				std::cout << "  Depth: " << pos.depth << "\n";
				std::cout.flush();
			}

			const TacticalResult result = run_position(pos, threads);

			const char* verdict = result.passed ? "PASS" : "FAIL";
			if (verbose) {
				std::cout << "  Engine: " << result.engine_move_uci << "  Expected: [";
				for (size_t i = 0; i < pos.best_moves.size(); ++i) {
					if (i)
						std::cout << ", ";
					std::cout << pos.best_moves[i];
				}
				std::cout << "]  " << verdict << "  (" << result.time_ms << " ms)\n\n";
			}

			results.push_back(result);
		}

		const SuiteVerdict v = evaluate_results(results, required_pass_rate);

		std::cout << "========================================\n";
		std::cout << "Results: " << v.passed << "/" << v.total << " passed"
		          << " (" << static_cast<int>(v.pass_rate * 100) << "%)\n";
		std::cout << "Required: " << static_cast<int>(required_pass_rate * 100)
		          << "% overall, 100% in mate categories\n";
		if (!v.failed_mate_ids.empty()) {
			std::cout << "Mate-category failures (always fatal):";
			for (const auto& id : v.failed_mate_ids)
				std::cout << " " << id;
			std::cout << "\n";
		}
		std::cout << (v.ok ? "PASS" : "FAIL") << "\n";
		std::cout << "========================================\n\n";

		return v.ok;
	}

	bool TacticalTestRunner::run_stability_suite(int n_runs, double required_pass_rate,
	                                             const std::string& json_filename, unsigned threads)
	{
		auto positions = load_test_cases(json_filename);

		std::cout << "\n========================================\n";
		std::cout << "Tactical Stability Suite (" << positions.size() << " positions x " << n_runs << " runs, "
		          << json_filename << ")\n";
		std::cout << "========================================\n\n";

		std::vector<std::vector<TacticalResult>> runs;
		runs.reserve(static_cast<size_t>(n_runs));

		for (int r = 1; r <= n_runs; ++r) {
			std::vector<TacticalResult> results;
			results.reserve(positions.size());
			int64_t run_ms = 0;

			for (const auto& pos : positions) {
				TacticalResult result = run_position(pos, threads);
				run_ms += result.time_ms;
				if (!result.passed) {
					std::cout << "  [" << result.id << "] engine " << result.engine_move_uci << "  FAIL\n";
				}
				results.push_back(std::move(result));
			}

			const SuiteVerdict rv = evaluate_results(results, required_pass_rate);
			std::cout << "Run " << r << "/" << n_runs << ": " << rv.passed << "/" << rv.total << " passed (" << run_ms
			          << " ms)  " << (rv.ok ? "PASS" : "FAIL") << "\n";
			std::cout.flush();
			runs.push_back(std::move(results));
		}

		const StabilityVerdict sv = evaluate_stability(runs, required_pass_rate);

		std::cout << "\n========================================\n";
		std::cout << "Stability: " << sv.runs << " runs, " << sv.failed_run_indices.size() << " failing run(s), "
		          << sv.flipped_ids.size() << " flipped position(s)\n";
		if (!sv.flipped_ids.empty()) {
			std::cout << "Flipped (nondeterministic pass/fail):";
			for (const auto& id : sv.flipped_ids)
				std::cout << " " << id;
			std::cout << "\n";
		}
		std::cout << (sv.ok ? "PASS" : "FAIL") << "\n";
		std::cout << "========================================\n\n";

		return sv.ok;
	}

} // namespace Testing
