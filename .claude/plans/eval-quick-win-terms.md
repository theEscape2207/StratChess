# Quick-Win Eval Terms: Bishop Pair, Connected Rooks, Castling-Done

**Issues**: #111, #114, #115 · **Epic**: #110 Tier 3 · **Depth**: full plan (three PRs) · **Status**: not started
**Depends on**: #127 (for the term-function shape), #130 (for measurability — see Measurement)

## Goal

Fill three of the empty/TODO slots in `EvalComplex::Evaluate`:

- **#111 Bishop pair** — `WHITE_BISHOP`/`BLACK_BISHOP` cases are empty; bishops get material + PST only.
- **#114 Connected rooks** — explicit `// TODO: Add bonus for connected Rooks!!` in the `BLACK_ROOK` case.
- **#115 Castling-done** — explicit `// TODO: Add bonus for castling-done!!` in the King case.

One plan, three independent PRs. They share nothing structurally; they share a *measurement problem*,
which is the reason to plan them together.

## Measurement: the actual hard part

Each of these is worth roughly 5–20 Elo by public engine precedent, against an instrument that resolves
±25 Elo per 500-game batch (`Docs/EloLog.md`). **A fixed batch cannot distinguish any of them from
zero.** Two acceptable strategies:

1. **Preferred — SPRT (#130), `Gain` preset, one PR per term.** Keeps attribution clean: if one term is
   a dud, it is identifiable and revertible.
2. **Fallback — one bundled PR measured as a unit**, if #130 slips or proves impractical. Acceptable, but
   record explicitly in `Docs/EloLog.md` that the measurement covers three terms jointly, so a future
   reader does not attribute the whole delta to whichever term the commit message mentions first.

What is **not** acceptable: landing them individually, running a 500-game batch each, and recording
three "+8 ± 26" rows as if they meant something. That is how a tracker accumulates false confidence.

Chess-correctness for all three is verifiable *without* an ELO match, via `[eval]` unit tests. Do that
work regardless of which measurement strategy is chosen — it is what proves the term does what it says.

---

## #111 — Bishop pair

### Design

**Bonus for genuinely opposite-colored bishops, not merely two bishops.** Promotions can produce two
same-colored bishops, which have none of the complementary-coverage property the bonus exists to reward.
The check is cheap:

```cpp
// Light/dark square masks — add to defines.h alongside the other constexpr board masks.
inline constexpr BITBOARD LIGHT_SQUARES = 0x55AA55AA55AA55AAULL;   // verify orientation against
inline constexpr BITBOARD DARK_SQUARES  = ~LIGHT_SQUARES;          // the a8=0 layout before use
```

then `(bishops & LIGHT_SQUARES) && (bishops & DARK_SQUARES)`. **Verify the mask orientation
empirically** against this engine's `a8 = 0` … `h1 = 63` layout (`defines.h:80-90`) — a1 is a dark
square, and a mask written from memory for an `a1 = 0` engine is inverted. A unit test asserting
`a1 ∈ DARK_SQUARES` and `h1 ∈ LIGHT_SQUARES` pins it down.

**Magnitude**: 30–50 cp is the conventional range. Start at the low end — an overlarge bishop-pair bonus
distorts piece-trade decisions, which is worse than a slightly small one.

**Phase/closedness scaling**: #111 suggests optionally scaling down in closed positions. **Defer.** It
needs a "closedness" measure that does not exist yet, and adding one inside a 15-Elo term makes the term
unmeasurable. Note it as a #117 candidate.

### Tests
Two bishops on opposite colors score higher than bishop+knight of equal material; two same-colored
bishops (a promotion position) do **not** get the bonus; the light/dark mask orientation cases above.

---

## #114 — Connected rooks

### Design

**Use `RookAttacks` rather than hand-rolled between-square logic.** Two rooks are connected when they
share a rank or file with nothing between them — which is exactly the condition
`RookAttacks(sqA, occupancy) & bit(sqB)`, since the magic attack set stops at and includes the first
blocker in each direction (`Magic.h`). If rook B is in rook A's attack set, B is that first blocker, so
nothing lies between them.

One line, correct by construction, no new masks, and it reuses the PEXT tables from #108 that are already
resident in cache during evaluation. Occupancy is `all_pieces` from `EvalContext`.

**Symmetry**: the relation is symmetric, so award the bonus **once per pair**, not once per rook. Iterating
both rooks and adding the bonus twice is the obvious bug here. With 3+ rooks (promotion), count connected
pairs — or cap at one bonus; pick one and comment it.

**Magnitude**: small, 10–15 cp. Connected rooks are a *positional* nicety, not a material fact.

### Tests
Rooks on a1/d1 with an empty b1/c1 score higher than with a piece on c1; rooks on a1/a8 with a clear
a-file are connected; rooks on a1/b2 (neither shared rank nor file) are not; the bonus is awarded once,
not twice (assert the exact delta, which is possible once #127 makes the term individually callable).

---

## #115 — Castling-done

### Design

**Do not add a `hasCastled_` flag to `Board`.** This is the important decision, and the reason is not
obvious:

`Board` state that affects evaluation must be part of the Zobrist key, or two positions hashing to the
same key can have different evaluations — and the transposition table will serve one's score for the
other. A `hasCastled_[2]` flag is exactly such state (two positions identical in piece placement and
rights can differ in whether the king got there by castling). So the honest version of that approach
costs: a new `Board` member, undo handling in `UndoMove`, a new Zobrist dimension, and TT-correctness
review — for a term worth perhaps 5–10 Elo. Not a good trade.

**Instead, derive the signal from state already in the position**, all of which is already hashed:
`GameInfo::castlingRights` (available via `Board::GetGameInfo()`) and the king's square.

- **Penalty** for having lost both castling rights on a side while that king still sits on a central
  file, weighted toward the midgame (mg-heavy `(mg, eg)` pair per #99) — this captures the thing that
  actually matters: the king is stuck in the centre with no way out.
- **Small bonus** for a king on a typical castled square with rights already spent.

This is a strictly weaker signal than true castling detection, and that is an accepted trade — document
it in the code comment so the next reader does not "improve" it into the Zobrist problem above.

**Interaction with #97**: king safety will subsume most of this. That is fine — #115 is explicitly
described in the epic as "a stepping stone toward #97". When #97 lands, revisit whether this term still
earns its place rather than leaving two overlapping king-placement terms to fight each other.

### Tests
A position with both rights lost and the king on e1 scores lower than the same position with rights
intact; a king on g1 with rights spent scores higher than the same king on e1 with rights spent; the
penalty is mg-weighted (smaller at low phase).

---

## Step-by-step (three PRs)

Each term is one PR, in this order. The order is not arbitrary — it runs cheapest-and-most-certain first,
so the measurement strategy gets exercised on the least risky term.

1. **PR 1 — #111 bishop pair.** Add `LIGHT_SQUARES`/`DARK_SQUARES` to `defines.h` and verify their
   orientation against the `a8 = 0` layout *before* anything consumes them (a mask written for an
   `a1 = 0` engine is inverted). Then the term function, then tests, then measure.
2. **PR 2 — #114 connected rooks.** `RookAttacks`-based detection; decide and comment the once-per-pair
   rule; tests; measure.
3. **PR 3 — #115 castling-done.** The riskiest of the three — the color-mirrored castling-rights bits are
   easy to get half-right, and the rejected `Board`-state approach is the tempting one. Term function,
   tests including the mirror cases, one self-play sanity game, measure.

If the bundled fallback is taken instead (see Measurement), land all three in one PR but keep them as three
separate commits, so a bisect or a partial revert is still possible.

## Files changed

| File | Change |
|---|---|
| `StratEngine/defines.h` | `LIGHT_SQUARES`/`DARK_SQUARES` (#111) |
| `StratEngine/Eval.h` | `BISHOP_PAIR_BONUS`, `CONNECTED_ROOKS_BONUS`, castling constants |
| `StratEngine/Eval.cpp` | Three term functions |
| `StratChessTests/EvalTests.cpp` | Cases per term |
| `Docs/TestDesign.md`, `Docs/Changelog.md` | Case list; dated entries |

`Magic.h` needs no change — `RookAttacks` is already a free function at namespace scope.

## Validation plan

Per PR:

```powershell
.\build.ps1 all
.\build.ps1 run-tests "[eval]"
.\build.ps1 run-tests
.\build.ps1 extended-tests
```

- The #125 mirror-symmetry cases must pass for every term. Bishop pair and connected rooks are naturally
  color-blind; **castling-done is the risk** — the castling-rights bit layout
  (`CastlingRights::WHITE_KINGSIDE` etc., `GameState.h`) and the king-square test both have to be
  mirrored correctly, and getting one of them right and the other wrong is easy.
- Measurement per the strategy above. State in each PR body which strategy was used and why.
- Self-play: worth one game for #115 specifically — a mis-signed castling penalty produces an engine that
  refuses to castle, which is visible in one game and invisible in a unit test.
- `eval-reviewer` dispatch on each PR (all three touch `Eval.cpp`).

## Key correctness properties

1. **Bishop pair requires opposite colors**; two same-colored bishops get nothing. Mask orientation
   verified against the `a8 = 0` layout, not assumed.
2. **Connected rooks awarded once per pair**, never twice; correct with 3+ rooks.
3. **Connected-rooks detection is blocker-aware** — any piece of either color between the rooks breaks
   the connection (free from `RookAttacks`, but test it).
4. **No new `Board` state, no new Zobrist dimension** for #115. If an implementation finds itself adding
   either, it has taken the rejected approach and needs to stop.
5. **Color symmetry** holds for all three (#125's test).
6. **Phase weighting**: #115's penalty is mg-weighted; the terms return `(mg, eg)` pairs per #99's
   convention rather than consulting a stage themselves.
7. **Attribution is honest**: whichever measurement strategy is used, `Docs/EloLog.md` records what was
   actually measured — one term or three.
