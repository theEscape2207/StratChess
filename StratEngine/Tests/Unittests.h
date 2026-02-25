#pragma once

#include "TestFramework.h"
#include "RepetitionTests.h"

// Declare test functions here
int run_all_tests()
{
	// Call individual test functions here
	repetition_tests_entry();
	
	// After running all tests, print summary
	Testing::print_test_summary();
	return Testing::all_tests_passed() ? 0 : 1;
}
