#include "Perft.h"
#include "../Board.h"
#include "../Move.h"
#include "../MoveHelper.h"
#include "../MoveFormatter.h"
#include "../MoveGenerator.h"
#include "../GameState.h"
#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Testing {

	using json = nlohmann::json;

	// Standard perft test positions
	std::vector<PerftPosition> Perft::get_test_positions(bool extendedSuite) {
		if (extendedSuite)
		{
			return Perft::load_perft_tests_modern("perft_test_cases.json");
		}
		return {
			// Position 1: Starting position
			{
				"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
				{
					1,          // depth 0
					20,         // depth 1
					400,        // depth 2
					8902,       // depth 3
					197281,     // depth 4
					4865609,    // depth 5
					119060324   // depth 6 (optional - takes ~30 seconds)
				},
				"Starting position"
			},
			// Position 2: Kiwipete (complex middlegame position)
			{
				"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
				{
					1,          // depth 0
					48,         // depth 1
					2039,       // depth 2
					97862,      // depth 3
					4085603,    // depth 4
					193690690,  // depth 5 (optional - takes time)
					8031647685, // depth 6 (optional - takes a long time)
				},
				"Kiwipete position"
			},
			// Position 3: Position with lots of checks and captures
			{
				"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
				{
					1,          // depth 0
					14,         // depth 1
					191,        // depth 2
					2812,       // depth 3
					43238,      // depth 4
					674624,     // depth 5
					11030083    // depth 6
				},
				"Endgame position"
			},
			// Position 4: Position with many promotions
			{
				"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
				{
					1,          // depth 0
					6,          // depth 1
					264,        // depth 2
					9467,       // depth 3
					422333,     // depth 4
					15833292,   // depth 5
					706045033   // depth 6
				},
				"Promotion position"
			},
			// Position 5: Tricky position
			{
				"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
				{
					1,          // depth 0
					44,         // depth 1
					1486,       // depth 2
					62379,      // depth 3
					2103487,    // depth 4
					89941194    // depth 5 (optional)
				},
				"Tricky position"
			},
			// Position 6: Position with en passant field (not actual ep possible)
			{
				"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
				{
					1,          // depth 0
					20,         // depth 1
					600, // Stockfish and I agree - Seems Claude got original "400" from a copy of the starting pos :-P // depth 2
					13160, // Not "8902",			// depth 3
					405385, // Again, not "197281", // depth 4
					9771632, // Not "4865609"     // depth 5
				},
				"Position after e2-e4"
			},

			// Position 7: An alternative Perft (from cpw.org) - midgame position with a lot of moves
			{
				"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
				{
					1,          // depth 0
					46,         // depth 1
					2079,		// depth 2
					89890,		// depth 3
					3894594,	// depth 4
					164075551,	// depth 5
				},
				"An alternative Perft (midgame)"
			}
		};
	}

	std::vector<PerftPosition> Perft::load_perft_tests_modern(const std::string& json_filename) {
		std::filesystem::path path = std::filesystem::current_path().parent_path();
		path /= "Tests/";
		path.append(json_filename);
		std::ifstream file(path);
		if (!file.is_open()) {
			std::cerr << "ERROR: Cannot open " << json_filename << std::endl;
			std::cerr << "Working directory: " << std::filesystem::current_path() << std::endl;
			throw std::runtime_error("Failed to load perft test cases");
		}
		json data = json::parse(file);

		std::vector<PerftPosition> test_cases;
		test_cases.reserve(data["perft_test_cases"].size());

		for (const auto& tc : data["perft_test_cases"]) {
			PerftPosition test;
			test.fen = tc["fen"].get<std::string>();
			// Convert depths from JSON object to vector
			const auto& depths = tc["depths"];
			int max_depth = 0;
			for (auto it = depths.begin(); it != depths.end(); ++it) {
				int depth = std::stoi(it.key());
				if (depth > max_depth) {
					max_depth = depth;
				}
			}
			test.expected_nodes.resize(max_depth + 1, 0); // +1 to include depth 0
			for (auto it = depths.begin(); it != depths.end(); ++it) {
				int depth = std::stoi(it.key());
				uint64_t expected = it.value();
				test.expected_nodes[depth] = expected;
			}
			test_cases.push_back(std::move(test));
		}
		return test_cases;
	}

	// Basic perft - just counts nodes
	uint64_t Perft::perft_recursive(Board& board, int depth) {
		if (depth == 0) {
			return 1;
		}

		// MoveGenerator needs latest GameInfo, which only board has - so ignore passed info
		GameInfo curInfo = board.GetGameInfo();

		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, curInfo, moves);

		uint64_t nodes = 0;

		for (const auto& move : moves) {
			// Try to make the move
			if (!board.DoMove(move)) {
				// Illegal move (leaves king in check)
				continue;
			}

			// Recursively count nodes at lower depth
			nodes += perft_recursive(board, depth - 1);

			// Undo the move
			board.UndoMove(move);
		}

		return nodes;
	}

	// Detailed perft - counts moves by type
	void Perft::perft_detailed_recursive(Board& board, int depth, PerftResult& result) {
		if (depth == 0) {
			result.nodes++;
			return;
		}

		auto info = board.GetGameInfo();
		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, info, moves);

		for (const auto& move : moves) {
			// Try to make the move
			if (!board.DoMove(move)) {
				// Illegal move
				continue;
			}

			// Count move types (only at depth 1 to count each move once)
			if (depth == 1) {
				result.nodes++;

				const auto type = MoveHelper::AsType(move);
				// Check move type
				switch (type) {
				case MoveType::CAPTURE:
					result.captures++;
					break;
				case MoveType::EP_CAPTURE:
					result.en_passant++;
					result.captures++;
					break;
				case MoveType::KING_CASTLE:
				case MoveType::QUEEN_CASTLE:
					result.castles++;
					break;
				case MoveType::PROMOTION_KNIGHT:
				case MoveType::PROMOTION_BISHOP:
				case MoveType::PROMOTION_ROOK:
				case MoveType::PROMOTION_QUEEN:
					result.promotions++;
					break;
				case MoveType::PROMOTION_KNIGHT_CAPTURE:
				case MoveType::PROMOTION_BISHOP_CAPTURE:
				case MoveType::PROMOTION_ROOK_CAPTURE:
				case MoveType::PROMOTION_QUEEN_CAPTURE:
					// Phase 4: capture-promotions have their own MoveType (CAPTURE_BIT + PROMOTION_BIT).
					result.promotions++;
					result.captures++;
					break;
				default:
					break;
				}

				// Check if move gives check
				if (board.InCheck()) {
					result.checks++;

					auto currInfo = board.GetGameInfo();
					// Check if it's checkmate
					MoveList nextMoves;
					MoveGenerator::ComputeLegalMoves(board, currInfo, nextMoves);
					bool hasLegalMove = false;
					for (const auto& nextMove : nextMoves) {
						if (board.DoMove(nextMove)) {
							hasLegalMove = true;
							board.UndoMove(nextMove);
							break;
						}
					}
					if (!hasLegalMove) {
						result.checkmates++;
					}
				}
			}
			else {
				// Recurse deeper
				perft_detailed_recursive(board, depth - 1, result);
			}

			// Undo the move
			board.UndoMove(move);
		}
	}

	// Main perft interface
	PerftResult Perft::run(Board& board, int depth, bool divide_mode) {
		PerftResult result;

		auto start = std::chrono::high_resolution_clock::now();

		if (divide_mode) {
			divide(board, depth);
		}
		else {
			result.nodes = perft_recursive(board, depth);
		}

		auto end = std::chrono::high_resolution_clock::now();
		result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

		return result;
	}

	// Detailed perft
	PerftResult Perft::run_detailed(Board& board, int depth) {
		PerftResult result;

		auto start = std::chrono::high_resolution_clock::now();
		perft_detailed_recursive(board, depth, result);
		auto end = std::chrono::high_resolution_clock::now();

		result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

		return result;
	}

	// Divide mode - shows node count for each root move
	void Perft::divide(Board& board, int depth) {
		if (depth == 0) {
			std::cout << "Nodes: 1\n";
			return;
		}

		auto curInfo = board.GetGameInfo();
		MoveList moves;
		MoveGenerator::ComputeLegalMoves(board, curInfo, moves);

		uint64_t total_nodes = 0;

		for (const auto& move : moves) {
			if (!board.DoMove(move)) {
				continue;
			}

			uint64_t nodes = perft_recursive(board, depth - 1);
			total_nodes += nodes;

			std::cout << MoveFormatter::ToUCI(move) << ": " << nodes << "\n";

			board.UndoMove(move);
		}

		std::cout << "\nTotal nodes: " << total_nodes << "\n";
	}

	// Print perft results
	void PerftResult::print() const {
		std::cout << "Nodes:       " << nodes << "\n";
		std::cout << "Captures:    " << captures << "\n";
		std::cout << "E.P.:        " << en_passant << "\n";
		std::cout << "Castles:     " << castles << "\n";
		std::cout << "Promotions:  " << promotions << "\n";
		std::cout << "Checks:      " << checks << "\n";
		std::cout << "Checkmates:  " << checkmates << "\n";
		std::cout << "Time:        " << duration.count() << " ms\n";
		std::cout << "NPS:         " << nps() << "\n";
	}

	// Run standard test suite
	bool Perft::run_test_suite(bool extended, bool verbose) {
		
		auto positions = get_test_positions(extended);
		bool all_passed = true;
		int test_count = 0;
		int passed_count = 0;

		std::cout << "\n========================================\n";
		std::cout << "Running Perft Test Suite\n";
		std::cout << "========================================\n\n";

		auto start = std::chrono::high_resolution_clock::now();

		for (const auto& pos : positions) {
			std::cout << "Testing: " << pos.description << "\n";
			std::cout << "FEN: " << pos.fen << "\n";

			// Set up board from FEN
			Board board;

			// A suite FEN that does not parse fails the suite: running the position anyway would
			// report node counts for an empty board, and skipping it quietly would let
			// run_test_suite return true for a suite that never ran every position.
			if (!board.SetupFromFEN(pos.fen)) {
				all_passed = false;
				std::cout << "  FAIL: FEN failed to parse, position not run\n\n";
				continue;
			}

			// Test each depth (limit to reasonable depths for speed)
			int max_test_depth = std::min(5, static_cast<int>(pos.expected_nodes.size()) - 1);
#ifdef DEBUG
			max_test_depth = std::min(4, static_cast<int>(pos.expected_nodes.size()) - 1);	// Debug builds are slower
#endif // DEBUG


			for (int depth = 1; depth <= max_test_depth; ++depth) {
				test_count++;

				if (verbose) {
					std::cout << "  Depth " << depth << ": ";
					std::cout.flush();
				}

				auto result = run(board, depth, false);
				uint64_t expected = pos.expected_nodes[depth];

				bool passed = (result.nodes == expected);
				if (passed) {
					passed_count++;
					if (verbose) {
						std::cout << "✓ PASS (" << result.nodes << " nodes, "
							<< result.duration.count() << " ms, "
							<< result.nps() << " nps)\n";
					}
				}
				else {
					all_passed = false;
					std::cout << "✗ FAIL\n";
					std::cout << "    Expected: " << expected << "\n";
					std::cout << "    Got:      " << result.nodes << "\n";
				}
			}
			std::cout << "\n";
		}

		std::cout << "========================================\n";
		std::cout << "Results: " << passed_count << "/" << test_count << " tests passed\n";
		if (all_passed) {
			std::cout << "OK - ALL TESTS PASSED\n";
		}
		else {
			std::cout << "NO! SOME TESTS FAILED\n";
		}
		std::cout << "========================================\n\n";
		auto end = std::chrono::high_resolution_clock::now();
		std::cout << "Time spent: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start) << " seconds\n";
		
		return all_passed;
	}

} // namespace Testing
