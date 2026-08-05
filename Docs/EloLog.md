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
| Opening book | Resolved by `Run-EloMatch.ps1`: `-Book <path>` if given, else `EngineTesting\openings-large.pgn|.epd` if present, else the committed `Tests/openings/openings-250.pgn` — first 250 games of `8moves_v3.pgn` (official-stockfish/books), sequential order, each pair color-swapped (`-repeat`). **250 openings = 500 distinct games**; see "Book size" below |
| Reference build | git tag **`elo-reference-v2`** (`df9245f`, 2026-08-03 — first baseline built by clang-cl/CMake, matching what ships). Cached as `EngineTesting\StratChess-elo-reference-v2.exe`; rebuilt from the tag automatically on cache miss. **`elo-reference-v1` is retained, not replaced** — see "Two anchors" below |
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

## Choosing SPRT vs a fixed batch

A fixed batch answers *"how big is the difference?"*. SPRT answers *"is this worth keeping?"* — and
stops as soon as the evidence is decisive instead of always playing the full game count.

**Use a fixed batch** when a point estimate with an error bound is what you want: baselines, sanity
runs, and large expected effects (e.g. the Lazy SMP threads=4 row at +128.55 ± 28.36).

**Use SPRT** when the question is accept/reject and the effect is expected to be small — which is
most of epic #110. This matters more than it sounds:

> 500 games resolves ±25 Elo. Bishop pair, connected rooks, castling-done, outposts and queen
> activity are each worth roughly 5–20 Elo. **A fixed batch cannot distinguish any of them from
> zero.** The mop-up row below (`+15.94 ± 27.62`) is exactly this: a result equally consistent with
> "+16 Elo", "no change", and "−10 Elo".

```
# "prove it did not make things worse" — refactors, restructures, anything expected neutral
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -Sprt NonRegression"

# "prove it is worth >= ~10 Elo" — small new eval terms
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -Sprt Gain"

# explicit bounds
... -Sprt Custom -Elo0 0 -Elo1 5
```

| Preset | Bounds | Question it answers |
|---|---|---|
| `NonRegression` | elo0=−5, elo1=0 | Did this hurt? |
| `Gain` | elo0=0, elo1=10 | Is this worth ≥ ~10 Elo? |
| `Custom` | `-Elo0` / `-Elo1` (both required) | Anything else |

Notes:

- **`-Games` becomes an upper bound**, not a target — the match stops early on a decision, or at the
  cap if it never reaches one.
- **Bounds are in logistic Elo**, the same scale as the `Elo:` line and everything in the history
  table below. The script pins `model=logistic` for this reason; fastchess's own default is
  `normalized` (nElo), a different scale on which `elo1=10` would mean something else entirely.
- `-Sprt` cannot be combined with `-Smoke` (a 20-game run can never reach a decision).
- Resume (`-ResumeDir`) works normally and preserves the original SPRT bounds — and matters *more*
  here, since a sequential test can run longer than a fixed batch. It recovers an **interrupted**
  match only; it cannot extend one that already finished at its `-Games` cap (see
  "Adjusting `-Games`" below).
- **Record the verdict, not just the Elo.** The Notes column carries
  `SPRT <preset> [elo0, elo1] — H1 accepted` / `H0 accepted` / `inconclusive @ N games`, written
  automatically by the script. An `H1 accepted` at 300 games is a *stronger* claim than a fixed
  500-game point estimate — do not read it as though it were weaker.
- **Inconclusive is a real result**, meaning "smaller than elo1, or we ran out of budget". It is not
  a failure and not a measurement of zero. The script flags it in yellow rather than letting it pass
  as a decision.

Verified against the pinned fastchess 1.8.0 build: an SPRT run with bounds `[0, 200]` between two
identical builds accepted H0 after 10 games rather than playing the 200-game cap.

### Book size, and why 500 games is a cliff

An opening pair is two games, so **N openings yield 2N distinct games**. The committed book holds 250
openings, so a 500-game batch consumes it **exactly**. Every game past that replays an opening
already played, which narrows the reported error bar without adding information to it — the run looks
more precise than it is.

This matters most where it is least visible: SPRT runs take `-Games` as an upper bound and routinely
ask for thousands. `Run-EloMatch.ps1` now prints the book and its opening count on every run, and
warns when `-Games` exceeds the distinct-game count.

**To run bigger batches honestly**, drop a large book beside the checkout as
`EngineTesting\openings-large.pgn` (or `.epd` — the format flag follows the extension). The natural
choice is the full `8moves_v3.pgn` from official-stockfish/books, the same source the committed
250-game book was cut from. It is deliberately **not committed**: it is third-party data of varying
provenance and this repository is public, so it lives with fastchess and the reference binaries,
which is where every other external test asset already lives.

Rows measured on the 250-opening book are not directly comparable to rows measured on a larger one —
opening selection changes the draw rate, and the draw rate changes the error bar. Note the book
alongside any row where it is not the committed default.

### Adjusting `-Games` (and when not to)

`-Games 500` means two different things depending on mode, and conflating them is the trap:

- **Fixed batch — 500 *is* the measurement.** Error scales as 1/√N, so it is an expensive dial:
  ±25 Elo at 500 → ±12.5 at 2 000 → ±6 at 8 000. Raise it only when a point estimate is the
  deliverable (a new reference baseline, or fitting data for #117), not to make a verdict "more
  certain" — that is what SPRT is for.
- **SPRT — 500 is only the give-up point.** It has no bearing on the answer's quality; the test
  stops as soon as the evidence is decisive. Raising it is statistically free, and costs wall-clock
  only in the runs that would otherwise have returned inconclusive. Note the 500 default was chosen
  as a *fixed-batch resolution target* and is simply inherited as the SPRT cap — there is no
  statistical reason for those two numbers to be equal.

**Reach for wider bounds before more games.** Expected sample size scales roughly with the inverse
square of the indifference region's width, so `-Sprt Custom -Elo0 -10 -Elo1 0` costs about **4×
fewer games** than `NonRegression`'s `[-5, 0]` — usually a better trade than quadrupling the budget.
Ask the loosest question that still settles the decision.

**Estimating what an inconclusive run would have needed.** LLR accumulates roughly linearly in N
(in expectation, when the true effect lies outside the indifference region), so
`games_needed ≈ N × 2.94 / LLR_at_N`. The #126 row below reached LLR 0.76 at 500 games → ~1 900
games, ~2.5 h. Treat this as order-of-magnitude only: LLR is a random walk, and if the true effect
sits *inside* the indifference region the test may not converge at any practical N.

**The binding ceiling is operational, not statistical.** A full 500-game batch takes ≈40 min at the
default `-Concurrency 6`, and the 2026-07-26 mop-up row below was killed at ~60 min by a
background-task duration cap in the execution tooling. That puts the practical limit for a
*background-launched* match at roughly **700–750 games**; past that, run it in the foreground or
expect to resume. Two related traps:

- `-ResumeDir` **cannot extend a completed capped run.** It restores the original `-Games` from the
  saved `config.json`, so it recovers an *interrupted* match but will not top up a finished one.
  "Run 500, then add more" is not available — restart at the higher number.
- **Do not raise `-Concurrency` to buy throughput.** It is pinned to physical cores deliberately;
  oversubscribing injects timing noise, or genuine time losses, into a fixed real-time control.

**Lowering `-Games`** is only useful for `-Smoke` pipeline checks. Under SPRT it is actively
counterproductive: an early decision costs nothing, so a low cap buys nothing and risks an
avoidable inconclusive.

**Before spending 4× the games, consider buying information *per* game instead** — a sharper
opening book yields more decisive pairs (the #126 run drew 35.6%), and pentanomial scoring (the
`Ptnml(0-2)` line) is already in use as the other main variance reduction.

### When even SPRT cannot resolve a term

Some terms are too small to decide at any practical game count. The fallback is to **bundle several
into one PR and measure them jointly**. That is a deliberate measurement decision, not sloppy
scoping — but say so explicitly in the row's Notes, so a later reader does not attribute the whole
delta to whichever term the commit message happens to mention first.

## Baseline (established 2026-07-03)

Identical builds (candidate exe byte-identical to the reference exe, SHA256-verified) over
2×500 games: pooled **−1.4 ELO** (378W/382L/240D, 49.80%) — zero measurable bias in the
instrument. The two batches individually hit opposite ±2σ edges, which calibrates the
per-batch noise above. Future search/eval changes measure against the pinned reference with
this procedure; anything beyond the pooled error bound is signal. (That calibration was run
against `elo-reference-v1`; it measures the *instrument*, not the anchor, so it carries over to
`elo-reference-v2` unchanged.)

### Two anchors, and which to use

There are two pinned references, and picking the wrong one produces a number that looks real and
is not.

| Tag | Commit | Compiler | Use for |
|---|---|---|---|
| `elo-reference-v2` | `df9245f`, 2026-08-03 | clang-cl + ThinLTO (ships) | **Default.** Day-to-day search/eval changes |
| `elo-reference-v1` | `fd8b665`, 2026-07-03 | MSVC + LTCG | The long-run epic comparison only |

**Why v2 exists.** The shipping compiler changed to clang-cl when #177 merged, and that change alone
is worth roughly +40 Elo at 10+0.1 (measured in #84). Measuring a clang-built candidate against the
MSVC-built v1 credits that +40 to whatever change is under test — a phantom gain large enough to
make an eval regression look like an improvement. v2 removes it by being built the same way the
candidate is.

**Why v1 is kept.** It is deliberately preserved as the long-run anchor for the eval (#110) and build
modernization (#81) epics: a single before/after across both, where the compiler gain is *part of*
what is being measured rather than a confound. Do not delete
`EngineTesting\StratChess-elo-reference-v1.exe` or the tag. Tracked in #180.

Run it explicitly when that time comes:

```
... -File StratChessEvolved\Scripts\Run-EloMatch.ps1 -ReferenceTag elo-reference-v1
```

Note the rebuild path: v1 predates the CMake migration, so rebuilding it from its tag uses that
tag's own MSBuild `build.ps1` and needs the sibling spdlog/nlohmann checkouts to still exist.
`Run-EloMatch.ps1` warns when it rebuilds a pre-migration reference, but the warning only fires on a
cache *miss* — a cached v1 binary is used silently.

### The anchor answers "where do we stand", not "did this change help"

`elo-reference-v1` is a **fixed** anchor, so every row measures cumulative progress since it — the
engine's standing, not the PR's delta. That is the right instrument for tracking the project, and
the wrong one for deciding whether one small change earned its place, because:

- **An SPRT against the anchor tests the sum.** H1 means "`main` + this change beats the anchor by
  more than `elo1`", which can be true on `main`'s pre-existing margin alone. A verdict on a sum
  licenses no claim about one addend. See the 2026-07-29 `candidate-08d4ef8` row, where H1 was
  accepted in 23 minutes on a comparison that says nothing about the three terms it was run for.
- **The delta cannot be recovered by subtraction.** Differencing two anchor rows compounds their
  errors, so the result is less constrained than either input — and most rows here are individually
  inconclusive to begin with.

**So: to decide whether a change helps, measure it against `main`** — build the merge-base and pass
it via `-ReferenceExe`, with `-ReferenceTag` naming the commit. Keep the anchor run too when the
cumulative figure is wanted; they answer different questions and both belong in the log, clearly
labelled as to which is which. This tension is inherent to a fixed anchor and is tracked separately;
it is not a defect in any individual row.

## The Linux CI ledger is a separate instrument

`.github/workflows/strength.yml` (`workflow_dispatch` only) plays the same kind of match on a GitHub
runner: both sides built from source in one job by GCC on `ubuntu-24.04`, Release, `Threads=1`,
identical adjudication settings to `Run-EloMatch.ps1`.

**Its rows never go in the table below, and are never compared with one.** The binaries come from a
different compiler on different hardware, which is the one comparison this whole document exists to
prevent (issue #84 measured the clang-cl/MSVC gap alone at roughly +40 Elo). Ratios transfer between
the two ledgers; absolute values do not.

Two differences from the local setup, both deliberate:

- **Book.** `UHO_4060_v3.epd` (242,201 openings — the 4060 names the evaluation band the positions
  were selected from, not their count), downloaded per run from a pinned commit of
  `official-stockfish/books`. Not committed, for the reason M3 records in
  `.claude/plans/public-repo-and-strength-lab.md`.
- **Fixed N, no SPRT.** SPRT is a sequential test and does not shard across runners; the CI
  instrument buys resolution with games instead, since minutes there are free.

**Increments below 0.1 s forfeit.** `compute_budget()` floors a move at 100 ms, so at 5+0.05 the
engine loses ground every move once its clock drains — measured at 3 time losses in 4 games, against
none at 5+0.1. A handicap run must halve the base time and leave the increment alone. The workflow
warns when either side is given an increment under 0.1 s.

### Linux CI measurement history

| Date | Candidate | Reference | Games | TC | Elo diff | Notes |
|---|---|---|---|---|---|---|
| — | — | — | — | — | — | Awaiting the M4 calibration runs (null test and known-sign control); nothing may be recorded here before they pass |

## Measurement history

| Date | Candidate | Reference | Games | TC | Elo diff | Notes |
|---|---|---|---|---|---|---|
| 2026-07-03 | candidate-6e96e89+dirty | elo-reference-v1 | 20 | 10+0.1 | n/a | smoke — FAILURES, discard |
| 2026-07-03 | candidate-b4cd8b5+dirty | elo-reference-v1 | 20 | 10+0.1 | -17.39 +/- 104.93 | smoke |
| 2026-07-03 | candidate-1c05192 | elo-reference-v1 | 500 | 10+0.1 | 25.06 +/- 25.54 | sanity batch 1 (identical builds) |
| 2026-07-03 | candidate-1c05192+dirty | elo-reference-v1 | 500 | 10+0.1 | -27.85 +/- 27.11 | sanity batch 2 (identical builds; +dirty = batch-1 log row only) |
| 2026-07-03 | candidate-1c05192 (pooled) | elo-reference-v1 | 1000 | 10+0.1 | -1.4 (pooled batches 1+2) | **sanity baseline PASS** — 378W/382L/240D, 49.80% |
| 2026-07-03 | candidate-fd8b665+dirty | elo-reference-v1 | 20 | 10+0.1 | 70.44 +/- 131.59 | smoke |
| 2026-07-23 | candidate-1a2a97c+dirty (Threads=4) | candidate-threads1 (Threads=1) | 500 | 10+0.1 | 128.55 +/- 28.36 | **Gate 3 PASS** — Lazy SMP threads=4 vs threads=1, same binary; LOS 100.00%, 286W/109L/105D (67.70%); row appended manually — the `Run-EloMatch.ps1` wrapper process was killed by the harness immediately after fastchess finished (before its own Add-Content step), verified against the raw match log |
| 2026-07-26 | candidate-1cfc32c+dirty (mop-up eval, #70) | elo-reference-v1 | 480 of 500 planned (491 actually finished, see note) | 10+0.1 | 15.94 +/- 27.62 | **PARTIAL — match killed mid-run, not a script/engine failure.** `Run-EloMatch.ps1`/fastchess were terminated by what all available evidence points to as a background-task duration cap in the execution tooling (~60 minutes elapsed between match start and last log write, to the second); no error, exception, or crash text in either the fastchess log or the redirected transcript, and 491/500 "Finished" lines are present with no illegal-move/disconnect/adjudication anomalies — this is a harness/environment stoppage, not a `FAILURES, discard` case. No final fastchess summary was ever printed (match ended between periodic reports), so the last **periodic** summary — printed at the 480-game mark — is recorded here rather than a true final tally: LOS 87.17%, DrawRatio 36.25%, 199W/177L/104D (52.29%), Ptnml(0-2) [32, 39, 87, 39, 43]. Per project-owner decision, accepted as the final measurement for this validation rather than re-running (re-running risks the same cap). Mop-up's effect is expected to be concentrated in rare pawnless decisive endgames largely absent from a standard-opening-book batch, so a result within error of 0 (as here: +15.94 ± 27.62, well inside its own error bar) is consistent with "no measurable change in typical play," the expected outcome, not a sign the feature failed |
| 2026-07-27 | candidate-bce4d95+dirty (rook open-file, #126) | elo-reference-v1 | 500 | 10+0.1 | 23.66 +/- 25.70 | **SPRT NonRegression [-5, 0] — INCONCLUSIVE, hit the 500-game cap without crossing a bound** (LLR 0.76 of ±2.94, 25.8%). 204W/170L/126D (53.40%), LOS 96.51%, DrawRatio 35.60%, Ptnml(0-2) [26, 44, 89, 52, 39]. **Read this as "no regression detected", not as "+23.66 Elo"** — the 95% interval is [-2.04, +49.36] and still contains zero, so the point estimate is suggestive (LOS 96.5%) but not a demonstrated gain; the change is a 5 cp static-eval shift and was never expected to be resolvable at this sample size. Deciding it either way needs wider bounds or a much larger budget, which is not worth it for a correctness fix. `+dirty` is accurate but harmless here: the only uncommitted engine-source delta vs `bce4d95` was comment text in `Eval.cpp` (verified — no non-comment lines), so the binary is behaviourally identical to the commit. Wall time 00:40:41 at `-Concurrency 6` |
| 2026-07-29 | candidate-9394bad (tapered eval, #99 + #118 item 4) | elo-reference-v1 | 2000 | 10+0.1 | 13.90 +/- 13.44 | **SPRT Gain [0, 10] — INCONCLUSIVE, hit the 2000-game cap without crossing a bound** (LLR 1.90 of ±2.94, 64.4% of the way toward accepting H1). 842W/762L/396D (52.00%), LOS 97.89%, DrawRatio 37.80%, PairsRatio 1.16, Ptnml(0-2) [139, 149, 378, 161, 173], nElo 15.78 +/- 15.23. Wall time 02:41:56 at `-Concurrency 6`. **Read this as "positive, not yet decided" — not as "+13.90 Elo".** The 95% interval is [+0.46, +27.34]: it excludes zero only barely, and the test was still 36% short of a decision when the budget ran out. What it does establish is a *direction*: the eval-reviewer flagged middlegame king centralization as this change's main regression risk (the mg king table is file-blind while the eg table peaks centrally, so the blend crosses zero around phase 14), and a real regression there would have pushed this negative. It did not. Deciding [0, 10] properly needs roughly 4000-6000 games; the earlier run at the same settings was killed and restarted after the mop-up gate was re-keyed onto the loser's phase, so this figure covers the final code only |
| 2026-07-29 | candidate-08d4ef8 (bishop pair + connected rooks + castling, #111/#114/#115) | elo-reference-v1 | 288 | 10+0.1 | 54.74 +/- 35.67 | **SPRT Custom [-5, 15] — H1 accepted at 288 games, LLR 3.01, wall time 00:23:07.** 138W/93L/57D (57.81%), LOS 99.89%, DrawRatio 38.89%, PairsRatio 1.75, Ptnml(0-2) [13, 19, 56, 22, 34], nElo 62.84 +/- 40.13. **This row does NOT measure the three eval terms.** Like every row here it compares against the fixed `elo-reference-v1` anchor, so what H1 establishes is that **`main` + these terms, together, beat the anchor by more than 15** — the 95% interval [+19, +90] lies entirely above `elo1`. A test on that sum cannot decompose it into "main's existing margin" plus "this PR's delta", so no part of the +54.74 is attributable here. It decided in 23 minutes precisely *because* the true difference sits far above the midpoint of 5, which is the signature of a comparison dominated by something much larger than the quantity of interest. Nor can the delta be recovered by subtracting anchor rows: the tapered-eval row above is itself **inconclusive** (+13.90 with interval [+0.5, +27.3]), and differencing two noisy anchor measurements compounds their errors to roughly ±38 — less constrained than either input. Resolving a ~10-20 Elo change requires measuring it directly against `main`, which is the `main-56b3f45` row below. `+dirty` on the candidate label was this file only (the auto-appended row) — engine sources verified clean, so the binary is exactly `08d4ef8` |
| 2026-07-29 | candidate-08d4ef8 (bishop pair + connected rooks + castling, #111/#114/#115) | **`main` @ 56b3f45** | 378 | 10+0.1 | 38.76 +/- 29.86 | **SPRT Custom [-5, 15] — H1 ACCEPTED at 378 games, LLR 2.97, wall time 00:30:52.** 169W/127L/82D (55.56%), LOS 99.49%, DrawRatio 39.68%, PairsRatio 1.38, Ptnml(0-2) [15, 33, 75, 27, 39], nElo 45.96 +/- 35.02. **This is the row that measures the change** — reference is the merge-base `main` build, not the fixed anchor, so the delta is attributable to this PR. Binaries verified to differ only in `Eval.cpp`/`Eval.h` (the `UCIHandler.cpp` delta is two extra rows in the `eval` command, never executed during play; `EvalTests.cpp` is not linked into the engine). **Read this as "the three terms are worth having", not as "+38.76 Elo".** The 95% interval is [+8.90, +68.62]: it excludes zero comfortably, but the point estimate carries the usual small-sample inflation and its lower bound is what matches the eval-reviewer's +10-20 prior. Attribution among the three terms was deliberately traded away for budget (one SPRT instead of three) — per the bundling guidance above, do not credit any single term. The gain is evaluation quality, not search efficiency. An initial 3-position probe suggested the candidate reached depth 11 on 21.75% fewer nodes, but that was a **sampling artifact**: widening to 5 term-isolating positions gives a per-position spread of **-44% to +229%** and an aggregate of **-1.6%**, i.e. no systematic change. Node count at fixed depth is chaotic under small eval perturbations — a 20-30 cp shift can flip the PV and thus the whole tree — so it cannot be sampled meaningfully at this scale, and notably the deltas do not track each position's net root contribution. Move ordering never consults static eval in any case (`Sort.cpp` has no reference to `Evaluate`); eval feeds only the leaf return, the qsearch stand-pat cutoff, its alpha raise, and delta pruning. So the terms pay a real **2.03% nps** cost, buy no node reduction, and still won the SPRT — the strength comes from scoring positions better. `+dirty` on the label was this file only (the row appended by the preceding anchor run) — engine sources verified clean |
| 2026-07-31 | clang-cl ThinLTO @ 64a36a9 | MSVC LTCG @ 64a36a9 | 20 | 10+0.1 | -0.00 +/- 172.63 | **Smoke only — not a measurement.** 9W/9L/2D. Run purely to confirm fastchess could drive binaries living outside the normal `x64/Release` layout (both sit in a throwaway `bench/` tree). At 20 games the error bar is +/-172, so the Elo figure carries no information whatsoever and must not be quoted. |
| 2026-08-01 | clang-cl ThinLTO @ 64a36a9 (#84) | MSVC LTCG @ 64a36a9 | 622 | 10+0.1 | 27.43 +/- 24.00 | **SPRT Custom [-5, 15] — H1 ACCEPTED at 622 games, LLR 2.98 (101.3%), wall time 00:50:19.** 268W/219L/135D (53.94%), LOS 98.79%, DrawRatio 36.01%, PairsRatio 1.37, Ptnml(0-2) [38, 46, 112, 59, 56], nElo 31.38 +/- 27.30. **This row is the DECISION, not the estimate** — read it as "worth more than 15 Elo", not as "+27.43 Elo"; the 95% interval is [+3.43, +51.43]. The estimate is the 3500-game row below, which supersedes this number. **Both sides are the same source at `64a36a9`** — only the compiler and its LTO mode differ (MSVC `/GL /LTCG` vs clang-cl `-flto=thin` with `lld-link`), so the delta is attributable to code generation alone. Equivalence was verified before the match: at Threads=1 both binaries visit identical nodes and return identical best moves on all 8 `Run-Bench.ps1` positions. `[-5, 15]` was chosen over `-Sprt Gain` deliberately: `Gain` is `[0, 10]`, and the tapered-eval row above hit its 2000-game cap still inconclusive on those bounds. Wider bounds decide faster, and `elo1 = 15` is the more useful question — adopting a second toolchain has real cost, so what matters is whether the gain is material. `+dirty` on the auto-appended label was this file plus untracked scratch directories; `git status -- StratEngine StratChessEvolved CMakeLists.txt` was empty, so the binary is exactly `64a36a9`. |
| 2026-08-01 | clang-cl ThinLTO @ 64a36a9 (#84) | MSVC LTCG @ 64a36a9 | 3500 | 10+0.1 | **40.28 +/- 9.81** | **Fixed batch (no SPRT) — this is the ESTIMATE row. Wall time 04:44:55 at `-Concurrency 6`.** 1525W/1121L/854D (55.77%), LOS 100.00%, DrawRatio 36.91%, PairsRatio 1.62, Ptnml(0-2) [173, 248, 646, 368, 315], nElo 47.69 +/- 11.51. 95% interval **[+30.5, +50.1]**. Run because the SPRT's +/-24 was too wide to act on; a fixed batch was the right tool because SPRT optimises for deciding a hypothesis, not for estimating a value, and its early stop leaves the point estimate subject to optional-stopping bias. **Do NOT pool this with the 622-game SPRT row** — pooling a stopped sample into a fixed batch drags that bias along. This row alone is the estimate; the SPRT row alone is the decision. **The two are consistent, not contradictory**: +40.28 falls inside the SPRT's [+3.43, +51.43], so the earlier figure was simply noisy and landed low. (A prediction that the batch would land *below* +27.43 on optional-stopping grounds was wrong — that effect is real but second-order, and sampling noise at 622 games dominated it.) **Quote this as "+40 Elo at 10+0.1", not as "+40 Elo".** The nps side measured +23.32% (2,798,491 -> 3,451,020 aggregate, 5 runs each, per-config spread 0.27-1.11%), which is 0.302 doublings; at the conventional ~60 Elo/doubling that predicts ~18 Elo. The measured result implies **~133 Elo per doubling**, far above the textbook figure. The likely cause is the time control: speed advantages are amplified at 10+0.1, where an engine is more often one iteration short. Expect a smaller gain at longer controls; this was not measured. **What this does not settle is the cost of adoption** — VS's ClangCL toolset has no LTO integration at all (`-flto` and `lld-link` were wired by hand), `WholeProgramOptimization=true` under it is a trap that produces a smaller binary while passing no `-flto`, and the clang legs only compiled with `-Wno-error` because this engine has only ever been `/W4 /WX`-clean against MSVC. See #84. |
| 2026-08-03 | elo-reference-v2 (same binary) | elo-reference-v2 | 20 | 10+0.1 | -107.54 +/- 127.62 | **Re-pin verification — carries no strength information.** Both sides were the *same file*, SHA256 `D0B4F7F4…B308D6A`, so the true difference is exactly zero by construction. Run to confirm the new default reference resolves from cache and fastchess drives it; both did. The ±127.62 interval spans zero more than eight times over, which is the point: a 20-game smoke resolves nothing, and reading its point estimate as a result is the mistake this row exists to illustrate. The `+dirty` in the auto-generated label was uncommitted doc/script edits in the worktree — the binary is exactly `df9245f` |
