# ELO Measurement Method

How strength is measured in this project. **The measurements themselves are in
[`EloLog.md`](EloLog.md)** — this file is the method, that one is the record. Plan/design:
`.claude/plans/elo-baseline-measurement.md`.

## The rules that are never traded away

Everything else here is guidance. These five are not, because breaking any of them produces a number
that looks exactly like a measurement and is not one.

1. **Never compare binaries from different compilers.** The clang-cl/MSVC gap alone is worth roughly
   +40 Elo (#84) and lands on whatever change is under test. See [the two anchors](#the-two-anchors).
2. **A batch reporting a time loss, illegal move or disconnect is discarded, not reported.** Those
   are harness or engine bugs, never strength data.
3. **An SPRT verdict is not a point estimate.** `H1 accepted` means "bigger than `elo1`", and its
   headline Elo figure carries the usual small-sample inflation. See [reading the
   result](#reading-the-result).
4. **The fixed anchor measures where the engine stands, not what your change did.** To decide
   whether a change earned its place, measure against `main`. See [the anchor measures the
   sum](#the-anchor-measures-the-sum-not-your-change).
5. **State the book on any row that did not use the default one.** Opening selection moves the draw
   rate, and the draw rate moves the error bar.

---

## 1. Running a measurement

### What are you asking?

Pick the row first; the instrument follows from it.

| Your question | Use | Typical cost |
|---|---|---|
| Did this make things worse? (refactors, restructures, anything expected neutral) | `-Sprt NonRegression` — bounds `[-5, 0]` | Stops early when decisive; 500-game cap |
| Is this worth ≥ ~10 Elo? (small new eval terms) | `-Sprt Gain` — bounds `[0, 10]` | Often hits the cap; see [budget](#spending-the-budget) |
| Anything else, with bounds you choose | `-Sprt Custom -Elo0 <a> -Elo1 <b>` | Wider bounds decide faster |
| How big is the difference? (baselines, sanity runs, large expected effects — e.g. the Lazy SMP threads=4 row at +128.55 ± 28.36) | Fixed batch | 500 games ≈ 40 min ≈ ±25 Elo |
| Does the pipeline work at all? | `-Smoke` | 20 games, ~2 min, resolves nothing |

A fixed batch answers *"how big is it?"*. SPRT answers *"is this worth keeping?"* and stops as soon
as the evidence is decisive. The distinction matters more than it sounds:

> 500 games resolves ±25 Elo. Bishop pair, connected rooks, castling-done, outposts and queen
> activity are each worth roughly 5–20 Elo. **A fixed batch cannot distinguish any of them from
> zero.** The mop-up row in `EloLog.md` (`+15.94 ± 27.62`) is exactly this: a result equally
> consistent with "+16 Elo", "no change", and "−10 Elo".

### The commands

Build the candidate first (`.\build.ps1 main`) — the script does not build it, and `build.ps1`
defaults to the shipping clang-cl build, which is the only one comparable against the reference.

```
# fixed batch against the default anchor — "where do we stand"
pwsh -ExecutionPolicy Bypass -File <abs>\Scripts\Run-EloMatch.ps1

# against main rather than the anchor — see rule 4
... Run-EloMatch.ps1 -ReferenceExe <merge-base build> -ReferenceTag <commit>

# every -Sprt form needs that same isolating reference, so build the merge base first
# "prove it did not make things worse"
... Run-EloMatch.ps1 -Sprt NonRegression -ReferenceExe <merge-base build> -ReferenceTag <commit>

# "prove it is worth >= ~10 Elo"
... Run-EloMatch.ps1 -Sprt Gain -ReferenceExe <merge-base build> -ReferenceTag <commit>

# explicit bounds
... Run-EloMatch.ps1 -Sprt Custom -Elo0 0 -Elo1 5 -ReferenceExe <merge-base build> -ReferenceTag <commit>

# a cumulative verdict, asked for deliberately — "how far ahead of the anchor are we"
... Run-EloMatch.ps1 -Sprt Custom -Elo0 0 -Elo1 20 -AnchorSprt
```

`-Sprt Custom` requires both `-Elo0` and `-Elo1` explicitly. `-Sprt` cannot be combined with
`-Smoke`: a 20-game run can never reach a decision, so the result would always read "inconclusive",
which looks like a measurement and is not one. Nor can it run against the tag-resolved reference —
that verdict would be about the wrong quantity; see [the anchor measures the
sum](#the-anchor-measures-the-sum-not-your-change).

The SPRT wiring itself is verified against the pinned fastchess build: bounds `[0, 200]` between two
identical builds accepted H0 after 10 games rather than playing out the 200-game cap.

### Reading the result

The script appends one row to `EloLog.md` automatically and prints the same figures.

- **Within ±error of 0** — no measurable change. This is the *expected* result for a pure refactor;
  byte-identical node-count validation is a stronger check for those than any match.
- **Clearly negative after pooling** (e.g. −30 ± 18 over 1000 games) — a regression. Investigate
  before merging.
- **Near the bound** — unresolved. Re-run and pool; see [resolution](#resolution-what-500-games-can-see).
- **`H1 accepted` / `H0 accepted`** — a decision, not an estimate. An `H1 accepted` at 300 games is a
  *stronger* claim than a fixed 500-game point estimate, not a weaker one. Record the verdict, which
  the script writes into the Notes column as `SPRT <preset> [elo0, elo1] — H1 accepted`.
- **`inconclusive @ N games`** — a real outcome meaning "smaller than `elo1`, or the budget ran out".
  Not a failure, and **not a measurement of zero**. The script flags it in yellow rather than letting
  it pass as a decision. **An inconclusive `NonRegression` run whose interval excludes zero still
  answers the question it was asked** — "is this worse?" — so it is normally enough to act on. Record
  it as `inconclusive, interval [a, b]` and move on; that phrasing is the whole treatment the case
  needs, and it does **not** license quoting the point estimate as the change's value. Deciding the
  bounds properly is a separate spend, priced under [budget](#spending-the-budget).
- **`FAILURES, discard`** — a time loss, illegal move, disconnect or stall. The script marks the row
  and exits 1. Discard the batch; do not read its Elo.
- **`same binary and configuration — carries no strength information`** — both sides were the same
  bytes with the same options, so the true difference is zero by construction and the Elo column is
  pure noise. Pipeline checks, time-forfeit checks and reference re-pin verification all land here.
  The script detects this rather than offering a flag to suppress the row: a suppression switch can
  be forgotten, and could be reached for after seeing an unwelcome number. **Same binary with
  *different* options is not this case** — that is a configuration comparison and a real
  measurement, which is how the Lazy SMP `+128.55 Elo` row was produced.

Bounds and reported Elo are both **logistic** Elo. The script pins `model=logistic` deliberately —
fastchess's own default is `normalized` (nElo), a different scale on which `elo1=10` would silently
mean something else.

---

## 2. The setup

### Pinned components

Changing any of these starts a new setup record.

| Component | Value |
|---|---|
| Match runner | fastchess **v1.8.2-alpha** (`fastchess alpha 1.8.2`, windows-x86-64), from https://github.com/Disservin/fastchess/releases/tag/v1.8.2-alpha |
| Runner location | `<DepsRoot>EngineTesting\fastchess.exe` (repo sibling, same convention as spdlog/json/Catch2) |
| Opening book | Resolved by `Run-EloMatch.ps1`: `-Book <path>` if given, else `EngineTesting\openings-large.pgn\|.epd` if present, else the committed `Tests/openings/openings-250.pgn` — first 250 games of `8moves_v3.pgn` (official-stockfish/books), sequential order, each pair color-swapped (`-repeat`). **250 openings = 500 distinct games**; see [the book runs out](#the-opening-book-runs-out) |
| Reference build | git tag **`elo-reference-v2`** (`df9245f`, 2026-08-03). Cached as `EngineTesting\StratChess-elo-reference-v2.exe`; rebuilt from the tag automatically on cache miss |
| Time control | 10 s + 0.1 s increment |
| Adjudication | draw: movenumber=40 movecount=8 score=10; resign: movecount=4 score=800 |
| Machine | Windows 11 Pro x64 (theEscape2207 dev machine) — results are machine-relative; re-establish the sanity row when measuring on different hardware |
| Concurrency | 6 concurrent games (`-Concurrency`) — sized off physical cores (12) ÷ 2 single-threaded engine processes per game, not the 24 logical/SMT threads; re-tune alongside the Machine row |

**The instrument has been calibrated.** Identical builds (candidate byte-identical to the reference,
SHA256-verified) over 2×500 games pooled to **−1.4 Elo** (378W/382L/240D, 49.80%): zero measurable
bias. The two batches individually hit opposite ±2σ edges, which is what calibrates the per-batch
noise quoted throughout this document. That run used `elo-reference-v1`, but it measures the
*instrument* rather than the anchor, so it carries over to v2 unchanged.

### Upgrading the match runner

Every automated use of fastchess is text-scraping its console output: `Run-EloMatch.ps1`'s Elo,
game-count, LLR and SPRT-verdict patterns, both harnesses' diagnostic classification, and
`pool_pentanomial.py`'s `Ptnml(0-2)` parse. A release that rewords a line degrades a run to
"inconclusive" or refuses to pool a shard — failure modes that look like results. The bump is gated
on the new binary's output, not on its changelog.

1. **Run the same short SPRT under both binaries**, same build on both sides:
   `-Games 20 -Sprt Custom -Elo0 0 -Elo1 200` (`-Sprt` cannot be combined with `-Smoke`). All four
   result patterns must still match — one that silently stops matching reports "inconclusive"
   rather than failing.
2. **Make the new build check the pooling arithmetic.** Feed its printed `Ptnml(0-2)` to
   `.github/scripts/pool_pentanomial.py` and require the pooled figure to reproduce the
   `Elo: x +/- y` the binary printed itself. Add that triple to `--self-test`.
3. **Re-extract the diagnostic wordings.** Both harnesses classify by fastchess's exact message
   strings, because a keyword sweep also matches engine output echoed into the log. The release
   archive ships `app/src`, so the authoritative list is one grep away:
   `grep -rhoE '"(Warning|Error);[^"]*"' app/src`. Keep `$pvWarnRe`/`$fatalRe` in `Run-EloMatch.ps1`
   byte-identical to `pv_warn_re`/`fatal_re` in `strength.yml`. A wording neither names is reported
   as an unclassified diagnostic rather than silently tolerated — a prompt to update the lists, not
   a substitute for doing it.
4. **Provoke the warning classes rather than hoping a match emits them.**
   `.github/scripts/fastchess_probe_engine.py` is a mock UCI engine that reports an illegal PV
   (`badpv`) or plays an illegal move (`illegalmove`); two of them playing each other over two games
   is enough. The first must be counted and tolerated, the second must fail the run — under the old
   binary as well as the new one, so that any difference is attributable to the version.

Traps:

- The Windows zip and the Linux tar both extract to a **subdirectory**. The CI fetch step depends on
  that and breaks quietly if a release changes it.
- The calibration runs append rows to `Docs/EloLog.md`. Revert them — both sides are the same
  binary, so they carry no strength information.
- Elo history stays comparable only while the time control and adjudication settings are untouched.
  That is why they are their own rows in the table above.

### The two anchors

There are two pinned references, and picking the wrong one produces a number that looks real and is
not.

| Tag | Commit | Compiler | Use for |
|---|---|---|---|
| `elo-reference-v2` | `df9245f`, 2026-08-03 | clang-cl + ThinLTO (ships) | **Default.** Day-to-day search/eval changes |
| `elo-reference-v1` | `fd8b665`, 2026-07-03 | MSVC + LTCG | The long-run epic comparison only |

**Why v2 exists.** The shipping compiler changed to clang-cl when #177 merged, and that change alone
is worth roughly +40 Elo at 10+0.1 (#84). Measuring a clang-built candidate against the MSVC-built
v1 credits that +40 to whatever change is under test — a phantom gain large enough to make an eval
regression look like an improvement.

**Why v1 is kept.** It is the long-run anchor for the eval (#110) and build-modernization (#81)
epics: a single before/after across both, where the compiler gain is *part of* what is being
measured rather than a confound. Do not delete `EngineTesting\StratChess-elo-reference-v1.exe` or
the tag. Tracked in #180.

```
... Run-EloMatch.ps1 -ReferenceTag elo-reference-v1
```

v1 predates the CMake migration, so rebuilding it from its tag uses that tag's own MSBuild
`build.ps1` and needs the sibling spdlog/nlohmann checkouts to still exist. `Run-EloMatch.ps1` warns
when it rebuilds a pre-migration reference — but only on a cache *miss*. A cached v1 binary is used
silently.

### Resuming an interrupted match

Symptom: `logs\elo\<stamp>.log` has a trailing `Started game N` with no matching `Finished game N`,
or simply stops advancing.

```
... Run-EloMatch.ps1 -ResumeDir StratChessEvolved\logs\elo
```

fastchess autosaves tournament state every `-AutosaveInterval` games (default 20) to
`logs\elo\config.json`, and `-ResumeDir` reloads it to restore the original engine and tournament
configuration. At most one autosave interval of games is replayed, not the whole batch.
`-CandidateExe`, `-ReferenceTag`, `-ReferenceExe`, `-Games`, `-Tc`, `-Concurrency` and the SPRT
bounds are all ignored when resuming — they come from the saved state.

Two constraints follow from that file being a single flat file shared by every invocation:

- **Resume promptly.** Starting any other (non-resume) match first overwrites it.
- **A completed capped run cannot be extended.** Resume restores the original `-Games`, so "run 500,
  then add more" is not available. Restart at the higher number.

---

## 3. Why measurement is hard here

Each of these has cost this project a run, a wrong conclusion, or both.

### Resolution: what 500 games can see

500 games ≈ **±25 Elo** at 95% confidence, given this engine's ~37% draw ratio. That is measured,
not assumed — the ±15 rule of thumb assumes a higher draw rate than this engine produces. Error
scales as 1/√N, so halving the bound costs 4× the games: ±25 at 500 → ±12.5 at 2 000 → ±6 at 8 000.

**Single batches genuinely wander.** The two identical-build sanity batches landed at +25.1 and
−27.9, both at the edge of their own error bars, pooling to −1.4. Treat any single-batch result near
the bound as unresolved: re-run and pool before acting on it.

### The anchor measures the sum, not your change

`elo-reference-v1` and `-v2` are **fixed** anchors, so every row against one measures cumulative
progress since it — the engine's standing, not the PR's delta. That is the right instrument for
tracking the project and the wrong one for deciding whether one small change earned its place:

- **An SPRT against the anchor tests the sum.** H1 means "`main` + this change beats the anchor by
  more than `elo1`", which can be true on `main`'s pre-existing margin alone. A verdict on a sum
  licenses no claim about one addend. The 2026-07-29 `candidate-08d4ef8` row in `EloLog.md` accepted
  H1 in 23 minutes on a comparison that says nothing about the three terms it was run for.
- **The delta cannot be recovered by subtraction.** Differencing two anchor rows compounds their
  errors, so the result is less constrained than either input — and most rows are individually
  inconclusive to begin with.

So to decide whether a change helps, build the merge-base and pass it via `-ReferenceExe`, with
`-ReferenceTag` naming the commit. Keep the anchor run too when the cumulative figure is wanted; the
two answer different questions and both belong in the log, labelled as to which is which.

`Run-EloMatch.ps1` enforces this rather than trusting it: `-Sprt` exits 1 when the reference comes
from the tag lookup, because such a reference is a fixed anchor by construction. `-ReferenceExe` is
the per-change path. `-AnchorSprt` overrides the refusal for a cumulative reading asked for on
purpose, and labels the `EloLog.md` row accordingly, so the two kinds of verdict stay distinguishable
without anyone having to annotate a row by hand afterwards — which is what the 2026-07-29 row needed.

### The opening book runs out

An opening pair is two games, so **N openings yield 2N distinct games**. The committed book holds
250 openings, so a 500-game batch consumes it **exactly**. Every game past that replays an opening
already played, which narrows the reported error bar without adding information — the run looks more
precise than it is.

This is least visible where it matters most: SPRT runs take `-Games` as an upper bound and routinely
ask for thousands. `Run-EloMatch.ps1` prints the book and its opening count on every run and warns
when `-Games` exceeds the distinct-game count.

To run bigger batches honestly, drop a large book beside the checkout as
`EngineTesting\openings-large.pgn` (or `.epd` — the format flag follows the extension). The natural
choice is the full `8moves_v3.pgn`, the same source the committed book was cut from. It is
deliberately **not committed**: third-party data of varying provenance, in a public repository, so it
lives with fastchess and the reference binaries like every other external test asset.

**The discovery glob matches the name, not the content.** A book sitting in `EngineTesting\` under
its upstream name is invisible to it, and the run silently falls back to the committed 250 — which is
how #338's first SPRT came to exhaust the small book while `8moves_v3.pgn` sat unused in the same
directory. Copy or rename it to `openings-large.pgn`, and read back the line the script prints before
trusting a batch larger than 500 games:

```
Opening book : ...\EngineTesting\openings-large.pgn (34700 openings, format=pgn)
```

### Spending the budget

`-Games 500` means two different things depending on mode, and conflating them is the trap:

- **Fixed batch — 500 *is* the measurement.** An expensive dial (1/√N above). Raise it only when a
  point estimate is the deliverable — a new reference baseline, or fitting data for #117 — not to
  make a verdict "more certain", which is what SPRT is for.
- **SPRT — 500 is only the give-up point.** It has no bearing on the answer's quality. Raising it is
  statistically free and costs wall-clock only in runs that would otherwise return inconclusive. The
  500 default was chosen as a fixed-batch resolution target and simply inherited as the SPRT cap;
  there is no statistical reason for the two numbers to be equal.

**Reach for wider bounds before more games.** Expected sample size scales roughly with the inverse
square of the indifference region's width, so `-Sprt Custom -Elo0 -10 -Elo1 0` costs about **4× fewer
games** than `NonRegression`'s `[-5, 0]`. Ask the loosest question that still settles the decision.

**Buy information per game before buying more games.** A sharper opening book yields more decisive
pairs (the #126 run drew 35.6%); pentanomial scoring (the `Ptnml(0-2)` line) is already in use as the
other main variance reduction.

**Estimating what an inconclusive run would have needed.** LLR accumulates roughly linearly in N in
expectation, when the true effect lies outside the indifference region, so
`games_needed ≈ N × 2.94 / LLR_at_N`. The #126 row reached LLR 0.76 at 500 games → ~1 900 games,
~2.5 h. Order-of-magnitude only: LLR is a random walk, and if the true effect sits *inside* the
indifference region the test may not converge at any practical N.

**The binding ceiling is operational, not statistical.** A 500-game batch takes ≈40 min at
`-Concurrency 6`, and the 2026-07-26 mop-up row was killed at ~60 min by a background-task duration
cap in the execution tooling. The practical limit for a *background-launched* match is roughly
**700–750 games**; past that, run it in the foreground or expect to resume.

**Do not raise `-Concurrency` to buy throughput.** It is pinned to physical cores deliberately;
oversubscribing injects timing noise — or genuine time losses — into a fixed real-time control, and
by rule 2 a batch with a time loss is thrown away.

**Lowering `-Games`** is only useful for `-Smoke`. Under SPRT it is actively counterproductive: an
early decision costs nothing, so a low cap buys nothing and risks an avoidable inconclusive.

### The 100 ms per-move floor

`compute_budget()` floors every move at 100 ms regardless of how much clock is left, so at any
increment below that the engine loses ground every move once its clock drains, and eventually
forfeits. Measured: **3 time losses in 4 games at 5+0.05, none at 5+0.1**; at 2+0.02 the handicapped
side flagged in all four.

Two consequences. A handicap run must halve the **base** time and leave the increment alone —
halving both tests the time manager's floor rather than whatever is under test. And the standard
10+0.1 sits *exactly* on the floor: its 100 ms increment repays the minimum move cost and no more, so
there is no margin on a slower or contended machine. Tracked as #204.

### When even SPRT cannot resolve a term

Some terms are too small to decide at any practical game count. The fallback is to **bundle several
into one PR and measure them jointly**. That is a deliberate measurement decision, not sloppy
scoping — but say so explicitly in the row's Notes, so a later reader does not attribute the whole
delta to whichever term the commit message happens to mention first.

---

## 4. The Linux CI instrument

`.github/workflows/strength.yml` (`workflow_dispatch` only) plays the same kind of match on a GitHub
runner: both sides built from source in one job by GCC on `ubuntu-24.04`, Release, `Threads=1`,
with adjudication settings copied from `Run-EloMatch.ps1` verbatim.

**Its rows live in their own table in `EloLog.md` and are never compared with a local one** — rule 1,
applied between instruments rather than within one. Ratios transfer between the two ledgers;
absolute values do not.

Two deliberate differences from the local setup:

- **Book.** `UHO_4060_v3.epd` (242,201 openings — the 4060 names the evaluation band the positions
  were selected from, not their count), downloaded per run from a pinned commit of
  `official-stockfish/books`. Not committed, for the reason M3 records in
  `.claude/plans/public-repo-and-strength-lab.md`.
- **Fixed N, no SPRT.** SPRT is a sequential test and does not shard across runners; the CI
  instrument buys resolution with games instead, since minutes there are free.

The workflow warns when either side is given an increment under 0.1 s, for the reason in
[the 100 ms floor](#the-100-ms-per-move-floor).

Every run also uploads the annotated PGN of every game it played, retained 90 days.
[`MoveQuality.md`](MoveQuality.md) is the method and baseline for reading them: the pooled Elo says
whether a change helped, that scan says where.

**Calibrated 2026-08-05**, by the two runs recorded at the top of `EloLog.md`:

- **Null test** — identical commit and clock on both sides, 1000 games: **-3.47 ± 18.21**, an
  interval containing the zero the setup guarantees. No measurable bias, matching the local
  instrument's -1.4 at the same game count.
- **Known-sign control** — same binary, reference on half the base time, 200 games:
  **+75.88 ± 42.56**, LOS 99.99%. The instrument detects a real difference rather than returning
  noise whatever it is shown.

Both ran with **zero time losses**, which also answers the open question from
[the 100 ms floor](#the-100-ms-per-move-floor): a shared 4-vCPU runner does hold 10+0.1 at
concurrency 3.

A null test and a control are the minimum, not a guarantee. They establish that the instrument is
unbiased and not blind; they say nothing about resolution, which is what the error bar reports on
every run.
