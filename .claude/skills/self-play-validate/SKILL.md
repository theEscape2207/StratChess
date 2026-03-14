---
name: self-play-validate
description: Run an AI vs AI self-play game to validate search behavior after changes
  to AIPerplex, evaluation, or move ordering. Use after any search-related change.
---

Run a headless AI vs AI game and verify expected log output.

## Setup

1. Ensure `StratChessEvolved/game_settings.json` has:
   - Both players set to `"type": 6` (AIPerplex vs AIPerplex)
   - FEN set to starting position (not a test FEN left over from a session)
2. Ensure `StratChessEvolved/logs/` subdirectory exists (spdlog silently fails if missing)

## Run

Execute from the **`StratChessEvolved/` directory** — both `game_settings.json` and log
output are resolved relative to the working directory:

```powershell
$proc = Start-Process ..\x64\Release\StratChessEvolved.exe `
  -PassThru -NoNewWindow -RedirectStandardOutput out.txt
$proc.WaitForExit(30000)   # 30s timeout
if (-not $proc.HasExited) { $proc.Kill() }
Get-Content out.txt
```

## Verify

Each move should log:
```
GetMove complete: move=..., depth=..., time=...ms, nodes=..., stable=...
```
Game should terminate on checkmate or stalemate (no infinite loop, no crash, no
missing move logs).

## Notes
- `game_settings.json` uses C-style `/* */` comments — do NOT parse with PowerShell
  `ConvertFrom-Json` (it rejects comments); read/edit as plain text
- Reset FEN to starting position before committing
- For changes to PlayerAI/PlayerBase base classes, also run AIAgent self-play
  (`"type": 3` for both sides) to verify the inheritance chain
