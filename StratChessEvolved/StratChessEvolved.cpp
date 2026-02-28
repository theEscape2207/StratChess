// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Game.h"
#include "Board.h"
#include <Tests/Perft.h>
#include <Tests/Unittests.h>

std::ofstream outLegalMoves("legalmoves.txt", std::ios::trunc | std::ios::out);

static void print_usage() {
    std::cout << "Perft Test Runner\n";
    std::cout << "=================\n\n";
    std::cout << "Usage: perft <command> [depth]\n";
    std::cout << "Commands:\n";
    std::cout << "  test               Run full perft test suite\n";
    std::cout << "  run <depth>       Run perft to given depth\n";
    std::cout << "  divide <depth>    Run perft divide to given depth\n";
    std::cout << "  detailed <depth>  Run detailed perft to given depth\n";
    std::cout << "Depth must be between 0 and 10.\n";
}

static void test_fen_integration() {
    std::cout << "\n========================================\n";
    std::cout << "FEN Integration Test\n";
    std::cout << "========================================\n\n";

    Board& board = Board::Instance();

    // Test positions with expected results
    struct TestCase {
        std::string fen;
        std::string description;
        int expectedPieces;
        eColor expectedSide;
    };

    std::vector<TestCase> quick_tests = {
        {
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            "Starting position",
            32,
            WHITE
        },
        {
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
            "Kiwipete",
            30,
            WHITE
        },
        {
            "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
            "Endgame position",
            10,
            WHITE
        }
    };

    int passed = 0;
    int failed = 0;

    for (const auto& test : quick_tests) {
        std::cout << "Testing: " << test.description << "\n";
        std::cout << "FEN: " << test.fen << "\n";

        // Load FEN
        board.SetupFromFEN(test.fen);

        // Verify side to move
        if (board.GetCurrentColor() != test.expectedSide) {
            std::cout << "FAIL: Wrong side to move\n";
            failed++;
            continue;
        }

        // Extract FEN back
        std::string extracted = board.ExtractFEN();
        std::cout << " Extracted: " << extracted << "\n";

        // Round-trip test (piece placement should match)
        // Note: We compare only the first field (piece placement)
        std::string origPlacement = test.fen.substr(0, test.fen.find(' '));
        std::string extrPlacement = extracted.substr(0, extracted.find(' '));

        if (origPlacement == extrPlacement) {
            std::cout << "PASS: Round-trip successful\n";
            passed++;
        }
        else {
            std::cout << "FAIL: Round-trip mismatch\n";
            std::cout << " Original:  " << origPlacement << "\n";
            std::cout << " Extracted: " << extrPlacement << "\n";
            failed++;
        }

        std::cout << "\n";
    }

    std::cout << "========================================\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
    std::cout << "========================================\n\n";
}

static int perftrunner(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "test") {
        // Run full test suite
        bool verbose = true;
        bool all_passed = Testing::Perft::run_test_suite(verbose);
        return all_passed ? 0 : 1;
    }
    else if (command == "run" || command == "divide" || command == "detailed") {
        if (argc < 3) {
            std::cerr << "Error: depth required\n";
            print_usage();
            return 1;
        }

        int depth = std::stoi(argv[2]);
        if (depth < 0 || depth > 10) {
            std::cerr << "Error: depth must be between 0 and 10\n";
            return 1;
        }

        Board& board = Board::Instance();  // TODO: Update when Board is no longer singleton
        
        if (argc > 3)   //custom fen
        {
            std::string fen = argv[3];
            // Custom fen: Kiwipete
            //const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
			std::cout << "Running perft with custom board setup from FEN at depth " << depth << "\nFEN: " << fen << "\n\n";
            board.SetupFromFEN(fen);
        }
        else {
            // Set up starting position
            board.SetDefaultBoard();
            std::cout << "Running perft on starting position at depth " << depth << "\n\n";
        }
        
        if (command == "run") {
            auto result = Testing::Perft::run(board, depth, false);
            std::cout << "Nodes: " << result.nodes << "\n";
            std::cout << "Time:  " << result.duration.count() << " ms\n";
            std::cout << "NPS:   " << result.nps() << "\n";
        }
        else if (command == "divide") {
            Testing::Perft::divide(board, depth);
        }
        else if (command == "detailed") {
            auto result = Testing::Perft::run_detailed(board, depth);
            result.print();
        }

        return 0;
    }
    else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "test-fen") {
        test_fen_integration();
        return 0;
        
    }
    if (argc > 1 && std::string(argv[1]) == "unittest") {
		// Hack: Run unittests if "unittest" is passed as the first argument
        bool all_passed = run_all_tests();
        return all_passed ? 0 : 1;
    }
        
	// Check for perft commands
	if (argc > 2 && std::string(argv[1]) == "perft") {
		return perftrunner(argc - 1, &argv[1]);
	}
	// No Perft - Normal game execution
	Game game;
	game.Run();
	return 0;
}