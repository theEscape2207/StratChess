# #348 Stage 1 — implementation progress

Design: `.claude/plans/gameinfo-board-single-authority.md`.
**PR #357 is open. Cross-agent review round 1 is addressed; awaiting re-review — not merged.**

All eleven steps are done: the `Board` accessors and movegen change, `DoMove` writing `lastMove`,
both `GameInfo` sequences deleted, the FEN cleanup, `GetMove` returning a `SearchResult`, `Game::Run`
adjudicating the fifty-move rule, the `Game` test seam, validation, and the harvest.

## Evidence

| Check | Result |
|---|---|
| `Compare-SearchEquivalence -BaselineRef origin/main` | IDENTICAL, 90 lines, 6 positions, depth 12 — re-run after each risky step |
| `Run-PerftCheck.ps1` | 561,641 checks / 561,568 passed / 73 failures — matches the documented 2026-08-05 sweep exactly |
| `Run-Bench.ps1`, 4 alternating runs/side | 2,981,112 → 3,071,809 nps (+3.0%), node counts identical |
| Release / Debug suites | 7140 / 454 and 7136 / 453, pass |
| `search-reviewer` | LGTM, no correctness findings; 4 of 6 notes fixed |
| CI | 9/9 green |
| Cross-agent review round 1 | 4 findings, all addressed (below) |

## Cross-agent review round 1

| Finding | Resolution |
|---|---|
| P2 — `Game` printed `GetBestScore()`, stale on an ordinary search | `PrintStateMessage` takes the mover's `SearchResult`; two `[game]` tests, falsified against the old channel |
| P2 — no end-to-end coverage of terminal-result *production* | `[search]` cases on mate/stalemate/still-playing roots, plus `PlayerHumanTests.cpp` (`[player_human]`) for the non-interactive terminal paths |
| P3 — UCI discarded the returned result and re-read `AIPerplex::GetLastResult()` | The returned `SearchResult` is retained and drives every `info` line and `bestmove` |
| P3 — stale harvest in `Docs/Engine-Readme.md` | `ThreadData` no longer claims a `GameInfo` sequence; `SearchResult` is documented at its real location with `game_state` and `qnodes_searched` |

## Next steps

1. **Cross-agent review of PR #357**, then merge (user routes it, and merges via the web UI).
2. After merge: `Remove-Worktree.ps1 -Name playful-hatching-castle -SyncMaster -FromInside`.
3. Post-merge follow-ups, all recorded in the PR body:
   - File an issue for the pre-existing `_bestScore` data race the reviewer found
     (`adjustScoreForGameState` writes a player-level member at ply 0, reachable from helper threads).
   - Comment on #292 that it is re-scoped: the per-node copy is gone, `gameInfoHistory_` remains.
   - #308 closes with this PR.
4. **Stage 2 of #348 stays open** — splitting `GameInfo` into a reversible-position record and an
   outcome type, and merging it with the three parallel `MAX_PLY` undo arrays.

## Traps worth remembering

- **clang-cl does not flag an unused `GameInfo` local that Linux clang-tidy rejects.** Six survived
  the whole local gate — full build, `/W4 /WX`, clang-tidy Gate — and failed CI's `lint-linux`. The
  local gate cannot catch this class; expect one CI round-trip after a wide signature change.
- `Run-Lint.ps1 -Check BlameIgnore` fails any commit touching 20+ sources. Its `-AllowUnlistedReformat`
  escape hatch now forwards through `Validate-PrePR.ps1` and `New-PullRequest.ps1` (it did not before,
  which blocked the gate outright for a wide code change).
- Piping a UCI command file into the exe silently searches the wrong position — `go` is async, so every
  later `position` is refused and the engine answers for startpos. Use a driver that waits for
  `bestmove` (scratchpad `uci_drive.py`).
- `Run-Bench.ps1` reports via `Write-Host`, so an in-process `& $script` captures nothing. Invoke it as
  a child `pwsh -File` to parse its output.
