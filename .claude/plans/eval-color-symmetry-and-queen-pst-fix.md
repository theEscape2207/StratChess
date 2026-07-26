# Eval Color Symmetry + Queen PST Fix

**Issue**: #125 · **Epic**: #110 Tier 0 (correctness) · **Depth**: full plan · **Status**: not started

## Goal

Make `EvalComplex`/`EvalSimple` provably color-symmetric — a position and its color-mirror must
evaluate to the same score — and add a regression test that keeps it that way.

Two independent defects produce today's asymmetry:

1. `EvalManager::getEvalBoard` (`StratEngine/Eval.h:47`) maps a Black piece's square to its PST index
   with `MAXSQUARES - square` (`63 - square`). That is a **180° rotation**, which mirrors *files* as
   well as ranks. The intended operation is a vertical flip: `square ^ 56`.
2. The queen PST (`StratEngine/defines.h:278`) has one asymmetric pair — c6 = `4`, f6 = `3` — which
   makes defect 1 observable instead of merely latent.

**Scope limit**: this is a correctness fix, not a tuning change. Expected ELO impact is ≈0 and no ELO
match is planned (see Validation). Do not retune any PST value here; that is #117.

## Why this is Tier 0

The measured magnitude is 1 cp, which on its own would not justify a PR. Two consequences do:

- **It blocks standard future terms.** King-side castling bias and king-side pawn-storm tables are
  both file-asymmetric and both standard. Either would silently produce a different score for White
  and Black under a 180° rotation, with no test to catch it. #97 (King Safety) is the natural place
  such a table appears.
- **It corrupts #117 (Texel tuning).** A tuner fits parameters by minimising error against labelled
  positions. An evaluator that scores a position differently from its mirror injects a systematic
  residual that every fitted parameter partially absorbs. Symmetry must be true *before* tuning, not
  repaired after.

Both reasons are about work that comes later, which is exactly why this goes first.

## Design decisions

**D1 — Fix the mirroring, not just the table.** Making the queen table symmetric alone would make the
mirror test pass, since all seven tables would then be file-symmetric and a 180° rotation would be
*equivalent to* a vertical flip. That is precisely the trap: the invariant would hold by accident and
break the next time someone adds an asymmetric table. Fix the operation. The queen typo gets fixed
too, but as a separate, independently-justified change.

**D2 — `square ^ 56`, not `56 - 8*Rank + File` arithmetic.** `^ 56` flips the three rank bits of the
0..63 index and leaves the file bits untouched. Given the board layout in `defines.h:80-90`
(`a8 = 0` … `h1 = 63`), that is exactly a vertical mirror. It is also one instruction and stays
`constexpr`.

**D3 — Test the mirroring operation directly, in addition to whole-position symmetry.** A
whole-position mirror test passes if *either* defect is fixed, so on its own it cannot tell us the
mirroring is right — it would go green after a queen-table-only fix and then rot. Add a direct test
of `getEvalBoard` asserting file preservation (e.g. a Black piece on `a1` maps to index `a8`, and a
Black piece on `c6` maps to index `c3`, **not** `f3`). `getEvalBoard` is `protected static`, so the
test reaches it through a minimal test-local subclass — no production visibility change:

```cpp
// EvalTests.cpp — exposes EvalManager's protected mirroring helper for direct testing.
struct EvalProbe final : EvalManager {
    int Evaluate(const Board&) const override { return 0; }
    const char* GetType() const override { return "Probe"; }
    using EvalManager::getEvalBoard;
};
```

**D4 — Keep the queen typo fix in the same PR.** It is a one-character change to a value with no
chess justification (a queen on c6 and f6 are mirror-equivalent placements), and the mirror test
gives it a guard. Note it makes the change **not** score-identical to `HEAD`: positions with a queen
on c6/f6/c3/f3 shift by 1 cp. That is intended and must be stated in the PR body rather than
discovered by a reviewer.

**D5 — Do not add a `static_assert` on PST file-symmetry.** Tempting as a cheap guard, but it would
forbid exactly the file-asymmetric tables that D1 exists to enable. The mirror test is the correct
invariant: it holds for any table content.

## Files changed

| File | Change |
|---|---|
| `StratEngine/Eval.h` | `getEvalBoard` → `square ^ 56`; remove the now-unused `MAXSQUARES` member |
| `StratEngine/defines.h` | Queen PST row 3 (`:278`): index 5 `3` → `4` |
| `StratChessTests/EvalTests.cpp` | `EvalProbe` subclass; `getEvalBoard` cases; whole-position mirror cases; FEN mirror helper |
| `Docs/TestDesign.md` | §Evaluation Tests — add the new cases to the enumerated list |
| `Docs/Changelog.md` | Dated entry |

## Step-by-step

### 1. `Eval.h` — the mirroring operation

Replace:

```cpp
static constexpr inline int getEvalBoard(ePiece piece, eSquare square) noexcept
{
    return (PieceHelper::Color(piece) == eColor::BLACK) ? (MAXSQUARES - square) : square;
}
```

with a vertical flip (`square ^ 56`), carrying a comment that states *why* — that `63 - square`
rotates rather than mirrors, and that the distinction only shows up once a PST is file-asymmetric.
Delete the `private: static const int MAXSQUARES = ALL_SQUARES - 1;` member; it has no other use
(verify with a search before removing).

Both call sites are already routed through this helper — `GetPositionalScore` (`Eval.h:45`) and the
two direct king-table lookups in `Eval.cpp:192,195` — so there is nothing else to change.

### 2. `defines.h` — the queen PST

Row 3 of the queen table currently reads `0, 2, 4, 5, 5, 3, 2, 0`. Change index 5 from `3` to `4`.
Re-run the symmetry check from step 3 to confirm the table set is clean.

### 3. Confirm the defect empirically, before and after

The asymmetry claim came from a source read; confirm it mechanically. A throwaway script that parses
`g_Eval_Bitboards` out of `defines.h` and checks `t[sq] == t[sq ^ 7]` for all 64 squares of all seven
tables reports, against current `HEAD`:

```
PAWN: file-symmetric OK        ROOK:     file-symmetric OK        KING_MID: file-symmetric OK
KNIGHT: file-symmetric OK      QUEEN:    ASYMMETRIC (1 pair)      KING_END: file-symmetric OK
BISHOP: file-symmetric OK          sq 18 (c6) = 4  vs  sq 21 (f6) = 3
```

This is a scratch verification, not a deliverable — do not commit the script. The committed guard is
the test suite from step 4.

### 4. `EvalTests.cpp` — the tests

**4a. Direct mirroring test** (D3) — using `EvalProbe`, assert for a Black piece:
`getEvalBoard(BLACK_QUEEN, a1) == a8`, `== c3 → c6`, `== h1 → h8`; and for a White piece that the
index is the square unchanged. The `c3 → c6` case is the one that fails on `HEAD` (it currently
yields `f6`) and is the direct proof of defect 1.

**4b. FEN mirror helper** — a file-local `static std::string MirrorFen(std::string_view)` that
color-mirrors a FEN: reverse the order of the rank fields, swap the case of every piece character,
flip the side-to-move field, swap `KQ`↔`kq` in castling rights, and mirror the en-passant square's
rank (`e3`↔`e6`). Keep it local to the test file — it is test scaffolding, not engine functionality.

**4c. Whole-position symmetry cases** — for each of a small set of FENs, assert
`Evaluate(board(fen)) == Evaluate(board(MirrorFen(fen)))`, for **both** `SIMPLE` and `COMPLEX`.
Because `Evaluate` is side-to-move-relative and mirroring swaps which color moves, the mover is in
an identical situation in both positions, so the scores must be *equal* (not negated).

Cover at least: the starting position; a queen on c6 (the position that fails today); an asymmetric
middlegame with rooks on open/half-open files (exercises the rook terms); an endgame that trips the
`ENDGAME` stage and the king-PST switch; and a pawnless decisive ending that trips the mop-up term
(mop-up's `CenterManhattanDistance` is orientation-independent, so it should already be symmetric —
this case pins that down).

Every FEN must follow the `[tactical]` construction rules in CLAUDE.md: full side-to-move field, and
legal (no king in check on the mover's turn) — the FEN parser silently accepts illegal positions
(#45).

### 5. Docs

Add the new cases to the enumerated list in `Docs/TestDesign.md` §Evaluation Tests (that section
lists every eval case, so it goes stale otherwise), and a dated `Docs/Changelog.md` entry.

## Validation plan

```powershell
.\build.ps1 all                                     # Level4 + /WX, both projects
.\build.ps1 run-tests "[eval]"                      # the new + existing eval cases
.\build.ps1 run-tests                               # full fast tier
.\build.ps1 extended-tests                          # includes [slow] / [tactical_full]
```

**Order matters for the proof**: write the step-4a and step-4c tests *first* and confirm they
**fail** against unmodified `HEAD`. A symmetry test that has only ever been seen passing proves
nothing about the defect it claims to guard.

**Watch `[tactical]` specifically.** The 1 cp queen-PST change can tip a razor-thin move-ordering tie
— `Tactical - QFORK-001` has done exactly this twice before (#66, then again when the mop-up term
landed; see #118 item 1). If a tactical case flips, treat it as a tie-sensitivity finding to record in
#118, not as evidence this fix is wrong — but do investigate before merging, and do not silently hide
a test.

**No ELO match.** A 1 cp change in a rarely-relevant PST entry is orders of magnitude below the
instrument's ±25 Elo resolution (`Docs/EloLog.md`); running a 500-game batch would consume an hour to
produce a number indistinguishable from noise either way. The symmetry test plus a clean extended-test
run is the stronger evidence here. State this reasoning in the PR body — "no ELO match, and here is
why" — rather than leaving its absence to be questioned.

**Pre-PR**: `Scripts\Validate-PrePR.ps1` (source diff, so the full path applies), then dispatch the
`eval-reviewer` subagent per CLAUDE.md's checklist — this touches `Eval.h` and `defines.h`.

## Key correctness properties

1. **Color symmetry**: for every legal position `p`, `Evaluate(p) == Evaluate(mirror(p))` for both
   evaluators. This is the invariant the whole change exists to establish.
2. **File preservation**: `getEvalBoard` changes only the rank component of the index. A Black piece's
   PST index must have the same `File()` as its square.
3. **Rank inversion**: `getEvalBoard(black, sq)` maps rank 1 ↔ rank 8, 2 ↔ 7, etc. (`^ 56` gives this
   for free; property 2 is the part that was broken.)
4. **No White-side change**: White scores are untouched except through the queen-PST typo fix. Any
   other White-side delta means the mirroring change leaked into the non-Black branch.
5. **Table-content independence**: the symmetry property must hold for arbitrary PST content, not only
   file-symmetric content. This is what makes #97's future asymmetric tables safe, and why D5 rejects
   a `static_assert` on table symmetry.
