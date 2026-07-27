// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Game.h"
#include "Board.h"
#include "UCIHandler.h"
#include <Tests/Perft.h>
#include <Tests/TacticalTestRunner.h>
#include "Eval.h"
#include "Utils/FENParser.h"
#include <spdlog/spdlog.h>

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

    Board board;

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

static int tacticalrunner(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: tactical test [filename] | tactical stability [N] [filename] [threads]\n";
        return 1;
    }
    const std::string command = argv[1];
    if (command == "test") {
        const std::string filename = (argc >= 3) ? argv[2] : "tactical_test_cases.json";
        const bool ok = Testing::TacticalTestRunner::run_test_suite(0.90, true, filename);
        return ok ? 0 : 1;
    }
    if (command == "stability") {
        int n_runs = 10;
        if (argc >= 3) {
            try {
                n_runs = std::stoi(argv[2]);
            } catch (const std::exception&) {
                std::cerr << "Error: N must be a number, got '" << argv[2] << "'\n";
                return 1;
            }
            if (n_runs < 1) {
                std::cerr << "Error: N must be >= 1, got " << n_runs << "\n";
                return 1;
            }
        }
        const std::string filename = (argc >= 4) ? argv[3] : "tactical_test_cases.json";
        unsigned threads = 1;
        if (argc >= 5) {
            int parsed = 0;
            try {
                parsed = std::stoi(argv[4]);
            } catch (const std::exception&) {
                std::cerr << "Error: threads must be a number, got '" << argv[4] << "'\n";
                return 1;
            }
            if (parsed < 1) {
                std::cerr << "Error: threads must be >= 1, got " << parsed << "\n";
                return 1;
            }
            // Upper bound (32) is enforced by AIPerplex::SetThreads' own clamp;
            // no need to duplicate the ceiling check here.
            threads = static_cast<unsigned>(parsed);
        }
        const bool ok = Testing::TacticalTestRunner::run_stability_suite(n_runs, 0.90, filename, threads);
        return ok ? 0 : 1;
    }
    std::cerr << "Error: unknown tactical command '" << command << "'\n";
    std::cout << "Usage: tactical test [filename] | tactical stability [N] [filename] [threads]\n";
    return 1;
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

        Board board;

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

// Batch-scores a file of FENs (one per line) and prints "<fen>\t<score>" to
// stdout, one line per input FEN — machine-parseable for #117's tuner and
// for #127's before/after score-identity check
// (.claude/plans/uci-eval-command-term-breakdown.md, D4/D5).
//
// The printed score is the RAW value EvalManager::Evaluate() returns:
// side-to-move-relative, no sign transformation. This preserves a single
// source of truth (the search calls the same Evaluate()), and it is exactly
// the value #127's byte-identity check needs to diff. A consumer that wants
// a White-relative score already has the side-to-move field parsed out of
// the FEN and can flip the sign itself.
//
// stdout carries only "<fen>\t<score>" lines (no banner, no progress) so the
// output file is directly consumable; all diagnostics go to stderr.
static int evalrunner(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: eval <path-to-fen-file>\n";
        return 1;
    }

    std::ifstream file(argv[1]);
    if (!file.is_open()) {
        std::cerr << "Error: could not open FEN file '" << argv[1] << "'\n";
        return 1;
    }

    // Silence the default spdlog logger: with no sink configured yet in this
    // mode it falls back to spdlog's built-in stdout console sink, and
    // FENParser logs parse errors through it — which would otherwise leak
    // onto stdout and break the "only <fen>\t<score> lines" contract above.
    spdlog::set_level(spdlog::level::off);

    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;

        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;   // blank line
        if (line[first] == '#') continue;            // comment

        // A field-count pre-filter, purely so the most damaging malformation
        // gets a precise diagnostic: a FEN missing its side-to-move field is
        // silently treated as Black-to-move (issue #46), which across a large
        // tuning corpus is garbage fitted with no warning.
        //
        // This is NOT the authoritative gate. FENParser's regex actually
        // requires all six standard fields — halfmove and fullmove included —
        // so a 4- or 5-field line still fails the parser check below. The
        // floor is set at 4 to distinguish "this isn't a FEN at all" from
        // "this is a FEN the parser is strict about", which are different
        // problems for whoever is cleaning the corpus.
        std::istringstream field_stream(line);
        int field_count = 0;
        std::string field_tok;
        while (field_stream >> field_tok) ++field_count;
        if (field_count < 4) {
            std::cerr << "Warning: line " << line_no << ": malformed FEN ("
                       << field_count << " field(s), need at least 4), skipped: '"
                       << line << "'\n";
            continue;
        }

        // Belt-and-braces: validate through the real parser too, so a line
        // that clears the field-count floor above but is still rejected by
        // FENParser (bad piece placement, illegal castling letters, etc.)
        // is reported and skipped rather than silently scoring a fresh,
        // still-default-constructed Board — SetupFromFEN() logs and returns
        // without applying anything on a parse error (issue #45/#46
        // territory — corpus hygiene, not just a field count).
        FENParser::FENGameState state;
        std::vector<std::tuple<ePiece, eSquare>> pieces;
        if (auto err = FENParser::ParseFEN(line, state, pieces)) {
            std::cerr << "Warning: line " << line_no << ": " << *err
                       << ", skipped: '" << line << "'\n";
            continue;
        }

        Board board;
        board.SetupFromFEN(line);
        const int score = eval->Evaluate(board);
        std::cout << line << '\t' << score << '\n';
    }

    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "test-fen") {
        test_fen_integration();
        return 0;
        
    }
    if (argc > 1 && std::string(argv[1]) == "unittest") {
        // Unit tests have migrated to StratChessTests.exe (Catch2 v3).
        // Run:             x64\Release\StratChessTests.exe
        // Filter by tag:   StratChessTests.exe [repetition]
        std::cout << "Unit tests have moved to StratChessTests.exe\n";
        return 0;
    }
        
	// UCI mode: default (no args) or explicit "uci" — this is what GUIs expect
	// Game mode: StratChessEvolved.exe game
	if (argc == 1 || std::string(argv[1]) == "uci") {
		spdlog::set_level(spdlog::level::off);
		UciHandler handler;
		handler.run();
		return 0;
	}
	// Check for perft commands
	if (argc > 2 && std::string(argv[1]) == "perft") {
		return perftrunner(argc - 1, &argv[1]);
	}
	// Check for tactical commands
	if (argc > 1 && std::string(argv[1]) == "tactical") {
		return tacticalrunner(argc - 1, &argv[1]);
	}
	// Check for batch eval-scoring command — must come before the
	// unconditional Game::Run() fallthrough below, or an unrecognised arg
	// silently starts a game instead (D4, uci-eval-command-term-breakdown.md).
	if (argc > 1 && std::string(argv[1]) == "eval") {
		return evalrunner(argc - 1, &argv[1]);
	}
	// Explicit game mode or unknown arg — normal game execution
	Game game;
	game.Run();
	return 0;
}