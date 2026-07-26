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
| Concurrency | 6 concurrent games (default, `-Concurrency`) — sized off physical cores (12) ÷ 2 single-threaded engine processes per game, not the 24 logical/SMT threads; re-tune alongside the Machine row above if hardware changes |

## Resuming an interrupted match

If a match gets killed mid-run (check `logs\elo\<stamp>.log` for a trailing `Started game N`
with no matching `Finished game N` — or the process/log simply stops advancing), resume it instead
of restarting from scratch:

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -ResumeDir StratChessEvolved\logs\elo"
```

fastchess autosaves tournament state (`config.json`) every `-AutosaveInterval` games (default 20)
into `logs\elo\` — a single flat file shared by every invocation, not a per-match directory — and
`-ResumeDir` reloads it to restore the full original engine/tournament configuration.
`-CandidateExe`/`-ReferenceTag`/`-Games`/`-Tc`/etc. are all ignored when `-ResumeDir` is set. At
most `-AutosaveInterval` games get replayed (whatever completed since the last checkpoint before
the kill), not the whole batch. **Resume promptly** — because `config.json` is shared, starting any
other (non-resume) match first will overwrite it before you get the chance.

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
| 2026-07-03 | candidate-bda7189+dirty | elo-reference-v1 | 20 | 10+0.1 | 70.44 +/- 131.59 | smoke |
| 2026-07-23 | candidate-5bd0fda+dirty (Threads=4) | candidate-threads1 (Threads=1) | 500 | 10+0.1 | 128.55 +/- 28.36 | **Gate 3 PASS** — Lazy SMP threads=4 vs threads=1, same binary; LOS 100.00%, 286W/109L/105D (67.70%); row appended manually — the `Run-EloMatch.ps1` wrapper process was killed by the harness immediately after fastchess finished (before its own Add-Content step), verified against the raw match log |
| 2026-07-26 | candidate-5c4ef09+dirty (mop-up eval, #70) | elo-reference-v1 | 480 of 500 planned (491 actually finished, see note) | 10+0.1 | 15.94 +/- 27.62 | **PARTIAL — match killed mid-run, not a script/engine failure.** `Run-EloMatch.ps1`/fastchess were terminated by what all available evidence points to as a background-task duration cap in the execution tooling (~60 minutes elapsed between match start and last log write, to the second); no error, exception, or crash text in either the fastchess log or the redirected transcript, and 491/500 "Finished" lines are present with no illegal-move/disconnect/adjudication anomalies — this is a harness/environment stoppage, not a `FAILURES, discard` case. No final fastchess summary was ever printed (match ended between periodic reports), so the last **periodic** summary — printed at the 480-game mark — is recorded here rather than a true final tally: LOS 87.17%, DrawRatio 36.25%, 199W/177L/104D (52.29%), Ptnml(0-2) [32, 39, 87, 39, 43]. Per project-owner decision, accepted as the final measurement for this validation rather than re-running (re-running risks the same cap). Mop-up's effect is expected to be concentrated in rare pawnless decisive endgames largely absent from a standard-opening-book batch, so a result within error of 0 (as here: +15.94 ± 27.62, well inside its own error bar) is consistent with "no measurable change in typical play," the expected outcome, not a sign the feature failed |
