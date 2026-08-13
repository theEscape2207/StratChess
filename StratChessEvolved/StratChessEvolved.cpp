// This is an independent project of an individual developer. Dear PVS-Studio, please check it.

// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "StdAfx.h"
#include "Game.h"
#include "Board.h"
#include "UCIHandler.h"
#include <Tests/Perft.h>
#include <Tests/TacticalTestRunner.h>
#include "Eval.h"
#include "Utils/ArgParse.h"
#include "Utils/FenBatch.h"
#include <spdlog/spdlog.h>

static void print_usage()
{
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

static void test_fen_integration()
{
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
	    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "Starting position", 32, WHITE},
	    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", "Kiwipete", 30, WHITE},
	    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", "Endgame position", 10, WHITE}};

	int passed = 0;
	int failed = 0;

	for (const auto& test : quick_tests) {
		std::cout << "Testing: " << test.description << "\n";
		std::cout << "FEN: " << test.fen << "\n";

		// Load FEN
		if (!board.SetupFromFEN(test.fen)) {
			std::cout << "FAIL: FEN did not parse\n";
			failed++;
			continue;
		}

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
		} else {
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

static int tacticalrunner(int argc, char** argv)
{
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
			const auto parsed_runs = Engine::parse_int(argv[2]);
			if (!parsed_runs) {
				std::cerr << "Error: N must be a number, got '" << argv[2] << "'\n";
				return 1;
			}
			n_runs = *parsed_runs;
			if (n_runs < 1) {
				std::cerr << "Error: N must be >= 1, got " << n_runs << "\n";
				return 1;
			}
		}
		const std::string filename = (argc >= 4) ? argv[3] : "tactical_test_cases.json";
		unsigned threads = 1;
		if (argc >= 5) {
			const auto parsed_threads = Engine::parse_int(argv[4]);
			if (!parsed_threads) {
				std::cerr << "Error: threads must be a number, got '" << argv[4] << "'\n";
				return 1;
			}
			const int parsed = *parsed_threads;
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

static int perftrunner(int argc, char** argv)
{
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
	} else if (command == "run" || command == "divide" || command == "detailed") {
		if (argc < 3) {
			std::cerr << "Error: depth required\n";
			print_usage();
			return 1;
		}

		const auto parsed_depth = Engine::parse_int(argv[2]);
		if (!parsed_depth) {
			std::cerr << "Error: depth must be a number, got '" << argv[2] << "'\n";
			return 1;
		}
		const int depth = *parsed_depth;
		if (depth < 0 || depth > 10) {
			std::cerr << "Error: depth must be between 0 and 10\n";
			return 1;
		}

		Board board;

		if (argc > 3) //custom fen
		{
			std::string fen = argv[3];
			// Custom fen: Kiwipete
			//const std::string fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
			std::cout << "Running perft with custom board setup from FEN at depth " << depth << "\nFEN: " << fen
			          << "\n\n";
			if (!board.SetupFromFEN(fen)) {
				std::cerr << "Error: could not parse FEN '" << fen << "'\n";
				return 1;
			}
		} else {
			// Set up starting position
			board.SetDefaultBoard();
			std::cout << "Running perft on starting position at depth " << depth << "\n\n";
		}

		if (command == "run") {
			auto result = Testing::Perft::run(board, depth, false);
			std::cout << "Nodes: " << result.nodes << "\n";
			std::cout << "Time:  " << result.duration.count() << " ms\n";
			std::cout << "NPS:   " << result.nps() << "\n";
		} else if (command == "divide") {
			Testing::Perft::divide(board, depth);
		} else if (command == "detailed") {
			auto result = Testing::Perft::run_detailed(board, depth);
			result.print();
		}

		return 0;
	} else {
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
static int evalrunner(int argc, char** argv)
{
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

		// Classification (blank/comment/malformed/valid, including the two-tier
		// field-count-then-parser gate) lives in FenBatch::ClassifyLine so it is
		// directly unit-tested (StratChessTests [uci]) without linking this
		// translation unit — see FenBatch.h for why the guard is load-bearing.
		const auto result = FenBatch::ClassifyLine(line);
		if (result.kind == FenBatch::LineKind::Skip) {
			continue;
		}
		if (result.kind == FenBatch::LineKind::Malformed) {
			std::cerr << "Warning: line " << line_no << ": " << result.error << ", skipped: '" << line << "'\n";
			continue;
		}

		Board board;
		if (!board.SetupFromFEN(line)) {
			// ClassifyLine checks syntax only; this is where an illegal position (waiting side in
			// check, issue #45) is caught, and what keeps one out of a tuning corpus.
			std::cerr << "Warning: line " << line_no << ": parses but will not load"
			          << " (illegal position?), skipped: '" << line << "'\n";
			continue;
		}
		const int score = eval->Evaluate(board);
		std::cout << line << '\t' << score << '\n';
	}

	return 0;
}

// Arguments following 'uci'. Exactly one is recognised:
//
//   --log-commands           log received commands to UciHandler::DefaultCommandLogPath()
//   --log-commands=<path>    ... to <path>, verbatim
//
// Recognition is an exact match or the '--log-commands=' prefix with a non-empty remainder --
// deliberately not a starts_with("--log-commands") test, which would accept '--log-commandsfoo'.
// A token that begins with the flag but matches neither form is a near miss, i.e. someone trying
// to use it and failing: that is reported and refused rather than ignored, because silently
// starting without the log is what costs a debugging session. Anything else keeps UCI's own
// convention of ignoring what it does not recognise, but says so, since a client that expected an
// argument to do something should hear that it did not.
//
// Diagnostics go to stderr: stdout is the protocol channel.
//
// Returns false when an argument was malformed; the caller exits non-zero.
static bool parse_uci_args(int argc, char** argv, std::optional<std::string>& log_path)
{
	static constexpr std::string_view FLAG = "--log-commands";

	for (int i = 0; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == FLAG) {
			log_path = UciHandler::DefaultCommandLogPath(); // last occurrence wins
		} else if (arg.starts_with(std::string(FLAG) + "=") && arg.size() > FLAG.size() + 1) {
			log_path = std::string(arg.substr(FLAG.size() + 1));
		} else if (arg.starts_with(FLAG)) {
			std::cerr << "Error: malformed argument '" << arg
			          << "'. Use --log-commands or"
			             " --log-commands=<path>.\n";
			return false;
		} else {
			std::cerr << "Warning: ignoring unrecognised argument '" << arg << "'\n";
		}
	}
	return true;
}

// Convert uncaught startup/runtime exceptions into a process-level failure code.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
	try {
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
			std::optional<std::string> log_path;
			if (argc > 2 && !parse_uci_args(argc - 2, &argv[2], log_path)) {
				return 1;
			}
			UciHandler handler;
			// Fatal rather than a warning: the log was asked for explicitly, and running on
			// without it produces an empty answer to whatever question it was enabled for.
			if (log_path && !handler.EnableCommandLog(*log_path)) {
				std::cerr << "Fatal: could not open UCI command log '" << *log_path << "'\n";
				return 1;
			}
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
	} catch (const std::exception& ex) {
		// Keep fatal diagnostics off stdout, which is the UCI protocol channel.
		std::cerr << "Fatal: " << ex.what() << '\n';
		return 1;
	} catch (...) {
		std::cerr << "Fatal: unknown exception" << '\n';
		return 1;
	}
}
