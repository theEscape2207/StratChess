# Tapered Evaluation (Midgame → Endgame)

**Issue**: #99 (carries #118 item 4) · **Epic**: #110 Tier 1 · **Depth**: full plan · **Status**: in progress 2026-07-29
**Depends on**: #127 (EvalContext restructure) — **landed**, PR #141

## Drift review (2026-07-29, before implementation)

Re-checked every load-bearing assumption against `main` at `a025614`. All the eval-side facts hold:

- #127 landed, and `ctx.stage` is consulted in exactly the three places this plan names —
  `eval_rooks` (7th-rank gate), `eval_pst` (king table selection), `eval_mopup` (gate).
- **D5's arithmetic is exact, and was verified against `defines.h`, not assumed.** The endgame king
  table is precisely `60 - 10 * CenterManhattanDistance(sq)` (spot-checked a8=0/CMD 6, b7=20/CMD 4,
  d4=60/CMD 0), and the midgame table is uniformly −40/−20/0 by rank. So the ≈100 cp cliff and the
  net −6 cp/step disincentive both stand as written.
- `MOPUP_*` constants unchanged; #130 (SPRT) is closed, so the preferred measurement is available;
  #129's batch mode is available for the step-2 identity check, which is why it was sequenced first.

**One real drift, and it widens the blast radius of D6.** #129 phase 2 (PR #145) landed *after* this
plan was written and made `PlayState` **public API**: `EvalBreakdown::stage` is a field on the struct
`EvalComplex::Breakdown()` returns, `UciHandler::cmd_eval()` prints it as `stage: middlegame`, and
both `[uci]` and `[eval]` tests assert on it. Deleting the enum (D6) therefore also touches
`UCIHandler.cpp`, `UCITests.cpp`, `EvalTests.cpp` and `Docs/TestDesign.md` — none of which appear in
the Files-Changed table below.

**Resolution**: replace `EvalBreakdown::stage` with `int phase`, and have `cmd_eval` print
`phase: N/24` instead of `stage: <name>`. This is strictly more informative than the boolean it
replaces — a reader debugging a king-placement score wants to know *how far* through the taper the
position is, not which side of a threshold it fell on — and it keeps #129's introspection honest
rather than leaving it reporting a concept the evaluator no longer has. The `[uci]`/`[eval]` cases
that assert `stage: middlegame`/`ENDGAME` become phase-value assertions.


## Goal

Replace the binary `MIDDLEGAME`/`ENDGAME` switch with a continuous phase value, and interpolate
phase-sensitive terms between a midgame and an endgame score instead of hard-switching them.

Concretely:

1. A `phase` in `[0, MAX_PHASE]` computed from non-king, non-pawn material.
2. Terms that care about phase return a `(mg, eg)` pair, blended once at the end.
3. The king PST becomes the first tapered term (it already has both tables).
4. **Fix #118 item 4** — the mop-up king-approach term is currently out-competed by the endgame king
   PST, which is the most likely reason mop-up measured ≈0 Elo.

**Scope limit**: taper the terms that already have phase-dependent behaviour, plus the king PST. Do
**not** invent `(mg, eg)` pairs for every PST — that is a tuning exercise (#117) masquerading as a
structural change, and it would make this PR unmeasurable.

## Why this is the Tier 1 keystone

Today the only phase-awareness in `EvalComplex::Evaluate` is:

```cpp
if (iMinScore <= 11500)
    gameStage = PlayState::ENDGAME;
```

used in three places — the king PST table selection, the rook-on-7th gate, and the mop-up gate. Three
problems, all of which tapering removes:

- **It is a discontinuity.** A single capture crossing the threshold jumps the king's positional score
  by up to 100 cp (the mid table is uniformly −40/−20/0 by rank; the end table peaks at +60 centrally).
  A search that sees this cliff mid-tree gets scores that depend on *when* a trade happens rather than
  whether it is good — a plausible contributor to the shuffling behaviour measured in #76.
- **The threshold is keyed on the wrong quantity.** `GetMaterialScore` includes the king at 10000 cp
  (`g_iPieceValues`, `defines.h:161`), which is the entire reason the constant is the otherwise
  inexplicable `11500` (= 10000 + 1500). Nothing in the code says so.
- **It keys on `min()` of the two sides.** A side still holding a queen switches to endgame
  king-centralization scoring as soon as its *opponent* is stripped down — i.e. it walks its king out
  while the opponent still has mating material.

And every Tier 2/3 term wants phase scaling: king safety must fade toward the endgame, mobility weights
differ by phase, and #116's passed-pawn bonus has a standing TODO in the source saying it "should be
dependant on game stage". Without a shared phase, each one invents its own ad hoc check.

## Design decisions

**D1 — Standard piece-count phase, `MAX_PHASE = 24`.** Weights N=1, B=1, R=2, Q=4 summed over both
sides: `24` with all pieces present, `0` in a bare pawn endgame. Widely used, no tuning needed to be
reasonable, and trivially derived from the piece bitboards already in `EvalContext`. Clamp to
`[0, 24]` — promotions can exceed 24 (e.g. three queens), and an unclamped phase would extrapolate
outside the interpolation range.

Deliberately **not** material-sum based: a material threshold conflates "few pieces" with "material
imbalance", which is the `min()` bug in another form.

**D2 — Blend once, at the end, in one place.**

```cpp
score = (mg * phase + eg * (MAX_PHASE - phase)) / MAX_PHASE;
```

Integer arithmetic throughout — the engine has no floating point in eval and should not gain any here.
Non-tapered terms are added to both `mg` and `eg` (equivalently, added after the blend); pick one and
be consistent, because mixing the two conventions is how tapering bugs hide.

**D3 — Taper the king PST only, in this PR.** `g_Eval_Bitboards[5]` (mid) and `[6]` (end) become the
`(mg, eg)` pair for the king — which is what the two tables always were, just selected discontinuously
instead of blended. This is the change with real expected Elo and the one that fixes #118 item 4.

Rook-on-7th moves from a hard `gameStage != MIDDLEGAME` gate to an eg-weighted bonus (mg contribution
0, eg contribution `ROOK_ON_7TH_BONUS`), which reproduces today's intent continuously. Note in passing
that "7th-rank rook is an endgame-only bonus" is itself dubious chess — a rook on the 7th is often
strongest in the middlegame against pawns still on their starting squares — but changing that is a
tuning decision for #117, not this PR. Flag it, don't fix it.

**D4 — Mop-up keeps a hard gate, expressed as a phase threshold.** Mop-up is a special-case term for
pawnless decisive endings, not a smoothly-scaling one; blending it in at phase 12 would be wrong. Keep
a discrete gate but key it on the new phase (plus the existing pawnless + material-lead conditions).
Tightening the *material* half of that gate is #128's job (#118 item 5) — do not do both here.

**D5 — Fix #118 item 4 by suppressing the winner's king PST inside the mop-up branch.** This is the
substantive bug fix riding along, and it needs stating precisely because the arithmetic is
counterintuitive.

The endgame king PST is exactly `60 - 10 * CenterManhattanDistance(sq)` for every square. So:

- `MOPUP_CMD_WEIGHT = 10` applied to the *losing* king reproduces that same slope — it doubles an
  existing signal (10 → 20 cp/step) rather than adding a new one.
- `MOPUP_KINGDIST_WEIGHT = 4` pays the *winning* king 4 cp per step of approach, while that king's own
  endgame PST charges it 10 cp per step of centralization surrendered to approach the corner. Net
  **−6 cp per step**: concretely, Kd4→Kc5 with the loser at a8 loses 10 PST and gains 4 king-distance.

Mop-up softens the pre-existing disincentive to walk the king in, but never reverses it. Standard
mop-up formulations assume no competing centralization term for the winning king.

Two candidate fixes; **prefer (a)**:

- **(a) Zero the winner's king PST contribution inside the mop-up-gated branch** and let mop-up's own
  terms fully own king placement there. Restores the standard formulation, and *decouples* the two
  parameters — which #118 explicitly flags as a prerequisite for #117 (`MOPUP_CMD_WEIGHT` and the
  endgame king PST slope currently express one concept as two numerically coupled parameters).
- (b) Raise `MOPUP_KINGDIST_WEIGHT` above 10. Cheaper, but leaves the coupling in place for the tuner
  to trip over.

**D6 — Delete the dead `FINALGAME`.** `PlayState::FINALGAME` exists in the `Eval.h` enum, is never
assigned (the assignment is commented out), and its commented-out branch did the same thing as
`ENDGAME`. With a continuous phase the whole enum can go. Removing it is in scope; it is the thing being
replaced.

## Files changed

| File | Change |
|---|---|
| `StratEngine/Eval.h` | `PlayState` removed; phase constants; `(mg, eg)` score-pair type; term signatures |
| `StratEngine/Eval.cpp` | Phase computation; blend; king PST tapered; rook-7th eg-weighted; mop-up gate rephrased + D5 fix |
| `StratChessTests/EvalTests.cpp` | Phase/monotonicity/continuity cases; a mop-up king-approach case |
| `Docs/TestDesign.md` | §Evaluation case list |
| `Docs/Changelog.md` | Dated entry |

Do **not** change `Board::GetMaterialScore` or `g_iPieceValues` — the king's 10000 cp is load-bearing
elsewhere (move ordering via `MoveHelper::Value`). Phase is computed from bitboard popcounts instead,
which sidesteps the issue entirely.

## Step-by-step

1. **Phase computation** in `EvalContext` (from #127): popcount each non-king non-pawn piece bitboard,
   weight per D1, clamp to `[0, 24]`. Unit-test it directly — startpos = 24, bare kings = 0, KQ vs KR = 6.
2. **Blend plumbing**: term functions return an `(mg, eg)` pair; `Evaluate()` accumulates both and blends
   once (D2). At this stage set `mg == eg` for every existing term. **Scores must be identical to
   pre-change** — a corpus diff via #129's batch mode, exactly as in #127. This isolates the plumbing
   from the behaviour change.
3. **Taper the king PST** (D3). Scores now change. This is the first real behavioural step.
4. **Rook-on-7th → eg-weighted** (D3).
5. **Mop-up gate → phase-based** (D4), behaviour-preserving.
6. **Fix #118 item 4** (D5, option (a)). Keep this as its own commit — it is an independently arguable
   change and a reviewer needs to see it separately from the tapering.
7. **Remove `PlayState`** and the dead `FINALGAME` (D6).
8. **Tests** (below).

## Validation plan

```powershell
.\build.ps1 all
.\build.ps1 run-tests "[eval]"
.\build.ps1 run-tests
.\build.ps1 extended-tests
```

New `[eval]` cases:

- **Phase values**: startpos 24; bare kings 0; a known middlegame position's expected value; a
  three-queen promotion position clamps to 24 rather than exceeding it.
- **Continuity**: for a position where a capture previously crossed the `11500` threshold, the score
  difference before/after the capture is now smooth — assert the delta is far below the ~100 cp cliff
  the hard switch produced. This is the property the change exists to create, so test it directly.
- **King centralization scales with phase**: the same king placement is worth more in a low-phase
  position than a high-phase one.
- **#118 item 4 regression**: in a pawnless KQ-vs-KR position, moving the winning king *toward* the
  cornered losing king must now increase the score. Under current `HEAD` it decreases it (net
  −6 cp/step) — write this test first and confirm it fails, or the fix is unproven.

**ELO measurement — this is the one Tier 1 item with real expected strength impact.** Tapering is a
well-established gain and the removed discontinuity plausibly touches #76's shuffling behaviour, so a
measurable effect is expected. Use SPRT (#130) with the `Gain` preset; a 500-game fixed batch is
acceptable as a fallback since the expected effect may exceed ±25 Elo, but SPRT is preferred.

Also run self-play (`self-play-validate` skill / `"type": 6` both sides) — the king PST is now blended
at every node, and a sign error there produces kings that walk into the open in the middlegame. That is
obvious in one game and invisible in a unit test.

**Pre-PR**: `Scripts\Validate-PrePR.ps1` + `eval-reviewer` dispatch (mandatory — this is a core
`Eval.cpp` change).

## Key correctness properties

1. **Phase bounds**: always in `[0, MAX_PHASE]`, monotonically non-increasing as material comes off,
   clamped against promotion overshoot.
2. **Blend endpoints exact**: at `phase == MAX_PHASE` the score equals the pure mg score; at
   `phase == 0`, the pure eg score. Off-by-one here is the classic tapering bug.
3. **Color symmetry preserved** — the #125 mirror test must still pass. Phase is color-blind (summed
   over both sides), so a per-color phase would be a bug.
4. **No discontinuity**: no term may consult a phase *threshold* except the deliberately-gated mop-up
   (D4).
5. **Step-2 identity**: with `mg == eg` for all terms, scores are byte-identical to pre-change. If not,
   the blend arithmetic is wrong and everything after it is measuring the wrong thing.
6. **Winning king approaches**: in a mop-up-gated position, decreasing king-to-king distance must not
   decrease the score (D5).
7. **`EvalManager` stays stateless** — phase lives in the per-call context, never as a member.
