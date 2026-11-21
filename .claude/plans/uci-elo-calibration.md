# UCI_Elo calibration against Stockfish 19 — Design

**Issue:** #985 (calibration slice; the option-and-pick slice shipped in PR #992, `bdb0f838`)

## Goal

`UCI_Elo` is a label until it is measured. `StrengthLimit.cpp` ships a provisional two-point
table (1320 → level 0.0, 2600 → level 19.0), so `UCI_Elo=1800` today means "level 7.1", not "plays
like 1800". This slice measures what each level actually scores against Stockfish 19 at
`UCI_LimitStrength` (the anchor the owner chose on 2026-11-21) and replaces the anchor points and
the advertised range with the measured ones. Nothing else in the policy changes.

## Review focus

- **D5, the fit's top-block rule.** A plateau at the top (expected: levels from about 13-15 up are
  clock-bound at 60+0.6, design record D2) is written as two anchors one Elo apart. Is that the
  right way to keep the table strictly increasing while telling the truth about a plateau?
- **D3/D4, precision for the budget.** Seven levels, 300 games each, with the second stage's
  opponent picked from a 60-game first stage. Does that buy ±50 per point, and are the levels the
  right ones?
- **A1/A2, the anchor's own drift.** Stockfish's `UCI_Elo` fit is from 2023 master and states no
  time control; we inherit both.

## Scope

**This change will:**

- add `Scripts/Run-UciEloCalibration.ps1` (the gauntlet driver; codifies the 2026-11-20 scouting run)
  and `Scripts/fit_uci_elo_calibration.py` with `Scripts/test_fit_uci_elo_calibration.py`;
- play the gauntlet locally in two stages, each started by the owner (D4);
- replace `kAnchors` in `StratEngine/StrengthLimit.cpp` and `kMinElo`/`kMaxElo` in
  `StrengthLimit.h`, rewrite their "provisional" comments, and update the one test literal that
  spells the range (`UCIStrengthLimitTests.cpp:112`);
- record the fit in a new ledger, `Measurements/uci-elo-calibration.md`, listed in
  `Measurements/README.md`; update `Docs/EngineGuide.md` (range, TC, no longer provisional) and
  `Docs/Changelog.md`.

**This change will not:**

- touch the pick (`pick`, `pick_depth`, `weakness`, the mate guard), the forced MultiPV 4, the
  depth cap, or anything in the search. If the data says a level plays badly, that is a new issue;
- calibrate below level 0 (the UI's beginner levels own that band) or for `Threads` > 1 or a user
  `MultiPV` > 1;
- fix #991 (`go infinite`); fastchess never sends it;
- add a CCRL-engine anchor (option B of the triage).

## Decisions

### D1: 60+0.6 on both sides

Owner decision, 2026-11-21. StratChessUI sends only `go movetime` today (default 1000 ms), but
clocked games are planned soon, and 60+0.6 is also the scouting's TC, so the scouting result
carries over as a cross-check. Both engines play the same clock: the label should mean "scores 50%
against Stockfish at this Elo when both think the same time".

Rejected: `movetime 1000`, what the UI sends today; it is about to be superseded, and is offered as
an optional spot check (Cost, O1). Rejected: playing Stockfish at the TC its fit used, as the
domain review suggested, because the fit states none (A2).

### D2: Local, on the owner's machine, shipping binary, concurrency 8

The candidate is the clang-cl Release build of this branch (`build/windows-clang-cl/StratChessEvolved.exe`,
the binary the UI runs), on the machine the UI runs on. Above the plateau onset a level's depth is
set by the clock, so its Elo depends on per-core speed: calibrating on different hardware would
label it for that hardware.

Concurrency 8, as in the scouting (240 games, no time losses). Neither engine ponders, so a game
keeps one core busy at a time, and 8 games stay under the 12 physical cores. All-core load lowers
per-core boost below a lone UI game; that biases clock-bound levels low, so their labels err
conservative (A3).

Rejected: the CI strength lab. It has no Stockfish opponent (a workflow change), builds GCC on
Linux rather than what ships, runs on 2-core runners unlike the UI's machine, and holds 18 of 20
CI slots for hours.

### D3: Seven sampled levels, reached through the provisional inverse

The gauntlet reaches level L by setting `UCI_Elo = ceil(provisional inverse(L))` (design record
D3), so every point stays inside the depth band `1 + int(L)`. With the provisional table
`level = 19 * (elo - 1320) / 1280`:

| L | `UCI_Elo` set | Level played | Pick depth |
|---|---|---|---|
| 0 | 1320 | 0.0000 | 1 |
| 2 | 1455 | 2.0039 | 3 |
| 4 | 1590 | 4.0078 | 5 |
| 7 | 1792 | 7.0063 | 8 |
| 10 | 1994 | 10.0047 | 11 |
| 14 | 2264 | 14.0125 | 15 |
| 19 | 2600 | 19.0000 | 20 |

Denser at the bottom, where one ply is the largest share of strength; 14 and 19 bracket the
expected plateau (pick depth 15 and up sits at or past the 13-15 plies MultiPV 4 reached at
60000+600 in the domain review's probe). Levels between points are interpolated, which is what the
table does anyway.

Rejected: all 20 levels (about 3× the games for points the interpolation already covers); five
points (leaves 10 → 19 as one segment across the plateau onset).

### D4: Two stages, 60 + 240 games per level, fixed size

- **Stage 1** — each level plays 60 games against Stockfish at its own provisional label (the
  `UCI_Elo` it was set to): "does the label hold". Openings 1-30.
- **Stage 2** — each level plays 240 games against Stockfish at the stage-1 estimate, rounded to
  10 and clamped to Stockfish's 1320..3190, so the score sits near 50%, where a game carries the
  most information. Openings 31-150.

Every level plays the same opening slice, so differences between levels are not opening luck.
Fixed size, not SPRT: this estimates a number, it decides nothing. At about 50% with a 20% draw
rate, 300 games give a 95% interval of about ±35 Elo (`1.96 * 695 * sqrt(0.8/4/300)`); stage-1
games against a mismatched opponent carry less, so expect ±35-45, inside the issue's ±50 target.

Each stage is its own quiet window, started on the owner's go, with the stage-1 timings giving a
firm stage-2 estimate before that go is asked for.

**Stop and return to the owner** (no fit is written) when:

- **S1:** level 0 scores under 10% against Stockfish 1320 over all its games. Its Elo would be an
  extrapolation more than ~380 below the anchor's floor, and the table's `front().level == 0.0`
  contract would then need changing, which is outside this slice;
- **S2:** the fit pools any levels below the top block (D5): a lower level measurably outscores a
  higher one mid-scale, which is the non-monotone case design-record A3 left to this slice;
- **S3:** a batch reports a time loss, illegal move or disconnect: discard it (`Measurements/README.md`)
  and rerun once; a second one stops.

### D5: Fit — per-level maximum likelihood, monotone, top block collapsed

1. **Per level:** the Elo R maximising `Σ s·ln E + (1-s)·ln(1-E)` over all that level's games
   (stage 1 and 2 pooled), with `E = 1/(1 + 10^((SF_elo - R)/400))` and s ∈ {0, ½, 1}.
   Stockfish's `UCI_Elo` is taken as exact. The 95% interval is the likelihood-ratio interval
   (ΔlnL = 1.92). Stockfish's own scale error (±100 per its fit's commit) is common to every point,
   so it is stated once and kept out of the per-point bars.
2. **Monotone:** weighted pool-adjacent-violators (weights 1/SE²) over the levels in order. Only a
   block that contains level 19 may pool; any other pooling is S2.
3. **Anchors:** each unpooled level gives `(round(R), level played)`. A pooled top block spanning
   levels a..19 with pooled Elo E gives two anchors, `(E, a)` and `(E + 1, 19.0)`, and drops its
   interior levels: the measured strengths cannot be told apart, and the integer scale cannot hold a
   flat segment, so every `UCI_Elo` from E up plays the top of the block.
4. **Range:** `kMinElo` = level 0's anchor Elo, `kMaxElo` = level 19's. `kMaxElo` is what the top
   level measures (design record D2), not the scouting's uncapped 2600-2700.

Rejected: a smooth curve (a cubic, as Stockfish fits). It would put residuals at every point and
add a shape nobody measured; the table takes measured points directly (design record D3).
Rejected: dropping level 19 into a plateau and letting a lower level carry `kMaxElo`; the
`back().level == kMaxLevel` contract forbids it, and it would hide the strongest setting.

### D6: Tooling — one driver, one fit, one ledger

`Run-UciEloCalibration.ps1 -UciElo <int[]> -SfElo <int[]> -Games <n> -OpeningStart <k>
[-Tc 60+0.6] [-Concurrency 8] [-Smoke] [-SelfTest]` plays each `(UciElo, SfElo)` pair as one
fastchess match, in order, into `logs\elo\calibration-<stamp>\`, with Run-EloMatch's adjudication
(draw 40/8/10, resign 4/800) and the large book (`openings-large.pgn`, sequential from
`-OpeningStart`). It prints a QuietWindow phase plan, exits 1 on any time loss, illegal move or
disconnect, and writes no `local.md` row: a limited engine against Stockfish is not a per-change
result, and `Run-EloMatch.ps1` has no way to name the candidate's `UCI_Elo`.

**The contract between the two scripts is the engine names in the PGN:**
`StratChess-UCI_Elo-<n>` and `SF19-UCI_Elo-<n>`. The fit reads only PGNs, so a resumed or
re-run batch needs no manifest.

`fit_uci_elo_calibration.py <pgn or dir>...` computes the level played from `<n>` with the
provisional table (`--table 1320:0,2600:19` by default, the table the games ran under), runs D5,
reads each move comment's `{score/depth time}` (the parser `compare_lab_depth.py` uses), and
prints the ledger table, the C++ `kAnchors` initializer and `kMinElo`/`kMaxElo`. It exits 1 on
S1 or S2.

Engine options, both sides `Threads=1`, `Hash=64`, `UCI_LimitStrength=true`, `UCI_Elo=<n>`;
StratChess at the default `MultiPV` (the forced 4 is internal). Stockfish is
`StratChessSupport\EngineTesting\stockfish.exe` (`sf_19`, see its PROVENANCE file).

Rejected: extending `Run-EloMatch.ps1`. Its ledger row is the wrong record for these games, and it
refuses to suppress the row by design.

## Assumptions I cannot verify from the code

- **A1: Stockfish 19's `UCI_Elo` still tracks CCRL Blitz.** Its formula was fitted in 2023 (commit
  `a08b8d4`, master vs Stash versions) and is not refitted per release; the commit itself puts
  Stash within ±100 of CCRL. Not verifiable here; the owner chose this anchor knowing it (#985,
  2026-11-21). The ledger states the ±100.
- **A2: Stockfish's fit states no TC.** Checked 2026-11-21: the commit message and `search.h` say
  "CCRL Blitz" and nothing more. 60+0.6 on this machine is in the range of CCRL Blitz
  (2'+1" on slower reference hardware). Unverifiable beyond that.
- **A3: 8 concurrent games bias clock-bound levels low, not high.** Load lowers per-core speed;
  fewer plies at the top means a lower measured Elo for the same setting, so a user at that label
  meets a slightly stronger engine. Not measured; O2 would size it.
- **A4: local fastchess writes `{score/depth time}` move comments.** True of the lab's PGNs that
  `compare_lab_depth.py` parses; the local `fastchess.exe` is a different build. Settled by the
  `-Smoke` run before stage 1; the fit fails loudly on a PGN with no depth.
- **A5: draws as half-points in a logistic model.** Standard for this kind of fit; it ignores the
  pair correlation from colour-swapped openings, which only makes the bars slightly conservative.

## Invariants

- I1. The table stays strictly increasing in both columns, `front() == (kMinElo, 0.0)`,
  `back() == (kMaxElo, 19.0)`: the existing `static_assert`s hold unchanged.
- I2. Only `kAnchors`, `kMinElo`, `kMaxElo`, their comments, docs and the range literal in one test
  change. `level_for_elo` is read only while limiting (`UCIHandler.cpp:492`), so limiting-off play
  is untouched.
- I3. Each anchor is a measured Elo and the fractional level that was actually played (D3's table),
  except the one synthetic `E + 1` of a pooled top block (D5).
- I4. Every sampled level's measured Elo lies within its 95% interval of the final table's label
  for that level (covers dropped interior levels of a pooled block).
- I5. Monotone: below the top block, each sampled level measures above the one before it, and no
  adjacent pair anywhere reverses by more than its combined 95% error.

## Validation

Tier: engine code (a table), plus scripts and a ledger.

- **Tools before games:** `test_fit_uci_elo_calibration.py` on synthetic PGNs: recovers a known R
  within its interval; pools a reversed top pair into `(E, a)`, `(E + 1, 19.0)`; exits 1 on a
  mid-scale reversal (S2) and on level 0 under 10% (S1); fails on a PGN without depth comments.
  The driver's `-SelfTest` covers the name format and the `ceil(inverse)` arithmetic of D3's
  table. Then one `-Smoke` run (20 games, ~6 min) proves the pipeline and A4.
- **The gauntlet's own acceptance:** every point's 95% interval within ±50; I4 and I5 from the fit's
  output; no S3 batch kept.
- **Depth at this TC, per level:** median and 10th/90th-percentile depth of StratChess's moves, and
  the share of moves that reached the pick depth. A level where under half did is clock-bound; the
  ledger marks it, and the plateau's onset is read from that column, not from the Elo alone.
- **The code change:** `Run-Tests.ps1 "[strength]"` and `"[uci]"` with the new range literal;
  `Validate-PrePR.ps1`.
- **No Elo match beyond the gauntlet, no `Compare-SearchEquivalence.ps1`:** I2's single reader
  means the limiting-off search cannot change.

## Cost

- **Size:** over 200 lines, mostly the two scripts and the fit's tests; the engine change is about
  10 lines. Files: 3 new scripts, 1 new ledger, `StrengthLimit.h/.cpp`, one test, three docs.
- **Blast radius:** engine tier for the table (UCI-visible range change), scripts tier for the tools.
- **Review:** one code review of the diff; no domain review (no search or eval change).
- **Game budget (owner's quiet time, local):** 2100 games. From the scouting's 154 engine-seconds a
  game at full strength (240 games, 77 min, concurrency 8), and shorter games where StratChess
  answers early (about 90 s at levels 0-4, 115 s at 7-10, 154 s at 14-19):
  - stage 1: 420 games, **~1.5-2 h**;
  - stage 2: 1680 games, **~5.5-8.5 h** (one overnight window, or split by level);
  - tooling and fit sessions: about one session each side of the games.
- **Optional parts, each priced alone:**
  - **O1, a movetime-1000 spot check** at levels 7 and 19, 240 games each against their stage-2
    opponent: ~1.5-2 h. Tells how far the labels are off for today's UI until clocked games land.
  - **O2, sizing A3:** a `Run-Bench.ps1` at concurrency 1 vs 8 load, ~15 min. Sizes the
    clock-bound bias; changes nothing unless it is large.
  - **Lean stage 2** (180 games instead of 240): saves ~1.5-2 h; bars widen to about ±40-50.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 TC, D2 machine and concurrency, D4 sizes, the per-level results and depths | `Measurements/uci-elo-calibration.md` (table + Row detail) |
| D5 fit rule, the `E + 1` top-block anchor, A1/A2 anchor caveats | comment on `kAnchors` in `StrengthLimit.cpp`; ledger detail |
| D6 name contract, options, adjudication | `Run-UciEloCalibration.ps1` help; fit script docstring |
| Calibrated range, TC and conditions it holds for | `Docs/EngineGuide.md` → Configure a run |
| Headline | `Docs/Changelog.md` |
