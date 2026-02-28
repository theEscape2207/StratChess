// StratChessTests.cpp
// main() is provided by catch_amalgamated.cpp (Catch2 v3).
// Test suites: RepetitionTests.cpp, MoveFieldTests.cpp, PerftTests.cpp

#include <fstream>

// Global log stream required by legacy engine files (ABIterative, AIAgent, Board, etc.)
// The definition lives in StratChessEvolved.cpp in the main project; we provide it here.
std::ofstream outLegalMoves;
