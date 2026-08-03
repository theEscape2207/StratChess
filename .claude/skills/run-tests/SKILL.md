---
name: run-tests
description: Build and run StratChessTests with optional Catch2 tag filter. Use when the
  user asks to run tests, verify a change, or check test results.
---

Build the test project and run with an optional Catch2 tag filter.

## Canonical invocation (works from bash, cmd, or PowerShell)

No tag — full fast suite (excludes [slow]):
```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"
```

With tag filter:
```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [tactical]"
```

## Notes
- Run from worktree root (not a subdirectory)
- Available tags: [sort] [search] [tactical] [perft] [tt] [eval] [repetition] [formatter] [board] [time_mgr]
- Binary is under `build\<preset>\` — the script handles this; `Scripts\Get-BuildArtifact.ps1`
  resolves the path if you need it directly
- Fallback if Scripts/ unavailable: `cmd.exe /c "pwsh -ExecutionPolicy Bypass -File build.ps1 run-tests"`
