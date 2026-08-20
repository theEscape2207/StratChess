# #348 Stage 2 — implementation progress

Design: `.claude/plans/gameinfo-position-record-split.md`.
**Status: design review rounds 1 and 2 addressed. No implementation started.**

Stage 1 landed in PR #357. Its design doc and progress file are deleted by this branch — the Harvest
was complete, which is the lifecycle's condition for removing them. Git history keeps both.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `PositionState` added and dual-written alongside the four arrays, with the D8 oracle at all four points — after `DoMove`, after `DoNullMove`, on the rollback path, after both unmakes | not started |
| 2 | `Compare-SearchEquivalence` + Debug suite against the dual-write build | not started |
| 3 | The four `MAX_PLY` arrays deleted; the D8 oracle deleted with them | not started |
| 4 | Outcome moved off `Board` — `ThreadData::root_game_state`, `PlayerAiBase::root_game_state_`, `SetGameState` gone (D5) | not started |
| 5 | Per-call reset of the root verdict, plus one falsified regression test **per carrier** — AIPerplex and AIAgent (D6) | not started |
| 6 | `GameInfo` deleted; 59 `GetGameInfo()` sites migrated; `fullmove_count()` added (D7) | not started |
| 7 | `FiftyMoveRuleTests.cpp` rewritten against `Board` (D7) | not started |
| 8 | D4's three input bounds (both FEN counters, the UCI move list) + Debug asserts at the increments, plus one bound test per input | not started |
| 9 | Validation sweep — equivalence, perft, bench, self-play ×2, `search-reviewer` | not started |
| 10 | Harvest — `CLAUDE.md:153` corrected, `Docs/TestDesign.md` row 78 renamed, `Move.h:16` + `MoveFactory.h:8` repointed, source comments, `Docs/Changelog.md`, issue comments | not started |

Steps 1–3 are ordered by D8: the oracle only means something while both representations exist.
Step 5 depends on step 4, and stays its own commit so step 2's equivalence run covers everything else.

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
| Cross-agent design review | rounds 1–2 addressed (8 findings, above) |

## Design review round 1

Three open questions were put to the reviewer and all three came back confirmed: keep the 24-byte
target and the narrowing, keep D6 in this PR as its own commit, keep D8 and widen it. Five findings:

| Finding | Resolution |
|---|---|
| **P1** — clamping at the parser does not stop the narrowed counters wrapping on the next increment; a quiet move from 65,535 resets the clock to 0 and bypasses draw detection | The wrap is real; saturation was the wrong fix (see below). D4 now bounds the three *inputs* — both FEN counters and the UCI move list — rejecting anything past them with a diagnostic, and guards the increments with a Debug assert only. Round 2 set the bounds themselves |
| **P2** — D8's oracle compared only at unmake, so a broken *forward* update would be written into both representations, restore perfectly and pass | D8 now fires at four points: after `DoMove`, after `DoNullMove`, on `DoMove`'s illegal-move rollback path, and after both unmakes |
| **P2** — D6 introduces two independent verdict carriers but proposed one test | One test per carrier, AIPerplex and AIAgent. D5 now states *why* there are two: a single player-level member would be written by every Lazy SMP helper at its own ply 0, which is #358's race |
| `root_state` is ambiguous beside `PositionState` and `SearchState` | Renamed `root_game_state` / `root_game_state_`, after the `SearchResult::game_state` field it feeds |
| `static_assert(sizeof == 24)` guarantees the footprint, not the stated offsets | Wording softened — the claim is *no padding*, which is exactly what the assert proves. Offsets are a consequence; nothing reads the record by offset |

**P1's first fix was wrong, and both reviewer and implementer had it wrong the same way.** Saturating
every increment spends a per-node compare to make a 65,535-move game — an engine bug two hundred times
past anything real — report a plausible number instead of failing loudly. The threat model lists "a
silently wrong answer" as a failure mode alongside a crash, and there is no attacker to harden
against, so absorbing the runaway is the worse outcome, not the safe one.

`Board::DoMove` already answers this for the analogous bound, ten lines from where the counters live:
`assert(currentPly_ < MAX_PLY)` with a "defense in depth" comment, and no Release check. D4 now
follows that precedent, and splits the question by kind of event: **malformed input** is rejected with
a diagnostic; **an impossible runtime state** is a Debug assert. Net Release cost of D4: zero.

Bounding the inputs instead was the repo owner's suggestion, not the reviewer's and not the
implementer's — recorded because the review table above would otherwise imply the design got there on
its own. The bounds cover the two FEN
counters and, crucially, the `position … moves` list, which was the one genuinely unbounded path into
the counters. Together they mean nothing a caller can send reaches the field maximum, which is what
makes the assert an assert rather than a compromise. A scan of the whole tree for six-field FENs found
a maximum halfmove clock of 120 and fullmove counter of 60, so nothing committed is affected; that
retires the "grep the corpora first" precondition from the earlier draft.

## Design review round 2

| Finding | Resolution |
|---|---|
| The 1000 bounds were guesses. Fullmove's theoretical maximum is 5898.5 moves; halfmove wants 150 to leave room for the 75-move FIDE rule and to clear the corpus maximum of 120; the move list is the same limit in plies | Bounds are now **150 / 5899 / 11,797 plies**, each taken from the game rather than from observed usage, with the source URL carried into the constants. Worst counter reachable through UCI is 11,947 — still 5.5× inside the field maximum |
| **P2** — D4's numeric table and the round-trip invariant contradicted the parser: `FENParser.cpp:75`'s regex is `\s+\d+`, so negatives die at the regex and are never clamped, while fullmove `0` *is* accepted and repaired to 1 | Both verified against the source. The policy table is rewritten with zero and negative split out per counter, the round-trip invariant is narrowed to the parser's normalised domain (halfmove 0…150, fullmove 1…5899), and fullmove `0` gets its own test. `std::max(0, half)` is now noted as dead code rather than read as evidence |
| **P2** — nothing stopped the input cap silently becoming a runtime invariant | New Debug test: load fullmove 5899 with Black to move, make a quiet move, confirm it reaches 5900 without asserting, undo, confirm 5899. D4 now states explicitly that the bounds govern *input* while the assert guards the *field maximum*, and why conflating the two is the easy mistake |

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
