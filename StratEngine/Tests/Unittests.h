#pragma once
// MIGRATED — Safe to delete after review.
// Replaced by Catch2 v3 in StratChessTests/ (RepetitionTests.cpp, MoveFieldTests.cpp).
// No longer compiled by any project.

#include "TestFramework.h"
#include "RepetitionTests.h"
#include "MoveFieldTests.h"

// Declare test functions here
int run_all_tests()
{
	// Call individual test functions here
	repetition_tests_entry();
	run_move_field_tests();

	// After running all tests, print summary
	Testing::print_test_summary();
	return Testing::all_tests_passed() ? 0 : 1;
}
