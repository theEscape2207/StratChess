# Perft Implementation Checklist

Use this checklist to integrate and verify the Perft implementation.

## Phase 1: Basic Integration (30 minutes)

- [ ] Review SUMMARY.md to understand what Perft does
- [ ] Read INTEGRATION.md for step-by-step instructions
- [ ] Add Perft.h and Perft.cpp to your Visual Studio project
- [ ] Add TestFramework.h to your project
- [ ] Ensure all files compile without errors
- [ ] Choose integration approach from IntegrationExamples.cpp
- [ ] Modify your main() to support perft command

## Phase 2: First Run (5 minutes)

- [ ] Build in Release mode (perft is slow in Debug!)
- [ ] Run: `StratChessEvolved.exe perft`
- [ ] Check output for test results

### Expected Output:
```
========================================
Running Perft Test Suite
========================================

Testing: Starting position
FEN: rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
  Depth 1: ✓ PASS (20 nodes, 0 ms)
  Depth 2: ✓ PASS (400 nodes, 1 ms)
  Depth 3: ✓ PASS (8902 nodes, 15 ms)
  Depth 4: ✓ PASS (197281 nodes, 75 ms)
  Depth 5: ✓ PASS (4865609 nodes, 2500 ms)
```

## Phase 3: Debugging (if tests fail)

- [ ] Run divide mode: `StratChessEvolved.exe perft divide 1`
- [ ] Compare output to expected 20 moves
- [ ] Identify which moves are missing or incorrect
- [ ] Fix move generation bugs
- [ ] Re-run tests

## Phase 4: Additional Testing (optional)

- [ ] Add PerftRunner.cpp for standalone test executable
- [ ] Add UnitTests.cpp for automated unit tests
- [ ] Create separate test project in Visual Studio
- [ ] Set up continuous integration (CI) to run tests automatically

## Phase 5: FEN Support (required for full test suite)

Currently only starting position works. To test all 6 positions:

- [ ] Implement FEN parsing in Board or Game class
  ```cpp
  void Board::SetupFromFEN(const std::string& fen);
  ```
- [ ] Update Perft.cpp to use FEN parsing
  ```cpp
  // In run_test_suite(), replace:
  if (pos.fen == "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") {
      board.SetDefaultBoard();
  }
  // With:
  board.SetupFromFEN(pos.fen);
  ```
- [ ] Run full test suite with all 6 positions
- [ ] Verify Kiwipete position passes (complex test)

## Performance Benchmarks

After successful integration, record your baseline performance:

| Depth | Expected Nodes | Your Time | Your NPS | Time (debug) | NPS (debug) |
|-------|----------------|-----------|----------|--------------|-------------|
| 3     | 8,902          | _____ ms  | ________ | 12 ms        | 741833 NPS  |
| 4     | 197,281        | _____ ms  | ________ | 260 ms       | 761000 NPS  |
| 5     | 4,865,609      | _____ ms  | ________ | 6300 ms      | 772000 NPS  |

**Target NPS (nodes per second):**
- Good: 1,000,000+ NPS
- Excellent: 5,000,000+ NPS
- World-class: 20,000,000+ NPS

## Integration with Development Workflow

### Daily Development
- [ ] Run perft depth 4 before committing changes
  - Command: `StratChessEvolved.exe perft run 4`
  - Time: < 1 second
  - Catches: Most bugs

### Before Release
- [ ] Run full test suite
  - Command: `StratChessEvolved.exe perft`
  - Time: < 30 seconds
  - Catches: All known bugs

### Optimization Testing
- [ ] Record baseline NPS
- [ ] Make optimization
- [ ] Measure new NPS
- [ ] Document improvement

## Common Issues and Solutions

### Issue: Tests Don't Compile

**Symptom:** Compiler errors when building

**Solutions:**
- [ ] Check that Board.h is included in Perft.cpp
- [ ] Check that MoveGenerator.h is included
- [ ] Verify GameInfo structure is accessible
- [ ] Add missing includes to Perft.cpp

### Issue: Tests Fail at Depth 1

**Symptom:** Expected 20, got different number

**Solutions:**
- [ ] Run divide mode to see which moves are missing
- [ ] Check if DoMove() properly validates legality
- [ ] Verify MoveGenerator::ComputeLegalMoves() includes all move types
- [ ] Check if move filtering is too aggressive

### Issue: Tests Pass at Depth 1, Fail at Depth 2+

**Symptom:** Depth 1 = 20 (correct), but depth 2 != 400

**Solutions:**
- [ ] Check UndoMove() - likely not properly reversing state
- [ ] Verify hash keys are restored after undo
- [ ] Check that board state is identical after move/unmove
- [ ] Test with QuickVerify.cpp to isolate the issue

### Issue: Tests Are Very Slow

**Symptom:** Depth 4 takes > 5 seconds

**Solutions:**
- [ ] Make sure you're building in **Release mode**, not Debug
- [ ] Enable compiler optimizations (/O2 or /Ox)
- [ ] Profile to find bottlenecks
- [ ] Check if DoMove/UndoMove are inlined
- [ ] Consider bitboard optimizations

### Issue: Different Node Counts Each Run

**Symptom:** Same test gives different results on multiple runs

**Solutions:**
- [ ] **CRITICAL BUG**: Move generation must be deterministic!
- [ ] Check for uninitialized variables
- [ ] Check for random number usage in move generation
- [ ] Verify no reliance on memory addresses or pointer values
- [ ] Check hash table initialization

## Success Metrics

You've successfully integrated Perft when:

✅ All tests at depth 3 pass (8,902 nodes)
✅ All tests at depth 4 pass (197,281 nodes)
✅ Tests complete in reasonable time (< 5 seconds for depth 4)
✅ Results are deterministic (same every run)
✅ Move/unmove consistency tests pass

## Next Steps After Perft Works

1. **Add More Test Positions**
   - Positions where your engine failed
   - Tactical puzzles
   - Edge cases

2. **Integrate with CI/CD**
   - GitHub Actions
   - Jenkins
   - Azure DevOps
   - Automatic testing on every commit

3. **Add Tactical Test Suites**
   - Win At Chess (WAC)
   - Arasan suite
   - Strategic Test Suite (STS)

4. **Performance Tracking**
   - Log NPS over time
   - Create performance regression tests
   - Benchmark before/after optimizations

5. **Migrate to Professional Test Framework**
   - Google Test
   - Catch2
   - Better assertions and reporting

## Documentation to Review

Priority order:

1. **SUMMARY.md** - Overall overview (read first!)
2. **INTEGRATION.md** - Step-by-step integration guide
3. **README.md** - Detailed usage documentation
4. **IntegrationExamples.cpp** - Code examples
5. **QuickVerify.cpp** - Quick sanity check

## Questions? Issues?

If you encounter problems:

1. Check this checklist
2. Review the INTEGRATION.md guide
3. Try QuickVerify.cpp for simple test
4. Use divide mode to debug specific moves
5. Check Chess Programming Wiki for perft reference

## Notes

_Use this space to track your progress or issues:_

```
Date: ___________

Integration Status: [ ] Not started  [ ] In progress  [ ] Complete

Tests Passing:
- Depth 1: [ ]
- Depth 2: [ ]
- Depth 3: [ ]
- Depth 4: [ ]
- Depth 5: [ ]

Performance (NPS): __________

Issues encountered:




Solutions:




```

---

**Good luck!** Perft is a powerful tool that will save you hours of debugging and give you confidence in your move generation. Take your time with the integration and don't hesitate to use divide mode to debug any issues.
