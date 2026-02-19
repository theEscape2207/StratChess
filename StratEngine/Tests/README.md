# StratChess Engine Tests

This directory contains testing infrastructure for the StratChess engine.

## Overview

- **Perft.h/cpp** - Performance test (Perft) implementation for validating move generation
- **PerftRunner.cpp** - Command-line tool for running perft tests
- **UnitTests.cpp** - Basic unit tests for move generation and board state
- **TestFramework.h** - Simple assertion framework (to be replaced with Google Test/Catch2)

## Perft (Performance Test)

Perft recursively counts all leaf nodes at a given depth. This is the gold standard for validating chess move generation because:

1. **Deterministic** - Same position always produces same count
2. **Comprehensive** - Tests all move types (captures, castling, en passant, promotions)
3. **Standard** - Known positions with verified node counts exist for comparison

### Standard Test Positions

The implementation includes 6 standard positions:

1. **Starting position** - Basic validation
2. **Kiwipete** - Complex middlegame with many piece types
3. **Endgame position** - Tests pawn endgames
4. **Promotion position** - Tests underpromotion and captures
5. **Tricky position** - Edge cases
6. **En passant position** - Tests en passant captures

### Expected Results (Starting Position)

| Depth | Nodes       | Time (approx) |
|-------|-------------|---------------|
| 1     | 20          | < 1ms         |
| 2     | 400         | < 1ms         |
| 3     | 8,902       | < 1ms         |
| 4     | 197,281     | ~50ms         |
| 5     | 4,865,609   | ~1-2s         |
| 6     | 119,060,324 | ~30-60s       |

## Building the Tests

### Visual Studio

Add the test files to your existing StratChessEvolved.vcxproj:

```xml
<ItemGroup>
  <ClCompile Include="..\StratEngine\Tests\Perft.cpp" />
  <ClCompile Include="..\StratEngine\Tests\PerftRunner.cpp" />
  <ClCompile Include="..\StratEngine\Tests\UnitTests.cpp" />
</ItemGroup>
<ItemGroup>
  <ClInclude Include="..\StratEngine\Tests\Perft.h" />
  <ClInclude Include="..\StratEngine\Tests\TestFramework.h" />
</ItemGroup>
```

Or create a separate test project (recommended).

## Running the Tests

### Perft Runner

```bash
# Run standard test suite
StratChessTests.exe test

# Run perft to specific depth on starting position
StratChessTests.exe run 5

# Show move breakdown (divide mode)
StratChessTests.exe divide 4

# Run with detailed statistics
StratChessTests.exe detailed 4
```

### Unit Tests

```bash
StratChessUnitTests.exe
```

## Integration with CI/CD

The perft tests return exit code 0 on success, 1 on failure, making them suitable for CI/CD:

```bash
# In your CI script
./StratChessTests.exe test || exit 1
```

## Performance Benchmarking

Use perft for performance regression testing:

```bash
# Before optimization
./StratChessTests.exe run 5
# Note the NPS (nodes per second)

# After optimization
./StratChessTests.exe run 5
# Compare NPS - should be higher!
```

## Known Issues / TODOs

1. **FEN Parsing** - Currently only starting position is tested. Need to implement FEN string parsing to test all positions.
2. **Detailed Statistics** - Capture/check counting is implemented but not thoroughly tested
3. **Test Framework** - Replace simple TestFramework.h with Google Test or Catch2
4. **Thread Safety** - Add tests for parallel search once implemented

## Adding New Tests

To add a new perft position:

1. Find the position's FEN string
2. Run perft manually to get expected node counts
3. Add to `get_test_positions()` in Perft.cpp

Example:
```cpp
{
    "your-fen-string-here",
    "Description of position",
    { 1, 20, 400, 8902 }  // expected nodes at depths 0-3
}
```

## Debugging Failed Tests

If a perft test fails:

1. **Use divide mode** to see which moves have wrong counts:
   ```bash
   ./StratChessTests.exe divide 3
   ```

2. **Test individual moves** by modifying the test to start from a specific position

3. **Check board state** before/after moves to verify consistency

4. **Common issues**:
   - Missing en passant captures
   - Incorrect castling legality checks
   - Promotion not generating all piece types
   - King left in check after move

## Resources

- [Chess Programming Wiki - Perft](https://www.chessprogramming.org/Perft)
- [Perft Results Database](https://www.chessprogramming.org/Perft_Results)
- [EPD Format](https://www.chessprogramming.org/Extended_Position_Description)
