# UCI_LimitStrength / UCI_Elo — Design

**Issue:** #985

## Goal

The owner's level sits between the UI's strongest beginner level (`StratChessUI#94`) and the
engine's full strength (~2600-2700 on Stockfish 19's `UCI_Elo` scale, #985 scouting comment).
Nothing plays in that band. The standard UCI pair `UCI_LimitStrength` / `UCI_Elo` covers it in any
GUI. This document covers the option and the root move pick (the implementation slice). Its contract
also fixes what the calibration slice relies on. The owner chose weakened Stockfish 19 as the
calibration anchor (2026-11-21).

## Review focus

- **D2, stopping at the pick depth.** Stockfish searches on to the full clock and discards the
  extra depth. Stopping saves the time but makes low levels move almost instantly. Is anything
  lost besides wall time?
- **D4, the pick formula ported to our score scale.** The formula is linear in score units and
  caps `delta` at one pawn, so it should carry over (approximately: truncation can change ties) with
  a 100 cp pawn. The mate guard departs from Stockfish.
- **D6, the final block's swap.** It is the one change to the UCI output contract from PR #988
  ("`bestmove` is the final block's `multipv 1` move"). It keeps that sentence true by reordering
  the block.

## Scope

**This change will:**

- advertise `UCI_LimitStrength` (check, default `false`) and `UCI_Elo` (spin) and parse both;
- add a pure policy module, `StrengthLimit`: Elo → level, level → pick depth, and the pick;
- in `UciHandler`, force `MultiPV >= 4` and cap the depth while limiting, then replace `bestmove`
  with the pick;
- document both options in `Docs/EngineGuide.md` and the output change in `Docs/EngineContracts.md`.

**This change will not:**

- touch `AIPerplex`, `pvs()` or anything per-node. The search runs unchanged at the MultiPV and
  depth the handler gives it;
- calibrate. The Elo → level table ships provisional, and the calibration slice replaces it (D3);
- add a `Skill Level` option, beginner levels below the floor (UI-side), or a seed UCI option;
- change `Threads` behaviour (D7).

## Decisions

### D1: The policy lives at the UCI layer plus a pure module, not in the search service

`StrengthLimit.h/.cpp` (namespace `StrengthLimit`) holds pure functions. `UciHandler` wires them in:
it sets the effective MultiPV, caps `SearchLimits::depth` and picks inside the completion handler
from `SearchResult` (rank 1 plus `alternatives`). The existing API (`SetMultiPV`, a ranked
`SearchResult`, a depth limit) already gives everything the pick needs.

Rejected: a `SetStrengthLimit()` on `AIPerplex` that picks inside `iterative_deepening()`, as
Stockfish does. It would put a non-search policy into the search service, and the search-equivalence
gate would then rest on a new branch inside the search. The only consumer is UCI; `game` and
`tactical` don't need it.

### D2: Limiting stops the search at the pick depth

For any `go` other than `infinite`, the depth limit becomes
`min(depth the go command would have used, pick_depth(level))`. The clock, `movetime` and `nodes`
still apply as usual. If one stops the search before the pick depth, the pick uses the deepest
completed set.

With `go infinite` the pick-depth cap is not added (an explicit `depth N` still applies), because
`infinite` is analysis, not play. The pick then uses the final set: usually deeper and so stronger,
but a `stop` or a mate can end it before the pick depth. This is documented, not prevented. Today
`go infinite` can already send `bestmove` before `stop` (explicit depth, mate stop, depth 50 cap).
That predates this change and is tracked in #991. This slice neither fixes nor worsens it.

Rejected: Stockfish's form, which picks at `depth == 1 + int(level)` and searches on to the full
clock. Once the pick is fixed, nothing after it changes the move played. The extra search only
burns the clock and CPU. Stopping also makes a level's strength depend on depth, not time control,
whenever the clock reaches the pick depth. That shrinks the calibration's TC drift (#985's second
open question), but only on our side. Stockfish searches on, so its next pick-depth search gets
cutoffs from deep TT entries. Its level L therefore plays above its nominal depth and gets stronger
with time, so the anchor still drifts. Cost: low levels answer almost instantly. A deliberate delay
is UI policy. Pondering is not lost; the engine has none.

**The clock-bound band.** Forcing MultiPV 4 cost 1-3 plies at a fixed time in the domain review's
probe, about 2 as a working estimate: depth 14/13/15 at MultiPV 4 vs 15/15/18 at MultiPV 1 on a
`60000+600` clock, three positions. So in that sample, levels from 13-15 up (`pick_depth = 1 +
int(level)`) are clock-bound at 1+0.6. The plateau is a calibration hypothesis. Those levels all play at the same clock-limited
depth, and only `weakness` still separates them. The scale flattens at the top and may not reach
`kMaxElo`. The calibration slice therefore calibrates at the time control it targets, expects that
plateau, and sets `kMaxElo` to what the top level measures. The scouting's 2600-2700 measured the uncapped
engine, not this ceiling. The calibration slice also owns two other checks: that a lower level
plays worse on average (#985's slice-2 acceptance moves there, since the gauntlet measures it
directly), and the depth reached at the UI's actual time control.

### D3: The Elo → level map is a table, provisional until calibrated

`level_for_elo(int elo) -> double` interpolates piecewise-linearly between `(elo, level)` anchor
points in a `constexpr` array in `StrengthLimit.cpp`, clamped to `[0, kMaxLevel]` with
`kMaxLevel = 19.0`. The advertised range is `[kMinElo, kMaxElo]`, default `kMinElo` (Stockfish's
convention). The slice ships a provisional two-point table, `(1320, 0.0)` to `(2600, 19.0)`:
`kMaxElo = 2600` comes from the scouting, and 1320 is Stockfish's floor.

**Contract for the calibration slice:**

- the table must stay strictly increasing, so a gauntlet script can reach level L by setting
  `UCI_Elo` to the provisional inverse. `UCI_Elo` is an integer, so the script sets
  `ceil(inverse(L))`, which lands on or just above L and keeps the depth band `1 + int(L)`
  (provisional inverse of level 1 is 1387.37: 1387 gives level 0.9945 and depth 1, 1388 gives
  1.0094 and depth 2). It records the integer Elo and the resulting fractional level per point, and
  the fit keeps the depth bands;
- calibration replaces the anchor points and both range ends, and changes nothing else.

Until then `docs` call the values provisional.

Rejected: Stockfish's cubic. It was fitted to Stockfish's search against the Stash engine, so on
ours it is only a different arbitrary label. A table also takes measured points directly.

### D4: The pick is Stockfish's formula in our centipawns

Inputs: the ranked set `S` (rank 1 first, sorted best first, size `n`), the level `l`, and a
generator. With `top = S[0].score`, `weakness = 120 - 2*l`, `delta = min(top - S[n-1].score, 100)`
(one pawn, `g_iPieceValues` pawn), and `r_i` uniform in `[0, int(weakness))`, each candidate scores

`adjusted_i = S[i].score + int(weakness * (top - S[i].score) + delta * r_i) / 128`

(The sum is computed in `double`, truncated to `int` once, then divided by 128 as an integer.)

and the highest wins, the later rank on ties (Stockfish's `>=`). The first term *compresses* gaps:
it does not penalise them. A move's adjusted gap is `(128 - weakness)/128` of its real gap. So a
candidate can win only when `gap <= delta * (int(weakness) - 1) / (128 - weakness)`. That is the
exact maximum, found by brute force of the integer formula in the domain review: 1487 cp at level 0
and 176 cp at level 19 for `delta = 100`. Rank 1 with `r = 0` loses nothing to the truncation, so
there is no slack term. The tests check this bound (I5). Scores stay far inside `int`
(the largest intermediate sum is about 7.2 million).

**Mate guard (a departure from Stockfish).** The bound applies to gaps, and two mate scores can sit
close together. At level 0, `[29999, 29997]` with draws `[0, 119]` adjusts to `[29999, 30000]`, a
slower mate. Likewise `[-29995, -29999]` adjusts to `[-29995, -29992]`, a faster loss. So:

- if `|top| >= Mate_Threshold`, the pick returns rank 1: a proven mate is played as found, and
  so is the slowest proven loss;
- otherwise candidates with `score <= -Mate_Threshold` are dropped before the formula. A finite
  top already beats them by the bound, so this only makes the rule explicit.

Rejected: Stockfish's behaviour, which can play a slower mate each move and drift into a 50-move or
repetition draw. A won mate thrown away reads as a bug to a human opponent, and the guard costs
nothing outside mate positions, so it barely moves calibration.

`pick_depth(l) = 1 + int(l)`; the fractional level enters only `weakness`. Both as in Stockfish.

`S` is the whole set the search ranked. A user MultiPV above 4 therefore can change or weaken play
at the same `UCI_Elo`, both through the extra candidates and the budget each set costs (Stockfish
too). Calibration runs at the default MultiPV.

`#985`'s body describes the first term as a penalty, which is wrong: it compresses gaps (Assumption
A1).

`r_i` comes from `std::uniform_int_distribution<int>(0, int(weakness) - 1)`. That is this port's
choice; Stockfish's `% int(weakness)` carries a negligible modulo bias.

Rejected: a new formula. Stockfish's is the one with a calibration history. Its constants transfer
approximately: every term is linear in score units and `delta` is capped at one pawn in the same
units, though the truncation can change ties (`[1, 0]` vs `[2, 0]` at weakness 82).

### D5: Randomness comes from a per-search seed drawn on the controlling thread

`UciHandler` owns a `std::mt19937_64`, seeded from `std::random_device` at construction. `cmd_go`
draws one `uint64_t` from it only when limiting. It captures that seed **by value** in the completion
handler, together with the user MultiPV, the enabled flag and the level, because a previous
search's completion can still run after `IsSearching()` clears. `StrengthLimit::pick`
takes the seed and builds its own generator. So no mutable state crosses threads, and a test fixes
the pick by reseeding the handler's generator through `UciHandlerTestFixture`
(`STRAT_ENABLE_TEST_ACCESS`) or by calling `pick` with a seed.

Rejected: a UCI seed option, which widens the public surface for tests only. Also rejected: a
`static` generator as in Stockfish, which leaves the pick untestable. Also rejected: seeding lazily on
first use. One `random_device` read per process costs nothing measurable, and laziness adds state.

**Options.** `UCI_LimitStrength` takes `true`/`false`; any other value is ignored with an
`info string`. `UCI_Elo` clamps, saturates and ignores bad input exactly as `MultiPV` does (PR #988).
Both keep their values across `ucinewgame`. Names match exactly as advertised, like the existing
options. Rejected: case-insensitive matching for these two alone; it would make the parser
inconsistent, and a change across all options is out of scope.

### D6: Output while limiting

- **Effective MultiPV** = `max(user MultiPV, 4)` while limiting, else the user's value. It is
  recomputed whenever `setoption` changes `MultiPV` or `UCI_LimitStrength`. That is the only place
  it can change: `setoption` is idle-only, and `cmd_go` may overlap a finishing search.
- **Iteration lines** print the user's ranks only, ranks 1..min(user N, set size), tagged exactly as
  at that user N. The engine's real analysis stays visible, as in Stockfish. Both the per-depth
  observer and the final block decide the `multipv` tag from the **user's** N, not
  `ai_->MultiPV()`, and pass only the first `user N - 1` alternatives. `UCIHandler.cpp:450` reads
  `ai_->MultiPV()` today, and `send_ranked_info` asserts at `:110` that an untagged call has no
  alternatives. Without the change, user N = 1 asserts in Debug and prints `multipv` tags in Release.
- **Final block:** if the pick is not rank 1, its line swaps with rank 1, so the block's
  `multipv 1` is the picked line with its own exact score and full PV, and `bestmove` stays the
  block's `multipv 1` move. While limiting, this drops #988's other rule, that the final block equals
  the last iteration's block. Both public output sections (`EngineGuide.md`, `EngineContracts.md`)
  state the exception. At user N = 1 the block is the picked line alone. When the pick ranks below the user's N, rank 1
  moves into an unprinted slot, so the true best line drops out of the final block. That is intended:
  the block shows what was played. Stockfish also swaps the pick
  into `rootMoves[0]` and re-sends the PV.
- **No pick** (empty `alternatives`: one legal move, the depth-1 fallback, an emergency move, a
  terminal root) → `bestmove` is rank 1, unchanged. A terminal root keeps `pv 0000` / `bestmove
  0000`.

Rejected: leaving the final block as searched, with `bestmove` differing from its rank 1. That
breaks PR #988's stated contract, and a GUI would show a line the engine didn't play.

### D7: Threads are left alone

Ranks come from the main thread (helpers stay `N = 1`), so the pick is well-defined at any
`Threads`. More threads still change the TT the ranks read, so strength at a level may move
slightly. Calibration runs at `Threads=1`, which `EngineGuide.md` states.

Rejected: forcing `Threads=1` while limiting. It would silently override a user setting to gain
reproducibility no consumer has asked for.

## Assumptions I cannot verify from the code

- **A1: Stockfish's code is as D2/D4 describe.** Verified 2026-11-21 against `master`
  `src/search.h`/`search.cpp`, read verbatim, and by the cross-agent review against the `sf_19` tag
  (`search.cpp` L1863-1896 pick, L564-570 swap). The `sf_19` swap does not itself clear the
  PV-sent flag; `master` does. We re-send by construction (D6), so the difference doesn't matter here:
  - the `Skill` struct: Elo range 1320-3190, a fitted cubic, `time_to_pick` = `depth == 1 + int(level)`,
    `enabled()` = `level < 20`;
  - the `multiPV = max(multiPV, 4)` raise;
  - `Skill::pick_best` (`search.cpp` ~l. 2088), the D4 formula;
  - the post-loop swap into `rootMoves[0]` with a PV re-send;
  - no depth cap.

  `pick_best` takes `top` and the minimum as a max/min over the set rather than the first and last
  rank, because tablebase roots order by rank, not score. Our set is always score-sorted, so first and
  last are the same. A later Stockfish change does not bind us: this port is a snapshot.
- **A2: GUIs and fastchess accept a `bestmove` that differs from earlier iterations' `multipv 1`.**
  This is normal for any engine whose best move changes late, and Stockfish does it while limiting.
  Settled by the calibration slice's gauntlet (fastchess flags illegal or unexpected moves).
- **A3: the shape of our score distribution.** Below the cap the pick depends only on gap/delta, and
  the cap is one material pawn in both engines, so the constants carry over (domain review,
  finding 6). What does not carry over is the shape: our eval has little positional range, so near-ties
  may be more common. That makes the pick more random among nearly equal moves, which costs little,
  so the direction of the effect on strength is unknown. Calibration absorbs it into the table. It
  matters only if strength is non-monotone in level, which the calibration slice checks.

## Invariants

- I1. `UCI_LimitStrength=false` (the default): search unchanged, and output the same shape as today
  apart from the two new `option` lines. `Compare-SearchEquivalence.ps1` normalises volatile
  fields (timing, hashfull, seldepth, banner, final PV tail), so the existing `[uci]` format tests
  carry the shape.
- I2. Limiting: `bestmove` is a legal root move and is one of the ranked set's moves (a terminal
  root's `0000` excepted).
- I3. Limiting: the final block's `multipv 1` line is the picked line, its move, exact score and
  full PV, and its move equals `bestmove` (PR #988's `bestmove` rule, kept).
- I4. Limiting, not `infinite`: `depth_completed <= pick_depth(level_for_elo(UCI_Elo))`.
- I5. The pick never picks a candidate whose gap to rank 1 exceeds
  `delta * (int(weakness) - 1) / (128 - weakness)` (D4's exact bound). A mate-scored rank 1 is
  always picked, and with a finite rank 1 a losing-mate candidate never is.
- I6. Within one build, the same seed, level and ranked set give the same pick. End to end, the
  same seed gives the same `bestmove` only with fresh search state (`ucinewgame`), `Threads=1` and
  a deterministic limit (`go depth`): the TT persists between searches, and a clock changes the set.
- I7. `level_for_elo` is non-decreasing, clamps at both ends, and the table is strictly increasing
  (D3's contract).

## Validation

Tier: engine code, UCI-facing.

- I1: `Compare-SearchEquivalence.ps1` IDENTICAL against the merge base (limiting off), plus the
  existing `[uci]` tests unchanged.
- I2, I3, I4: new `StratChessTests/UCIStrengthLimitTests.cpp` (`[uci][strength]`) at a fixed seed:
  - both option lines and their `setoption` cases: clamp, saturate, ignore, bad boolean, retention
    across `ucinewgame`;
  - toggling limiting off with user `MultiPV > 1` restores that N's output, and changing user
    MultiPV while limiting changes the visible rank count;
  - **the swap, at a seed chosen so the pick is not rank 1** (the test asserts that precondition):
    - at user N = 3 with the pick at rank 4, the final block's `multipv 1` has the picked line's
      score and full PV from the same depth's set;
    - ranks 2 and 3 are preserved, and the original rank 1 is displaced. Get the hidden line from a
      fixed-depth control search at limiting off and MultiPV 4, after `ucinewgame`, or from a result
      fixture;
    - at user N = 1 the same tuple, with no `multipv` field;
  - `go infinite` + `stop` is not capped at the pick depth;
  - a one-legal-move root and the depth-1 interrupted-set fallback (reuse the `UCIMultiPVTests.cpp`
    node-budget scenario): no pick, `bestmove` is rank 1.
- I5, I6, I7: `StrengthLimitTests.cpp` (`[strength]`), pure unit tests:
  - the bound over many seeds on synthetic sets whose spread exceeds 100 cp, so `delta` is capped.
    With a smaller spread every gap is at most `delta`, which is under the bound at every level, so
    the test could not fail;
  - mates: two winning mates (rank 1 always kept), two losing mates (rank 1 kept), and a finite
    rank 1 against a losing mate (never picked);
  - ties;
  - a set of size 1;
  - table endpoints and monotonicity, plus `level_for_elo` and `pick_depth` either side of a
    depth boundary (1387 → depth 1, 1388 → depth 2).
- Falsify: break the swap and an I3 test fails; drop the depth cap and an I4 test fails.
- **No bench and no Elo match.** No per-node code changes, and default-off is proven by
  equivalence. Strength while limiting is the calibration slice's measurement.

## Cost

- **Size:** over 200 lines, mostly tests. One new module (2 files), two new test files,
  `UCIHandler.h/.cpp`, `EngineGuide.md`, `EngineContracts.md`, `Changelog.md`.
- **Blast radius:** the engine tier. The UCI contract gains two options and the D6 final-block rule.
- **Review:** one full code review; `search-reviewer` on the diff is optional (no search code).
- **Optional parts:** none.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 why the policy is at the UCI layer; D5 seed handling | header comment in `StrengthLimit.h` |
| D2 stop at pick depth, `infinite` uncapped | comment at the depth cap in `UCIHandler.cpp`; `EngineGuide.md` options section |
| D3 provisional table and its contract | comment on the table in `StrengthLimit.cpp`; `EngineGuide.md` |
| D4 formula, the compression reading and the bound | comment on `StrengthLimit::pick` |
| D6 output rules | `EngineContracts.md` UCI section; `EngineGuide.md` output section |
| D7 Threads, calibration at Threads=1 | `EngineGuide.md` |
| the slice | `Docs/Changelog.md` |
