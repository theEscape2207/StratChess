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
  caps `delta` at one pawn, so it should carry over unchanged with a 100 cp pawn. Check that, and
  check the mate-score cases.
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

With `go infinite` the depth is not capped. UCI forbids `bestmove` before `stop`, and `infinite` is
analysis, not play. The pick then uses the final set, which is deeper and so stronger. This is
documented, not prevented.

Rejected: Stockfish's form, which picks at `depth == 1 + int(level)` and searches on to the full
clock. Once the pick is fixed, nothing after it changes the move played. The extra search only
burns the clock and CPU. Stopping also makes a level's strength depend on depth, not time control,
whenever the clock reaches the pick depth. That shrinks the calibration's TC drift (#985's second
open question). Cost: low levels answer almost instantly. A deliberate delay is UI policy.

### D3: The Elo → level map is a table, provisional until calibrated

`level_for_elo(int elo) -> double` interpolates piecewise-linearly between `(elo, level)` anchor
points in a `constexpr` array in `StrengthLimit.cpp`, clamped to `[0, kMaxLevel]` with
`kMaxLevel = 19.0`. The advertised range is `[kMinElo, kMaxElo]`, default `kMinElo` (Stockfish's
convention). The slice ships a provisional two-point table, `(1320, 0.0)` to `(2600, 19.0)`:
`kMaxElo = 2600` comes from the scouting, and 1320 is Stockfish's floor.

**Contract for the calibration slice:**

- the table must stay strictly increasing, so a gauntlet script can reach level L by setting
  `UCI_Elo` to the provisional inverse;
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
candidate can win only when `gap < delta * weakness / (128 - weakness)`, which gives a bound the
tests check (I5; the one truncation adds at most 1 cp of slack). Scores stay far inside `int`
(|score| <= 30000, weakness <= 120). A mate score's gap dwarfs `delta`, so a mate at rank 1 is
always kept, and a mated rank is never chosen over a non-mated one.

`pick_depth(l) = 1 + int(l)`; the fractional level enters only `weakness`. Both as in Stockfish.

`#985`'s body describes the first term as a penalty, which is wrong: it compresses gaps (Assumption
A1).

Rejected: a new formula. Stockfish's is the one with a calibration history. Its constants transfer
because every term is linear in score units and `delta` is capped at one pawn in the same units.

### D5: Randomness comes from a per-search seed drawn on the controlling thread

`UciHandler` owns a `std::mt19937_64`, seeded from `std::random_device` at construction. `cmd_go`
draws one `uint64_t` from it and captures the value in the completion handler. `StrengthLimit::pick`
takes the seed and builds its own generator. So no mutable state crosses threads, and a test fixes
the pick by reseeding the handler's generator through `UciHandlerTestFixture`
(`STRAT_ENABLE_TEST_ACCESS`) or by calling `pick` with a seed.

Rejected: a UCI seed option, which widens the public surface for tests only. Also rejected: a
`static` generator as in Stockfish, which leaves the pick untestable.

### D6: Output while limiting

- **Effective MultiPV** = `max(user MultiPV, 4)` while limiting, else the user's value. It is
  recomputed whenever `setoption` changes `MultiPV` or `UCI_LimitStrength`. That is the only place
  it can change: `setoption` is idle-only, and `cmd_go` may overlap a finishing search.
- **Iteration lines** print the user's ranks only, ranks 1..min(user N, set size), tagged exactly as
  at that user N. The engine's real analysis stays visible, as in Stockfish.
- **Final block:** if the pick is not rank 1, its line swaps with rank 1, so the block's
  `multipv 1` is the picked line with its own exact score, and `bestmove` stays the block's
  `multipv 1` move. At user N = 1 the block is the picked line alone. Stockfish also swaps the pick
  into `rootMoves[0]` and re-sends the PV.
- **No pick** (empty `alternatives`: one legal move, the depth-1 fallback, an emergency move, a
  terminal root) → `bestmove` is rank 1, unchanged.

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
  `src/search.h`/`search.cpp`, read verbatim:
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
- **A3: the transfer argument in D4.** Our score distribution is not Stockfish's: our eval has no
  positional range (memory: |material| is the same at level as at +150-400). Gaps between near-best
  moves may be smaller than Stockfish's, so a level here may play more randomly than its
  Stockfish counterpart. Calibration absorbs this into the table. It only matters if the result is
  non-monotone in level, which the calibration slice checks.

## Invariants

- I1. `UCI_LimitStrength=false` (the default): output byte-identical to today apart from the two new
  `option` lines. Search unchanged.
- I2. Limiting: `bestmove` is a legal root move and is one of the ranked set's moves.
- I3. Limiting: the final block's `multipv 1` move equals `bestmove` (PR #988's contract, kept).
- I4. Limiting, not `infinite`: `depth_completed <= pick_depth(level_for_elo(UCI_Elo))`.
- I5. The pick never picks a candidate whose gap to rank 1 exceeds
  `delta * weakness / (128 - weakness) + 1` (D4's bound; +1 for the truncation). A mate-scored rank 1 is always picked.
- I6. The same seed and the same ranked set give the same pick.
- I7. `level_for_elo` is non-decreasing, clamps at both ends, and the table is strictly increasing
  (D3's contract).

## Validation

Tier: engine code, UCI-facing.

- I1: `Compare-SearchEquivalence.ps1` IDENTICAL against the merge base (limiting off), plus the
  existing `[uci]` tests unchanged.
- I2, I3, I4: new `StratChessTests/UCIStrengthLimitTests.cpp` (`[uci][strength]`) at a fixed seed.
  It covers both option lines and their `setoption` clamp/saturate/ignore cases, a start-position
  search at user N = 1 and N = 3, `go infinite` + `stop` (no cap), and a one-legal-move root.
- I5, I6, I7: `StrengthLimitTests.cpp` (`[strength]`), pure unit tests:
  - the bound over many seeds on synthetic sets;
  - a mate at rank 1;
  - ties;
  - a set of size 1;
  - table endpoints and monotonicity.
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
