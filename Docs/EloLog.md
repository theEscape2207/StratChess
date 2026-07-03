# ELO Measurement Log

Differential strength measurement against a pinned reference build. Run via:

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1"
```

Build the candidate first (`.\build.ps1 main`). Use `-Smoke` for a 20-game pipeline check.
Plan/design: `.claude/plans/elo-baseline-measurement.md`.

## Measurement setup (pinned — changing any of these starts a new setup record)

| Component | Value |
|---|---|
| Match runner | fastchess **v1.8.0-alpha** (`fastchess alpha 1.8.0`, windows-x86-64), from https://github.com/Disservin/fastchess/releases/tag/v1.8.0-alpha |
| Runner location | `<DepsRoot>EngineTesting\fastchess.exe` (repo sibling, same convention as spdlog/json/Catch2) |
| Opening book | `Tests/openings/openings-250.pgn` — first 250 games of `8moves_v3.pgn` (official-stockfish/books), sequential order, each pair color-swapped (`-repeat`) |
| Reference build | git tag `elo-reference-v1` (post-ThreadData + UCI replay-overflow fix — last deterministic engine before Lazy SMP). Cached as `<DepsRoot>EngineTesting\StratChess-elo-reference-v1.exe`; rebuilt from the tag automatically on cache miss. Originally pinned at `da06b3c`, re-pointed after the smoke match exposed a >256-ply replay crash (`da06b3c` engines can crash mid-match, poisoning results) — see history row 1 |
| Time control | 10s + 0.1s increment |
| Adjudication | draw: movenumber=40 movecount=8 score=10; resign: movecount=4 score=800 |
| Machine | Windows 11 Pro x64 (theEscape2207 dev machine) — results are machine-relative; re-establish the sanity row when measuring on different hardware |

## Interpreting results

- 500 games ≈ **±15 ELO** at 95% confidence; scale games ×4 to halve the error bound.
- Candidate at **−20 ± 15**: likely regression — re-run with doubled games before reverting.
- Candidate within **±error of 0**: no measurable strength change (which is the *expected*
  result for pure refactors — byte-identical node-count validation is stronger for those).
- Losses on illegal move / disconnect / time stall are harness or engine **bugs**, never
  strength data — the script flags them, marks the row `FAILURES, discard`, and exits 1.

## Measurement history

| Date | Candidate | Reference | Games | TC | Elo diff | Notes |
|---|---|---|---|---|---|---|
| 2026-07-03 | candidate-de96f32+dirty | elo-reference-v1 | 20 | 10+0.1 | n/a | smoke — FAILURES, discard |
