#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

class Board;
class Move;

namespace Testing {

	// Perft result structure
	struct PerftResult {
		uint64_t nodes{0};
		uint64_t captures{0};
		uint64_t en_passant{0};
		uint64_t castles{0};
		uint64_t promotions{0};
		uint64_t checks{0};
		uint64_t checkmates{0};
		std::chrono::milliseconds duration{0};

		// Calculate nodes per second
		uint64_t nps() const noexcept
		{
			if (duration.count() == 0)
				return 0;
			return (nodes * 1000) / duration.count();
		}

		// Print results
		void print() const;
	};

	// Perft position for testing
	struct PerftPosition {
		std::string fen;
		std::vector<uint64_t> expected_nodes; // expected_nodes[depth] = node count
		std::string description;              // brief description of the position - optional
	};

	class Perft {
	  public:
		// Main perft function - counts nodes at given depth
		static PerftResult run(Board& board, int depth, bool divide = false);

		// Perft with detailed statistics (captures, checks, etc.)
		static PerftResult run_detailed(Board& board, int depth);

		// Divide mode - shows move breakdown at root
		static void divide(Board& board, int depth);

		// Run standard test suite
		static bool run_test_suite(bool extended, bool verbose = true);

		// Get standard test positions
		static std::vector<PerftPosition> get_test_positions(bool extended);

		//std::vector<PerftTestCase> load_test_cases(const std::string& json_filename);
		static std::vector<PerftPosition> load_perft_tests_modern(const std::string& json_filename);

	  private:
		// Internal recursive perft
		static uint64_t perft_recursive(Board& board, int depth);

		// Internal detailed perft
		static void perft_detailed_recursive(Board& board, int depth, PerftResult& result);

		// (move_to_string removed — use MoveFormatter::ToUCI instead)
	};

} // namespace Testing
