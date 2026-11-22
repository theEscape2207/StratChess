# UCI_Elo calibration against Stockfish 19 — Design

**Issue:** #985 (calibration slice; the option-and-pick slice shipped in PR #992, `bdb0f838`)

## Goal

`UCI_Elo` is a label until it is measured. `StrengthLimit.cpp` ships a provisional two-point
table (1320 → level 0.0, 2600 → level 19.0), so `UCI_Elo=1800` today means "level 7.1", not "plays
like 1800". This slice measures what each level actually scores against Stockfish 19 at
`UCI_LimitStrength` (the anchor the owner chose on 2026-11-21) and replaces the anchor points and
the advertised range with the measured ones. Nothing else in the policy changes.

## Review focus

- **D5, the fit's top-suffix rule.** A plateau at the top (expected: levels from about 13-15 up are
  clock-bound at 60+0.6, design record D2) is written as two integer anchors one Elo apart, and only
  levels 14 and 19 may pool. Is that the right way to keep the shipped integer table strictly
  increasing while telling the truth about a plateau?
- **D4/D5, precision for the budget.** Seven levels, 300 games each, intervals from an
  opening-pair bootstrap. Does that buy ±50 per point, and does the fit fail closed when it doesn't?
- **A1/A2, the anchor's own drift.** Stockfish's `UCI_Elo` was fitted in 2023 at 120+1; we play it
  at 60+0.6.

## Scope

**This change will:**

- add `Scripts/Run-UciEloCalibration.ps1` (the gauntlet driver; codifies the 2026-11-20 scouting run)
  and `Scripts/fit_uci_elo_calibration.py` with `Scripts/test_fit_uci_elo_calibration.py`;
- play the gauntlet locally in two stages, each started by the owner (D4);
- replace `kAnchors` in `StratEngine/StrengthLimit.cpp` and `kMinElo`/`kMaxElo` in
  `StrengthLimit.h`, rewrite their "provisional" comments, and re-pin the tests that rest on the
  provisional table (Validation);
- record the fit in a new ledger, `Measurements/uci-elo-calibration.md`, listed in
  `Measurements/README.md`; update `Docs/EngineGuide.md` (range, TC, and "provisional" removed from
  both UCI option passages, `:85-94` and `:317-318`) and `Docs/Changelog.md`.

**This change will not:**

- touch the pick (`pick`, `pick_depth`, `weakness`, the mate guard), the forced MultiPV 4, the
  depth cap, or anything in the search. If the data says a level plays badly, that is a new issue;
- extend the scale below level 0. The owner's note of 2026-11-21 expects the floor may need
  extending (weakness beyond level 0's maximum at depth 1), pending their own games at
  `UCI_Elo=1320`. That is a policy change with its own design and its own calibration points; this
  slice's level-0 measurement is its input;
- calibrate for `Threads` > 1 or a user `MultiPV` > 1;
- fix #991 (`go infinite`); fastchess never sends it;
- add a CCRL-engine anchor (option B of the triage).

## Decisions

### D1: 60+0.6 on both sides

Owner decision in the planning session, 2026-11-21: calibrate for clocked play. StratChessUI
offers `go depth` and `go movetime` today (1000 ms the fixed-time default); clocked games are
planned soon. 60+0.6 is also the scouting's TC, so the scouting result carries over as a
cross-check. Both engines play the same clock, so a label means "scores 50% against SF19 at this
`UCI_Elo`, both at 60+0.6 on this machine" — parity with weakened SF19 under these conditions, not a
CCRL rating (A1, A2).

Rejected: `movetime 1000` as the target; it stays an optional spot check (Cost, O1). Rejected:
playing Stockfish at its fit's 120+1 (A2): the labels then mean "beats a Stockfish given twice the
time", which no user of the UI meets.

### D2: Local, on the owner's machine, shipping binary, concurrency 8

The candidate is the clang-cl Release build of this branch (`build/windows-clang-cl/StratChessEvolved.exe`,
the binary the UI runs), on the machine the UI runs on. Above the plateau onset a level's depth is
set by the clock, so its Elo depends on per-core speed: calibrating on different hardware would
label it for that hardware.

Concurrency 8, as in the scouting (240 games, no time losses, uncapped StratChess). Neither engine
ponders, so a game mostly keeps one core busy, and 8 games stay under the 12 physical cores.
Concurrency is a condition of the benchmark: load slows both engines, and the sign of the relative
Elo shift is unknown (A3). The ledger records concurrency and machine with the result. If the smoke
run or stage 1 shows a time loss or stall, the run drops to the suite's 6.

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

Every level plays the same opening slice, so differences between levels are not opening luck (and
the intervals on those differences must account for it, D5). Fixed size, not SPRT: this estimates a
number, it decides nothing. Under independence, 300 games at about 50% with 20% draws give about
±35 Elo (`1.96 * 695 * sqrt(0.8/4/300)`); 60 stage-1 games give about ±79, enough only to place
the stage-2 opponent. Pair correlation can move the real figure either way, so ±50 is a gate the
fit checks (D5, G1), not a promise. A miss is reported as inconclusive; the budget stays fixed.

Each stage is its own quiet window, started on the owner's go. Stage 1 ends with a stage-1 report
(D6): the stage-2 opponents, saturated scores and the measured game times that price stage 2.

**Stop and return to the owner** (no initializer is emitted) when:

- **S1, floor:** evaluated on the stage-1 report and again at the fit, over all level-0 games (both
  stages play Stockfish 1320 if the clamp binds). Level 0 below 1320 is an extrapolation past the
  anchor's floor: the ledger flags it, and it ships as `kMinElo` only while level 0 scores at least
  10% (`400·log10(0.1/0.9) ≈ −382`, so `kMinElo` ≥ about 938). Below that, stop: placing the floor
  is then the owner's pending floor decision, not this fit's.
  **Owner decision 2026-11-22 (after the fit):** level 0 measured 8.5% (907, [826, 971]) and ships
  as `kMinElo` anyway, flagged as an extrapolation; extending the floor stays a separate decision;
- **S2, order:** the fit would pool or reverse levels outside the top suffix (D5). Stopping there is
  a policy choice: the non-monotone case design-record A3 left to this slice is worth a human look
  even when noise could explain it;
- **S3, harness:** a batch reports a time loss, illegal move, disconnect, stall or a failed runner:
  discard the attempt (`Measurements/README.md`) and rerun once; a second one stops.

### D5: Fit — per-level estimate, opening-pair intervals, an integer table that fails closed

1. **Point estimate per level:** the Elo R maximising `Σ s·ln E + (1-s)·ln(1-E)` over all that
   level's games (stage 1 and 2 pooled), with `E = 1/(1 + 10^((SF_elo - R)/400))` and s ∈ {0, ½, 1}.
   Stockfish's `UCI_Elo` is taken as exact.
2. **Intervals from opening pairs:** a bootstrap over opening IDs, stratified by stage, 10 000
   resamples at a fixed seed. One resample of opening IDs is applied to every level at once, so the
   intervals on adjacent *differences* carry the shared openings. The 95% interval is the
   percentile interval. These are match-sampling intervals only; the anchor's own scale drift is
   unmeasured and stated beside them (A1).
3. **Monotone:** weighted pool-adjacent-violators (weights 1/variance) over the levels in order.
   The only block allowed to pool is the predeclared top suffix {14, 19}.
4. **The integer table:** each unpooled level gives `(round(R), level played)`. If 14 and 19 pool,
   or their rounded estimates collide, with pooled Elo E they give `(round(E) - 1, 14.0125)` and
   `(round(E), 19.0)`: every `UCI_Elo` from `round(E)` plays level 19, and the levels between 14
   and 19 are reachable only through the one-Elo step, which the ledger states.
5. **Range:** `kMinElo` = level 0's anchor, `kMaxElo` = level 19's. `kMaxElo` is what the top level
   measures (design record D2), not the scouting's uncapped 2600-2700.

**Gates; any failure prints the report but no initializer:**

- **G1, precision:** each level's interval endpoints lie within 50 Elo of its point estimate.
  **Owner waiver 2026-11-22:** G1 binds only on levels whose stage-2 opponent was not clamped at
  Stockfish's 1320 floor; levels 0 and 2 (clamped, about ±70 and ±50) ship with their intervals stated.
- **G2, order:** no pooling or rounded collision outside {14, 19} (S2); no adjacent difference
  whose interval lies wholly below zero.
- **G3, fit:** reading the *emitted* integer table back at each level played gives a label inside
  that level's interval. This covers the synthetic `round(E) - 1` onset and the rounding.
- **G4, contract:** emitted Elo keys strictly increase; `front()` is level 0.0, `back()` level 19.0.
- **S1** as D4 states it.

Rejected: a smooth curve (a cubic, as Stockfish fits). It would put residuals at every point and
add a shape nobody measured; the table takes measured points directly (design record D3).
Rejected: letting a pooled block grow downward past 14. A plateau reaching level 10 contradicts
the depth evidence (D3) and would be S2's case, not a plateau. Rejected: dropping level 19 so a
lower level carries `kMaxElo`; the `back().level == kMaxLevel` contract forbids it.

### D6: Tooling — one driver, one fit, one ledger

`Run-UciEloCalibration.ps1 -Stage <1|2> -UciElo <int[]> -SfElo <int[]> -Games <n> -OpeningStart <k>
[-Tc 60+0.6] [-Concurrency 8] [-Smoke] [-SelfTest]` plays each `(UciElo, SfElo)` pair as one
fastchess match, in order, with Run-EloMatch's adjudication (draw 40/8/10, resign 4/800) and the
large book (`openings-large.pgn`, sequential, one-based `-OpeningStart`). It prints a QuietWindow
phase plan and writes no `local.md` row. `Run-EloMatch.ps1` could pass `option.UCI_Elo` through
`-CandidateOptions`, but its row names the candidate by commit only and cannot be suppressed by
design, and a limited engine against Stockfish is not a per-change result.

**One directory per attempt:** `logs\elo\calibration\<stamp>-s<stage>-elo<UciElo>-sf<SfElo>\`
holds fastchess's config, log and PGN. On a clean finish only, the driver writes `run.json`:
`complete`, `smoke`, stage, `UciElo`, `SfElo`, the candidate commit and SHA-256 of both exes, the
book path and SHA-256, the opening range, both engines' options, TC, concurrency and machine name.
A failed attempt (S3) gets no `run.json` and is never read.

`fit_uci_elo_calibration.py <attempt dir>...` reads only attempts with `complete: true` and
`smoke: false`, and fails unless every (level, stage, opening) in the plan has exactly one
colour-swapped pair, all attempts share one candidate hash, book hash and TC. It computes the level
played from `UciElo` with the provisional table (`--table 1320:0,2600:19` by default, the table
the games ran under), runs D5 and prints the ledger table, the gates and, if they pass, the C++
`kAnchors` initializer and `kMinElo`/`kMaxElo`. Two more modes:

- `--stage1-report`: the stage-2 opponent per level (D4), each score with saturated ones flagged,
  and game times;
- `--accept-floor`: the owner's 2026-11-22 waiver. S1 and G1 on floor-clamped levels print as NOTE
  instead of FAIL, so the initializer is emitted; G2-G4 still bind;
- `--smoke-check`: on the smoke attempt alone, run at level 0: every StratChess move comment parses
  and shows depth 1 (proof `UCI_LimitStrength`/`UCI_Elo` reached the engine), and no move lacks a
  comment.

Depth per level comes from each StratChess move comment's `{score/depth time}` (the parser
`compare_lab_depth.py` uses), attributed by side to move, never by position in the comment list.
Moves whose score is a mate are reported apart, since a mate stop ends the search below the pick
depth; moves with a missing or unparsable comment are counted, not dropped silently.

Engine options, both sides `Threads=1`, `Hash=64`, `UCI_LimitStrength=true`, `UCI_Elo=<n>`;
StratChess at the default `MultiPV` (the forced 4 is internal). Stockfish is
`StratChessSupport\EngineTesting\stockfish.exe` (`sf_19`, see its PROVENANCE file).

## Assumptions I cannot verify from the code

- **A1: SF19's `UCI_Elo` scale.** Its formula was fitted in 2023 (commit `a08b8d4`, master at skill
  levels vs Stash versions rated by CCRL) and is not refitted per release; SF19 at a given skill may
  sit elsewhere. The commit's "±100" is how closely the Stash versions matched their CCRL ratings,
  not a bound on SF19's scale. The tagged cubic also maps 1320-1346 to the same skill 0. Not
  verifiable here; the owner chose this anchor knowing it (#985, 2026-11-21). The ledger says the
  labels are parity with weakened SF19 under D1's conditions.
- **A2: the anchor's fit TC is 120+1, ours is 60+0.6.** Verified 2026-11-21: the commit's README
  diff reads "calibrated at a time control of 120s+1.0s and anchored to +- 100 Elo to CCRL Blitz".
  At half the time Stockfish at a given `UCI_Elo` likely plays somewhat below its label, so our
  labels read against SF19-at-60+0.6, not against SF's nominal scale. No hardware-equivalence claim
  is made.
- **A3: concurrency 8 is a benchmark condition.** Load can slow both engines; the sign of the
  relative Elo shift is unknown, and a fixed-depth bench could detect a throughput change but not
  size the Elo effect. Recorded with the result, not corrected for.
- **A4: local fastchess writes `{score/depth time}` move comments for the limited engine.** True of
  the lab PGNs `compare_lab_depth.py` parses; the local `fastchess.exe` is a different build and
  the configuration is new. Settled by `--smoke-check` before stage 1.
- **A5: the fractional-Bernoulli point estimate.** Standard for a mean score against known
  opponents; the bootstrap (D5) carries the uncertainty, so the estimate's own likelihood interval
  is not used.

## Invariants

- I1. The table stays strictly increasing in both columns, `front() == (kMinElo, 0.0)`,
  `back() == (kMaxElo, 19.0)`: the existing `static_assert`s hold unchanged.
- I2. Only `kAnchors`, `kMinElo`, `kMaxElo`, their comments, docs and the tests pinned to the
  provisional table change. `level_for_elo` is read only while limiting (`UCIHandler.cpp:492`), so
  limiting-off play is untouched.
- I3. Each anchor is a rounded measured Elo and the fractional level actually played (D3's table),
  except the synthetic `round(E) - 1` onset of a pooled {14, 19} suffix (D5).
- I4. The emitted integer table, read back at each level played, gives a label inside that level's
  95% interval (G3).
- I5. Monotone: no pooling outside {14, 19}, and no adjacent difference significantly below zero
  (G2).
- I6. Every fitted game comes from exactly one complete, non-smoke attempt, with one candidate
  binary, book and TC across the fit (D6).

## Validation

Tier: engine code (a table), plus scripts and a ledger.

- **Tools before games:** `test_fit_uci_elo_calibration.py` on synthetic attempt dirs: recovers a
  known R inside its interval; pairs each opening's two colours, and a paired fixture whose two games
  are perfectly correlated widens the interval against an independent one; a clamped, mismatched
  opponent fixture misses G1; a reversed {14, 19} pair emits `(round(E) - 1, 14.0125)`,
  `(round(E), 19.0)`; a rounded tie in the suffix takes the same path; a suffix that would grow
  down to 10 fails G2; a pooled pair whose synthetic onset falls just outside 14's interval fails G3;
  level 0 under 10% fails S1, and passes under `--accept-floor` with the NOTE still printed, while a
  G1 miss on an unclamped level still fails under it; a duplicate pair, a smoke attempt, an attempt without `run.json` and a
  mixed candidate hash are each refused; a comment-less StratChess move is counted and attributed by
  side. The driver's `-SelfTest` covers the attempt-dir name and the `ceil(inverse)` arithmetic of
  D3's table.
- **Smoke:** one `-Smoke` attempt (20 games at level 0 vs SF 1320, a few minutes) and
  `--smoke-check` on it (A4, options applied, depth attribution, `bestmove` is the picked move:
  fastchess's "bestmove does not match PV" warning count is read from the log).
- **The gauntlet's own acceptance:** G1-G4 and S1 from the fit; no S3 attempt kept.
- **Depth at this TC, per level:** median and 10th/90th-percentile depth of StratChess's non-mate
  moves, the share that reached the pick depth, and the mate and missing-comment counts. A level
  where under half reached it is marked clock-bound. That is a depth statistic; flattening is read
  from the Elo column and its difference intervals, reported apart.
- **The code change:** re-pin the tests that rest on the provisional table, keeping what they test:
  - `StrengthLimitTests.cpp:57-65`: the 1387/1388 depth boundary moves to the two integers around a
    depth transition in the new table, computed from `kAnchors` rather than hard-coded;
  - `UCIStrengthLimitTests.cpp`: the range literal (`:112`), the depth-5 fixture at 1590 (`:246-261`
    and the final-block tests at `:293-326`) moves to the new anchor for level 4.0078, and the
    option tests' in-range value (2000, `:129-143`) to one inside the new range; the fixed-depth
    control and the seeded non-rank-1 precondition stay;
  - then `Run-Tests.ps1 "[strength]"` and `"[uci]"`, and `Validate-PrePR.ps1`.
- **No Elo match beyond the gauntlet, no `Compare-SearchEquivalence.ps1`:** I2's single reader
  means the limiting-off search cannot change.

## Cost

- **Size:** over 200 lines, mostly the two scripts and the fit's tests; the engine change is about
  10 lines. Files: 3 new scripts, 1 new ledger, `StrengthLimit.h/.cpp`, one test, three docs.
- **Blast radius:** engine tier for the table (UCI-visible range change), scripts tier for the tools.
- **Review:** one code review of the diff; no domain review (no search or eval change).
- **Game budget (owner's quiet time, local):** 2100 games. Measured input: the scouting ran 240
  games in 77m31s at concurrency 8, about 155 game-slot seconds a game at full strength. Forecast
  input: shorter games where StratChess answers early, about 90 s at levels 0-4, 115 s at 7-10 and
  155 s at 14-19. Before per-attempt startup and drain:
  - stage 1: 420 games, **~1.7 h** forecast;
  - stage 2: 1680 games, **~6.7 h** forecast, **~9 h** if every game runs at the scouting rate (one
    overnight window, or split by level); priced again from stage 1's measured times before its go;
  - smoke: a few minutes;
  - tooling and fit sessions: about one session each side of the games.
- **Optional parts, each priced alone:**
  - **O1, a movetime-1000 spot check** at levels 7 and 19, 240 games each against their stage-2
    opponent: ~1.5-2 h. Tells how far the labels are off for today's UI until clocked games land.
  - **O2, a level-12 point** (60 + 240 games, ~1.3 h forecast), offered only if the fit shows 14 and
    19 pooled and 10 not clock-bound, so the plateau onset between 10 and 14 would change the table.
  - **Lean stage 2** (180 games instead of 240): saves ~1.7 h forecast; G1 is then likelier to miss
    (about ±39 under independence at 50%, before pair correlation and the floor's extrapolation).

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 TC, D2 machine and concurrency, D4 sizes, the per-level results and depths | `Measurements/uci-elo-calibration.md` (table + Row detail) |
| D5 fit rule, the `round(E) - 1` onset, A1/A2 anchor caveats | comment on `kAnchors` in `StrengthLimit.cpp`; ledger detail |
| D6 attempt-dir and `run.json` contract, options, adjudication | `Run-UciEloCalibration.ps1` help; fit script docstring |
| Calibrated range, TC and conditions it holds for; each measured level's Elo with its 95% interval, and the caveats a player reads a label with (floor extrapolated below SF 1320, levels between points interpolated and unmeasured, A1/A2 anchor drift) | `Docs/EngineGuide.md` → Configure a run |
| Headline | `Docs/Changelog.md` |
