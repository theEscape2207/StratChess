#include "Perft.h"
#include "../Board.h"
#include "../Move.h"
#include <iostream>
#include <string>

using namespace Testing;

void print_usage() {
    std::cout << "Perft Test Runner\n";
    std::cout << "=================\n\n";
    std::cout << "Usage:\n";
    std::cout << "  perft test              - Run standard test suite\n";
    std::cout << "  perft run <depth>       - Run perft on starting position\n";
    std::cout << "  perft divide <depth>    - Show move breakdown at depth\n";
    std::cout << "  perft detailed <depth>  - Run with detailed statistics\n";
    std::cout << "\n";
}

int perftrunner_main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "test") {
        // Run full test suite
        bool verbose = true;
        bool all_passed = Perft::run_test_suite(verbose);
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

        // Set up starting position
        Board& board = Board::Instance();  // TODO: Update when Board is no longer singleton
        board.SetDefaultBoard();

        std::cout << "Running perft on starting position at depth " << depth << "\n\n";

        if (command == "run") {
            auto result = Perft::run(board, depth, false);
            std::cout << "Nodes: " << result.nodes << "\n";
            std::cout << "Time:  " << result.duration.count() << " ms\n";
            std::cout << "NPS:   " << result.nps() << "\n";
        }
        else if (command == "divide") {
            Perft::divide(board, depth);
        }
        else if (command == "detailed") {
            auto result = Perft::run_detailed(board, depth);
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
