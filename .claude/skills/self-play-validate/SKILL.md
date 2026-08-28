---
name: self-play-validate
description: Run an AI vs AI self-play game to validate search behavior after changes
  to AIPerplex, evaluation, or move ordering. Use after any search-related change.
---

Run full pre-PR validation: parallel build, extended tests, and AIPerplex vs AIPerplex self-play.

## Canonical invocation

```
pwsh -ExecutionPolicy Bypass -File <abs>\StratChessEvolved\Scripts\Validate-PrePR.ps1
```

Absolute path, and **never** wrap it in `cmd.exe /c "..."` — that form swallows script output in
some shells, so a failing run looks like a silent no-op. Use the copy in your **own** worktree; the
scripts target the repo of their own `$PSScriptRoot`.

## What it checks
1. Full build — main solution + test project in parallel
2. Extended test suite — all tiers including [slow]
3. Self-play — AIPerplex vs AIPerplex, 60s timeout; verifies `GetMove complete:` per move (≥2)

## Notes
- Run `Validate-PreCommit.ps1` first (FEN check and fast tests live there)
- `game_settings.json` must have `"type": 6` for both players before running
- For PlayerAI/PlayerBase base class changes, also verify AIAgent self-play manually (`"type": 3` for both sides)
- Summary table always printed even if earlier checks fail
