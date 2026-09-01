# Measurements

Every strength measurement this project has taken. These are **data files, not documentation** —
append-only records, mostly written by a script. How to *choose and run* an instrument is the
`measure-strength` skill; how to *record what came back* is this file.

| Ledger | Instrument | What one row says |
|---|---|---|
| [`ci-calibration.md`](ci-calibration.md) | `strength.yml`, GCC on `ubuntu-24.04` | how the harness behaves — nothing about the engine |
| [`ci-per-change.md`](ci-per-change.md) | same | what one change was worth against the commit it forked from |
| [`ci-anchor.md`](ci-anchor.md) | same | cumulative strength against a fixed tag |
| [`local.md`](local.md) | `Run-EloMatch.ps1`, clang-cl on Windows | mixed — each row names its own reference |

**A row is only ever read against other rows in its own ledger.** Different instruments and
different references are not on a common scale, which is why these are separate files rather than
separate sections of one.

## Recording a row

Each ledger is a narrow table plus a `Row detail` section beneath it, in the same order. The table
carries the mechanical facts; the prose section carries what a reader could not reconstruct from
them.

**Verdict is a closed vocabulary.** Anything else means the row has not been classified:

| Verdict | Means |
|---|---|
| `gain` | an interval excluding zero on the positive side, or an accepted H1 on a `Gain`/`Custom` SPRT |
| `non-regression` | an accepted H1 on a `NonRegression` SPRT, or an interval tight enough to bound any regression |
| `regression` | an interval excluding zero on the negative side |
| `inconclusive @ N` | the run hit its N-game cap without crossing a bound |
| `calibration` | both sides are the same binary — carries no strength information |
| `smoke` | too few games to resolve anything; run to prove the plumbing works |
| `discarded` | see the discard rules below |

**A cap-stopped SPRT is not a measurement.** It is `inconclusive @ N`, whatever the point estimate
looks like. An SPRT that *did* cross a bound gives a decision, not a size: quote it as "worth more
than `elo1`", never as its point estimate, which carries optional-stopping inflation. Only a fixed
batch gives a size.

**Discard the batch, not the game**, on any time loss, illegal move played, disconnect or stall. On
a shared runner a time loss most likely means the box was oversubscribed, which invalidates
everything the batch measured. An illegal *PV* warning (#310) is a reporting defect fastchess plays
through and is not grounds to discard — say so in the row's detail section when one appears.

**Label the reference from what actually played.** `Run-EloMatch.ps1` names the reference side from
`-ReferenceTag` regardless of `-ReferenceExe` (#309), which silently inverts the distinction these
ledgers depend on — an anchor row measures cumulative standing, a merge-base row measures one
change. Pass `-ReferenceTag` naming the commit whose binary is really on the other side.

**Never compare across ledgers.** A Linux-lab row and a local clang-cl row differ in compiler,
machine and book at once. Same trap as the MSVC rule, different axis.

**The detail section is for what the table cannot hold** — which shards disagreed, why a figure
supersedes an earlier one, what a run does *not* settle, a hand-correction and its evidence. It is
not for restating the verdict. If a sentence would still be true with the numbers deleted, it is
padding: `INCONCLUSIVE` already says the run settled nothing, and `10.43 +/- 18.33` already says the
point estimate is not a measured gain.

**Node counts and nps quoted in rows before 2026-08-16 are main-tree only.** Until #312 the engine
counted `pvs()` nodes and never `quiescence()` nodes, so any earlier figure excludes every move
searched inside quiescence — 12.1% of the true total on the bench suite at the time of the fix,
understating nps by 13.8%. An nps figure is therefore not comparable across that boundary, and a
change moving work between the two trees could show an nps collapse with no real slowdown, or the
reverse. **Wall clock was never affected** and is the figure to trust in an older row. Where a row
quotes a "derived" time-to-depth, it was computed as `nodes / nps` from two different runs and is
unreliable.

## The setup

### Pinned components

Changing any of these starts a new setup record.

| Component | Value |
|---|---|
| Match runner | fastchess **v1.8.2-alpha** (`fastchess alpha 1.8.2`, windows-x86-64), from https://github.com/Disservin/fastchess/releases/tag/v1.8.2-alpha |
| Runner location | `<DepsRoot>EngineTesting\fastchess.exe` (repo sibling, same convention as spdlog/json/Catch2) |
| Opening book | Resolved by `Run-EloMatch.ps1`: `-Book <path>` if given, else `EngineTesting\openings-large.pgn\|.epd` if present, else the committed `Tests/openings/openings-250.pgn` — first 250 games of `8moves_v3.pgn` (official-stockfish/books), sequential order, each pair color-swapped (`-repeat`). **250 openings = 500 distinct games**, so a 500-game batch exhausts that book exactly |
| Reference build | git tag **`elo-reference-v2`** (`df9245f`, 2026-08-03). Cached as `EngineTesting\StratChess-elo-reference-v2.exe`; rebuilt from the tag automatically on cache miss |
| Time control | 10 s + 0.1 s increment |
| Adjudication | draw: movenumber=40 movecount=8 score=10; resign: movecount=4 score=800 |
| Machine | Windows 11 Pro x64 (theEscape2207 dev machine) — results are machine-relative; re-establish the sanity row when measuring on different hardware |
| Concurrency | 6 concurrent games (`-Concurrency`) — sized off physical cores (12) ÷ 2 single-threaded engine processes per game, not the 24 logical/SMT threads; re-tune alongside the Machine row |

**The local instrument has been calibrated.** Identical builds (candidate byte-identical to the
reference, SHA256-verified) over 2×500 games pooled to **−1.4 Elo** (378W/382L/240D, 49.80%): zero
measurable bias. The two batches individually hit opposite ±2σ edges, which is what calibrates the
per-batch noise. That run used `elo-reference-v1`, but it measures the *instrument* rather than the
anchor, so it carries over to v2 unchanged. The Linux instrument's own calibration is
[`ci-calibration.md`](ci-calibration.md).

**Moving the match runner forward** is gated on the new binary's output rather than on its
changelog, because every automated use of fastchess is text-scraping that output. The procedure and
its traps: [`../Docs/MatchRunnerUpgrade.md`](../Docs/MatchRunnerUpgrade.md).

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

A resumed row is assembled from two processes' output, and resume mode labels it from the resuming
invocation rather than the restored `config.json` (#388) — check the reference and the wall time by
hand before trusting the appended row.
