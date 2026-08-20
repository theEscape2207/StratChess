# #348 Stage 2 — implementation progress

Design: `.claude/plans/gameinfo-position-record-split.md`.
**Status: design written, awaiting cross-agent design review. No implementation started.**

Stage 1 landed in PR #357. Its design doc and progress file are deleted by this branch — the Harvest
was complete, which is the lifecycle's condition for removing them. Git history keeps both.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `PositionState` added and dual-written alongside the four arrays, with the D8 unmake assert | not started |
| 2 | `Compare-SearchEquivalence` + Debug suite against the dual-write build | not started |
| 3 | The four `MAX_PLY` arrays deleted; the D8 assert deleted with them | not started |
| 4 | Outcome moved off `Board` — `ThreadData::root_state`, `PlayerAiBase::root_state_`, `SetGameState` gone (D5) | not started |
| 5 | Per-call reset of the root verdict, plus its falsified regression test (D6) | not started |
| 6 | `GameInfo` deleted; 59 `GetGameInfo()` sites migrated; `fullmove_count()` added (D7) | not started |
| 7 | `FiftyMoveRuleTests.cpp` rewritten against `Board` (D7) | not started |
| 8 | FEN clamp for the narrowed clock/fullmove fields, plus its test (D4) | not started |
| 9 | Validation sweep — equivalence, perft, bench, self-play ×2, `search-reviewer` | not started |
| 10 | Harvest — `CLAUDE.md`, source comments, `Docs/Changelog.md`, `Docs/TestDesign.md`, issue comments | not started |

Steps 1–3 are ordered by D8: the assert only means something while both representations exist.
Step 5 depends on step 4.

## Evidence

*(Filled in as steps complete. Stage 1's table is the template: one row per check, with the number,
not the intention.)*

| Check | Result |
|---|---|
| `Compare-SearchEquivalence -BaselineRef origin/main` | — |
| `Run-PerftCheck.ps1` | — |
| `Run-Bench.ps1` before/after | — |
| Release / Debug suites | — |
| Self-play `type 6` / `type 3` | — |
| `search-reviewer` | — |
| CI | — |
| Cross-agent design review | pending |

## Open questions for the design review

- D4's clamp touches `FENParser` to buy eight bytes per ply. D2's 32-byte fallback avoids it. Is the
  clamp worth it, given no legal game can reach either cap?
- D6 is the one non-bit-identical change in the PR. Confirm it belongs here rather than as its own
  fix ahead of the restructure.
- D8's dual-write commit doubles the make/unmake write path for one commit. Confirm the game-mode and
  legacy-agent coverage it buys is worth that over relying on `Compare-SearchEquivalence` alone.

## Post-merge follow-ups

- Correct #292: the `eSquare : uint8_t` change was **never reverted** — it was validated locally and
  parked pending #292 itself. The issue's "reverted for unrecorded reasons" wording sends the next
  reader chasing a ghost. Re-scope #292 to the 16-byte step with this PR's bench as its input.
- Comment on #347 with where the halfmove clock now lives, which is the dependency it records.
- #355 and #356 close with this PR.
- `Remove-Worktree.ps1 -Name glistening-stargazing-scone -SyncMaster -FromInside`.

## Traps carried over from Stage 1

- **clang-cl does not flag an unused local that Linux clang-tidy rejects.** Six survived the whole
  local gate in Stage 1 and failed CI's `lint-linux`. Expect one CI round-trip after a wide signature
  change — and this change touches 59 call sites.
- `Run-Lint.ps1 -Check BlameIgnore` fails any commit touching 20+ sources; `-AllowUnlistedReformat`
  forwards through `Validate-PrePR.ps1` and `New-PullRequest.ps1`.
- Piping a UCI command file into the exe silently searches the wrong position — `go` is async, so
  every later `position` is refused. Use a driver that waits for `bestmove`.
- `Run-Bench.ps1` reports via `Write-Host`, so an in-process `& $script` captures nothing. Invoke it
  as a child `pwsh -File` to parse its output.
