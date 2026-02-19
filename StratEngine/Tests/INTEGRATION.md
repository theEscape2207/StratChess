# Perft Integration Guide

## Quick Start (5 minutes)

### Step 1: Add Files to Your Project

In Visual Studio:

1. Right-click on "StratEngine" project
2. Add → Existing Item
3. Select all files from `StratEngine\Tests\` directory:
   - Perft.h
   - Perft.cpp
   - TestFramework.h
   - PerftRunner.cpp (optional - for standalone runner)
   - UnitTests.cpp (optional - for unit test suite)

### Step 2: Create a Test Executable

**Option A: Add to existing main()** (quickest)

In your `StratChessEvolved.cpp`, add:

```cpp
#include "Tests/Perft.h"

int main(int argc, char** argv) {
    // Add perft command support
    if (argc > 1 && std::string(argv[1]) == "perft") {
        return Testing::Perft::run_test_suite(true) ? 0 : 1;
    }
    
    // Existing game code
    Game game;
    game.Run();
    return 0;
}
```

Then run:
```bash
StratChessEvolved.exe perft
```

**Option B: Separate test project** (recommended for CI/CD)

Create a new Console Application project named "StratChessTests" and use `PerftRunner.cpp` as the main file.

### Step 3: Run Your First Test

```bash
# Run the test suite
StratChessEvolved.exe perft

# Or if using separate project:
StratChessTests.exe test
```

Expected output:
```
========================================
Running Perft Test Suite
========================================

Testing: Starting position
FEN: rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1
  Depth 1: ✓ PASS (20 nodes, 0 ms, 0 nps)
  Depth 2: ✓ PASS (400 nodes, 1 ms, 400000 nps)
  Depth 3: ✓ PASS (8902 nodes, 15 ms, 593466 nps)
  ...
```

## Troubleshooting

### Issue: Tests Fail on Starting Position

**Likely causes:**
1. Move generation missing some moves
2. Legal move filter too aggressive
3. DoMove/UndoMove not properly reversible

**Debug steps:**
```bash
# Use divide mode to see which moves are wrong
StratChessTests.exe divide 1
```

Compare output to expected:
```
a2a3: 1
a2a4: 1
b2b3: 1
b2b4: 1
...
Total nodes: 20
```

### Issue: Compilation Errors

**"Board::Instance() not found"**
- Make sure Board.h is included
- Check that Board singleton is still available
- If you've already refactored Board, update the test code to use your new API

**"GameInfo not found"**
- Add `#include "../Game.h"` or wherever GameInfo is defined

**"MoveGenerator not found"**
- Add `#include "../MoveGenerator.h"`

### Issue: Tests Pass but Seem Slow

**Performance expectations:**
- Depth 4 should complete in < 100ms on modern hardware
- Depth 5 should complete in < 3 seconds

**If slower:**
1. Make sure you're running in **Release mode**, not Debug
2. Check compiler optimizations are enabled
3. Profile with Visual Studio Profiler to find bottlenecks

## Next Steps

### 1. Implement FEN Parsing (High Priority)

Currently only the starting position works. You need FEN parsing to test other positions.

Add to Board or Game class:
```cpp
void SetupFromFEN(const std::string& fen);
```

Then update Perft.cpp to use it:
```cpp
// In run_test_suite():
board.SetupFromFEN(pos.fen);
```

### 2. Add More Test Positions

Find interesting positions:
- Tactical puzzles that failed
- Positions where engine played poorly
- Edge cases (triple checks, multiple en passant, etc.)

### 3. Integrate with CI/CD

```yaml
# Example GitHub Actions
- name: Run Perft Tests
  run: |
    ./build/StratChessTests.exe test
```

### 4. Performance Tracking

Create a script to track performance over time:
```bash
# perft_benchmark.sh
echo "$(date),$(./StratChessTests.exe run 5 | grep NPS | awk '{print $2}')" >> perft_history.csv
```

### 5. Migrate to Google Test (Optional but Recommended)

Install Google Test via vcpkg:
```bash
vcpkg install gtest
```

Replace TestFramework.h with:
```cpp
#include <gtest/gtest.h>

TEST(Perft, StartingPositionDepth3) {
    Board board;
    board.SetDefaultBoard();
    GameInfo info;
    auto result = Testing::Perft::run(board, info, 3, false);
    EXPECT_EQ(8902ull, result.nodes);
}
```

## FAQ

**Q: Why does depth 5 take so long?**

A: Perft grows exponentially. Depth 5 explores ~4.8 million positions. This is normal. Use it to benchmark optimizations.

**Q: Can I run perft in parallel?**

A: Yes! Once you implement parallel search, you can parallelize perft too. Each root move can be counted independently.

**Q: What if I get different node counts?**

A: This indicates a bug in move generation. Use divide mode to identify which moves are wrong, then debug that specific move type.

**Q: Should I run perft before every commit?**

A: At minimum:
- Run depth 4 before every commit (fast feedback)
- Run depth 5 before every release (more thorough)
- Full test suite in CI/CD

**Q: How deep should I test?**

A: 
- Development: depth 4 (fast, catches most bugs)
- Pre-commit: depth 5 (more thorough)
- Release: depth 6 (very thorough, but slow)

## Example Workflow

```bash
# 1. Make changes to MoveGenerator
vim MoveGenerator.cpp

# 2. Quick test (< 1 second)
./StratChessTests.exe run 4

# 3. If passes, run full suite (< 10 seconds)
./StratChessTests.exe test

# 4. If all pass, commit
git commit -m "Fix knight move generation on edge squares"

# 5. Before release, run deep test (optional)
./StratChessTests.exe run 6
```

## Support

If you encounter issues:
1. Check the README.md in the Tests directory
2. Enable verbose output in perft
3. Use divide mode to isolate the problem
4. Check [Chess Programming Wiki](https://www.chessprogramming.org/Perft)
