# ELO Measurement Method

How strength is measured in this project: the pinned setup, how to run a match, how to read the
result, and when a fixed batch answers the question versus when only an SPRT can.

**The measurements themselves are in [`EloLog.md`](EloLog.md)** — this file is the method, that one
is the record. Plan/design: `.claude/plans/elo-baseline-measurement.md`.

Run a match with:

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1"
```

Build the candidate first (`.\build.ps1 main`). Use `-Smoke` for a 20-game pipeline check.

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

- 500 games ≈ **±25 ELO** at 95% confidence with this engine's ~37% draw ratio (measured, see the
  sanity rows in `EloLog.md` — lower draw ratios mean noisier matches than the ±15 rule-of-thumb
  assumes); scale games ×4 to halve the error bound.
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
> zero.** The mop-up row in `EloLog.md` (`+15.94 ± 27.62`) is exactly this: a result equally consistent with
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
- **Bounds are in logistic Elo**, the same scale as the `Elo:` line and everything in `EloLog.md`'s
  history table. The script pins `model=logistic` for this reason; fastchess's own default is
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
`games_needed ≈ N × 2.94 / LLR_at_N`. The #126 row in `EloLog.md` reached LLR 0.76 at 500 games → ~1 900
games, ~2.5 h. Treat this as order-of-magnitude only: LLR is a random walk, and if the true effect
sits *inside* the indifference region the test may not converge at any practical N.

**The binding ceiling is operational, not statistical.** A full 500-game batch takes ≈40 min at the
default `-Concurrency 6`, and the 2026-07-26 mop-up row in `EloLog.md` was killed at ~60 min by a
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
  licenses no claim about one addend. See the 2026-07-29 `candidate-08d4ef8` row in `EloLog.md`, where H1 was
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

**Its rows live in their own table in `EloLog.md`, and are never compared with a local one.** The
binaries come from a different compiler on different hardware, which is the one comparison this
whole document exists to prevent (issue #84 measured the clang-cl/MSVC gap alone at roughly +40 Elo). Ratios transfer between
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

