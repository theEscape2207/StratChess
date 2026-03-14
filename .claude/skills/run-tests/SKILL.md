---
name: run-tests
description: Build and run StratChessTests with optional Catch2 tag filter. Use when the
  user asks to run tests, verify a change, or check test results.
---

Build the test project, then run with an optional tag filter.

## Steps

1. Build tests:
   ```
   .\build.ps1 tests
   ```

2. Run all tests (no filter):
   ```
   StratChessTests\x64\Release\StratChessTests.exe
   ```
   Or with a tag filter (e.g. [sort], [search], [tactical], [perft], [tt], [eval],
   [repetition], [formatter], [board], [time_mgr]):
   ```
   StratChessTests\x64\Release\StratChessTests.exe [tag]
   ```

3. Report: pass/fail count, any assertion failures, total assertions.

## Notes
- Run from worktree root (not StratChessTests/ subdirectory)
- Binary is under `StratChessTests\x64\Release\` (not `x64\Release\`)
- Build with `/v:normal` instead of `/v:minimal` when diagnosing build errors
- Only x64 builds work — Win32/x86 not maintained
