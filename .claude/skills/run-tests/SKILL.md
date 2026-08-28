---
name: run-tests
description: Build and run StratChessTests with optional Catch2 tag filter. Use when the
  user asks to run tests, verify a change, or check test results.
---

Build the test project and run with an optional Catch2 tag filter.

## Canonical invocation

No tag — full fast suite (excludes [slow]):
```
pwsh -ExecutionPolicy Bypass -File <abs>\StratChessEvolved\Scripts\Run-Tests.ps1
```

With tag filter:
```
pwsh -ExecutionPolicy Bypass -File <abs>\StratChessEvolved\Scripts\Run-Tests.ps1 [tactical]
```

Absolute path, and **never** wrap it in `cmd.exe /c "..."` — that form swallows script output in
some shells, so a failing run looks like a silent no-op. Use the copy in your **own** worktree; the
scripts target the repo of their own `$PSScriptRoot`.

## Notes
- Run from worktree root (not a subdirectory)
- Available tags: [sort] [search] [tactical] [perft] [tt] [eval] [repetition] [formatter] [board] [time_mgr]
- Binary is under `build\<preset>\` — the script handles this; `Scripts\Get-BuildArtifact.ps1`
  resolves the path if you need it directly
- Fallback if Scripts/ unavailable: `pwsh -ExecutionPolicy Bypass -File <abs>\build.ps1 run-tests`
