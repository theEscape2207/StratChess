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

- 500 games ≈ **±25 ELO** at 95% confidence with this engine's ~37% draw ratio (measured, see
  sanity rows — lower draw ratios mean noisier matches than the ±15 rule-of-thumb assumes);
  scale games ×4 to halve the error bound.
- **Single batches genuinely wander.** The two identical-build sanity batches landed at
  +25.1 and −27.9 — both at the edge of their own error bars, pooling to −1.4. Treat any
  single-batch result near the bound as unresolved: re-run and pool before acting on it.
- Candidate clearly negative after pooling (e.g. −30 ± 18 over 1000 games): regression —
  investigate before merging.
- Candidate within **±error of 0**: no measurable strength change (which is the *expected*
  result for pure refactors — byte-identical node-count validation is stronger for those).
- Losses on illegal move / disconnect / time stall are harness or engine **bugs**, never
  strength data — the script flags them, marks the row `FAILURES, discard`, and exits 1.

## Baseline (established 2026-07-03)

Identical builds (candidate exe byte-identical to the reference exe, SHA256-verified) over
2×500 games: pooled **−1.4 ELO** (378W/382L/240D, 49.80%) — zero measurable bias in the
instrument. The two batches individually hit opposite ±2σ edges, which calibrates the
per-batch noise above. Future search/eval changes measure against `elo-reference-v1` with
this procedure; anything beyond the pooled error bound is signal.

## Measurement history

| Date | Candidate | Reference | Games | TC | Elo diff | Notes |
|---|---|---|---|---|---|---|
| 2026-07-03 | candidate-de96f32+dirty | elo-reference-v1 | 20 | 10+0.1 | n/a | smoke — FAILURES, discard |
| 2026-07-03 | candidate-916c9bd+dirty | elo-reference-v1 | 20 | 10+0.1 | -17.39 +/- 104.93 | smoke |
| 2026-07-03 | candidate-a0aa30a | elo-reference-v1 | 500 | 10+0.1 | 25.06 +/- 25.54 | sanity batch 1 (identical builds) |
| 2026-07-03 | candidate-a0aa30a+dirty | elo-reference-v1 | 500 | 10+0.1 | -27.85 +/- 27.11 | sanity batch 2 (identical builds; +dirty = batch-1 log row only) |
| 2026-07-03 | candidate-a0aa30a (pooled) | elo-reference-v1 | 1000 | 10+0.1 | -1.4 (pooled batches 1+2) | **sanity baseline PASS** — 378W/382L/240D, 49.80% |
