# Passed Pawn Bonus + Backwards Pawn Penalty (and the rook open-file fix)

**Issues**: #116, #126 · **Epic**: #110 Tier 3 · **Depth**: full plan · **Status**: **complete**. The #126 portion (D4/D5/D6, step 5) landed 2026-07-27 — see `Docs/Changelog.md` "Rook Open-File Definition Fix". The #116 portion (steps 1, 3, 4, 6) landed 2026-08-09; step 2 needed no work, because the mobility PR (#98/#113) had already put `pawn_attacks` in `EvalContext` for its own use, exactly as D3 anticipated.
**Depends on**: #99 (tapered eval) for the phase scaling the source TODO already asks for

## Goal

Wire up the two dead constants at `StratEngine/Eval.h:77-78` — `PASSED_PAWN_BONUS` and
`BACKWARDS_PAWN_PENALTY`, defined since the surrounding pawn code was written and referenced nowhere —
and fix the rook open-file definition (#126) in the same PR, since it is the same file, the same kind of
mask logic, and too small to measure on its own.

**Scope limit**: passed pawns, backwards pawns, rook open-file definition. Explicitly **not**:
connected/phalanx pawns, candidate passers, rook-behind-passed-pawn, or pawn-storm terms. Each is a
reasonable follow-up; none belongs in a PR whose strength contribution needs to be attributable.

## Why these two together

`EvalComplex::Evaluate` has a standing TODO directly above the pawn cases:

```cpp
case ePiece::WHITE_PAWN:
    // TODO: Add bonus for passed pawn - bonus should be dependant on game stage
```

Passed pawns are among the highest-signal terms in any evaluation, and they matter directly to two
*measured* weaknesses: #70 (endgame conversion) and #76 (no progress incentive in quietly-better
positions — 66/1000 games over 200 plies, 120/1000 drawn by repetition). An engine with no passed-pawn
gradient has no reason to push a pawn rather than shuffle.

#126 rides along because it is a genuine defect in adjacent code (see below) worth only a few Elo alone
— exactly the case where bundling is the correct measurement decision rather than sloppy scoping.

## Design decisions

**D1 — Passed-pawn detection via new precomputed span masks.** A pawn is passed when no enemy pawn
occupies its own or either adjacent file, ahead of it. The existing masks nearly cover this:
`g_bbFileUpMask[sq]` / `g_bbFileDownMask[sq]` give the same file ahead, and `g_bbFileMask[file]` gives a
whole file — but there is no three-file forward span.

Add two `constexpr` arrays to `defines.h` following the established `makeFileUpMask()` /
`makeFileDownMask()` pattern (compile-time generated, `inline constexpr auto`, no runtime init — the
`Eval.h` Lazy SMP sharing contract depends on eval reading only compile-time-initialized globals):

```cpp
inline constexpr auto g_bbPassedMaskWhite = makePassedMask(/* toward rank 8 */);
inline constexpr auto g_bbPassedMaskBlack = makePassedMask(/* toward rank 1 */);
```

Detection is then one test: `!(enemy_pawns & g_bbPassedMask<color>[sq])`. Note the board layout —
`a8 = 0`, `h1 = 63`, so "ahead" for White means *decreasing* index, matching `g_bbFileUpMask`'s
direction.

**D2 — Passed-pawn bonus scales by rank and by phase.** Two multipliers, both standard:

- **Rank**: a passer on the 7th is worth far more than one on the 3rd. Use a small `constexpr` per-rank
  multiplier table rather than folding it into `PASSED_PAWN_BONUS`, so #117 can tune the shape and the
  magnitude independently.
- **Phase**: passers are worth more as the endgame approaches (the source TODO says exactly this).
  Return the term as an `(mg, eg)` pair per #99's convention — `eg` weighted substantially higher.

This is the dependency on #99. If #99 has not landed, **do not** invent a local `gameStage` check here —
that is the ad hoc phase-checking the epic sequences #99 first to avoid.

**D3 — Backwards pawn: define it precisely, in a comment, before implementing.** "Backwards pawn" has
several incompatible definitions in the literature, and an unstated one is untunable. Use: a pawn that
(a) is behind every friendly pawn on its adjacent files, and (b) whose stop square (one step forward) is
attacked by an enemy pawn and not defended by a friendly one. Requires per-color pawn attack sets — a
one-line shift of the pawn bitboard, and something #97's pawn shield will want in `EvalContext` too, so
compute it there rather than locally.

`BACKWARDS_PAWN_PENALTY` is 5 cp, i.e. deliberately small. Keep it small: a mis-specified structural
penalty at 5 cp is a rounding error, at 30 cp it is a strategic distortion.

**D4 — #126: open file means no *pawns* of either color.** Today both rook cases read:

```cpp
if (!(g_bbFileMask[file] & all_black))       // all enemy PIECES, not pawns
    bonusScore[WHITE] += OPEN_FILE - HALF_OPEN_FILE;
```

so a single enemy knight — or the enemy **king** — anywhere on the file costs 5 cp by demoting a
genuinely open file to half-open. Backwards: an enemy piece on a file the rook controls is usually a
target. Change `all_black`/`all_white` to `black_pawns`/`white_pawns`.

**D5 — #126, second part: decide the own-pawn-behind question explicitly.** The half-open test uses
`g_bbFileUpMask[square]`, which only sees own pawns *ahead* of the rook — so an own pawn *behind* the
rook on the same file still counts as half-open. Defensible (the rook's forward file is clear), and
changing it would alter more positions than the `all_black` fix. **Keep the current behaviour and
document it in a comment.** The point of #126 is to make the definition deliberate, not to maximise the
diff.

**D6 — #126, third part: delete the dead commented-out branches.** Both rook cases carry a commented
`else if` reaching for "file contested only by an enemy rook/queen". Either implement it or remove it;
leaving speculative dead alternatives in the hot path is how this file accumulated a decade of ambiguity.
Recommendation: **remove**, and note the idea in #117's plan as a tunable-term candidate.

## Files changed

| File | Change |
|---|---|
| `StratEngine/defines.h` | `makePassedMask()` + the two `constexpr` span-mask arrays |
| `StratEngine/Eval.h` | Per-rank passer multiplier table; comments defining "passed" and "backwards" |
| `StratEngine/Eval.cpp` | Passer + backwards terms in the pawn term function; #126 fixes in the rook term |
| `StratChessTests/EvalTests.cpp` | `[eval]` cases (below) |
| `StratChessTests/BoardApiTests.cpp` or a bitboard test file | Direct mask-content tests (see #104) |
| `Docs/TestDesign.md`, `Docs/Changelog.md` | Case list; dated entry |

## Step-by-step

1. **Masks first, tested in isolation.** Add `makePassedMask()` and both arrays; test mask *content*
   directly — a pawn on d4 must have exactly c5-c8/d5-d8/e5-e8 set for White; an a-file pawn must have
   only a+b files (no wraparound to the h-file, the classic bug in file-shift mask generation); an
   8th-rank square must have an empty mask. Getting this wrong silently produces a term that is subtly
   wrong everywhere, so it is verified before anything consumes it.
2. **Pawn attack sets into `EvalContext`** (D3) — needed by the backwards term, reused by #97 later.
3. **Passed-pawn term** with the rank multiplier and `(mg, eg)` phase split (D2).
4. **Backwards-pawn term** (D3).
5. **DONE (2026-07-27)** — **#126 rook fixes** landed as a separate, independent commit (D4/D5/D6);
   see `Docs/Changelog.md` "Rook Open-File Definition Fix (issue #126)". Steps 1-4/6 below are for
   the remaining #116 (passed/backwards pawns) work only.
6. **Tests.**

## Validation plan

```powershell
.\build.ps1 all
.\build.ps1 run-tests "[eval]"
.\build.ps1 run-tests
.\build.ps1 extended-tests
```

New `[eval]` cases — all as A-vs-B deltas between two FENs differing in exactly one property, the
pattern the existing mop-up tests already use, plus direct term calls now that #127 makes terms callable:

- A white pawn on e5 with no black pawn on d/e/f ahead scores higher than the same position with a black
  pawn on d7.
- A passer on the 7th scores substantially higher than the same passer on the 3rd.
- Passer bonus is larger at low phase than high phase (the D2 property).
- An a-file passer is detected (no adjacent-file wraparound).
- A backwards pawn per D3's definition is penalised; a pawn failing either clause is not.
- **#126**: a rook on an open file with an enemy *knight* on that file now scores the full `OPEN_FILE`
  bonus (fails on `HEAD` — write it first and confirm it fails); with an enemy *pawn* on the file it
  gets only `HALF_OPEN_FILE`.
- The #125 mirror-symmetry cases must still pass — the two new masks are the most likely place for a
  color asymmetry to be introduced.

Every FEN: full ` w - - 0 1`-style side-to-move field, and verified legal (#45/#46).

**ELO**: passed pawns are the largest single term in this PR and plausibly resolvable — SPRT (#130)
`Gain` preset. If SPRT is unavailable, a 500-game batch is the fallback, with the honest caveat that a
result inside ±25 Elo is not evidence either way.

**Self-play** is worth one game here specifically: a passer bonus that is too large produces
pawn-pushing that abandons pieces, which shows up immediately in a game and not at all in a unit test.

**Pre-PR**: `Scripts\Validate-PrePR.ps1` + `eval-reviewer` dispatch (mandatory).

## Key correctness properties

1. **Mask correctness**: `g_bbPassedMask*[sq]` covers exactly the three-file forward span, no
   wraparound at the a/h files, empty on the promotion rank.
2. **Direction**: White's span goes toward index 0 (rank 8), Black's toward index 63. Getting these
   backwards produces a term that rewards the wrong side — and would be caught by property 3.
3. **Color symmetry**: #125's mirror test still passes.
4. **Phase monotonicity**: the passer bonus is non-decreasing as phase decreases toward the endgame.
5. **Rank monotonicity**: the passer bonus is non-decreasing as the pawn advances.
6. **Backwards-pawn definition matches the comment** — both clauses required; a pawn satisfying only one
   is not penalised.
7. **#126**: open-file classification depends only on pawns; own pawns *behind* the rook still yield
   half-open (D5, documented); no dead commented branches remain.
8. **Constants are used**: `PASSED_PAWN_BONUS` and `BACKWARDS_PAWN_PENALTY` are referenced — the literal
   thing #116 exists to fix. Grep for both before opening the PR.
