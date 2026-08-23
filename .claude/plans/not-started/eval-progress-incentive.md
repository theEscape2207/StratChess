# Progress Incentive in Quietly-Better Positions

**Issue**: #76 · **Epic**: #110 Tier 5 · **Depth**: design sketch · **Status**: not started
**Depends on**: #99 (phase), #116 (passed pawns) — most candidate fixes need both

> Sketch, not an executable plan. Unlike most of this epic, the *problem* here is measured and the
> *solution* is not identified — so this sketch is deliberately a list of candidates with a triage
> strategy, not a design.

## The measured problem

From the 1,000-game ELO baseline (`Docs/EloLog.md`, PGNs under `StratChessEvolved/logs/elo/`):

- **66 / 1,000 games exceeded 200 plies.** The pattern is a side holding +0.3 to +1.7 for 50+ moves while
  shuffling (e.g. the 265-ply Round-4 game in `20260703-130419.pgn`, plies 170-240).
- **120 / 1,000 games drew by threefold repetition**, many from early 0.00 agreement.

Both are the same underlying gap: **the evaluation has no gradient toward progress.** If shuffling and
improving score identically, repetition is always an acceptable option, and a won position never gets
converted. #70 (mop-up) addressed the *pawnless won endgame* instance of this; #76 is the general case,
where a mop-up term does not apply.

## Candidate fixes, in rough order of expected value

**1. Passed-pawn push urgency** (needs #116 + #99). If a passer's bonus grows as it advances, pushing it is
strictly better than shuffling — a direct progress gradient. Largely delivered *for free* by #116's
rank-scaled, phase-scaled passer bonus. **Measure whether #116 alone moves the >200-ply and
repetition-draw rates before building anything else here.** That is the cheapest possible experiment and it
may close a good fraction of the gap.

**2. Game-stage-aware king activity** (needs #99). Already partly addressed: #99's tapered king PST creates a
continuous gradient toward centralization as material comes off, where today there is a cliff. Same
principle — re-measure after #99 before designing more.

**3. Small contempt / repetition avoidance when eval says better.** The most direct fix and the most
dangerous: bias the draw score away from 0 when the engine believes it is winning, so a repetition is not an
acceptable substitute for playing on. Standard technique, but it interacts with the TT (a contempt-adjusted
draw score cached at one root eval is wrong at another) and with threefold detection. This is a **search**
change wearing an eval costume — treat it as such, and dispatch `search-reviewer` as well as `eval-reviewer`.

**4. 50-move-counter score scaling.** Scale the score toward 0 as `GameInfo::fiftyCount` rises, so the engine
sees the draw approaching and is pushed to make progress (a capture or pawn move resets the counter, which is
exactly "progress"). Cheap, principled, and needs no new eval infrastructure — `fiftyCount` is already in
`GameInfo`. **Note the TT hazard**: `fiftyCount` is not part of the Zobrist key, so a score that depends on it
can be cached and reused at a different counter value. Either accept the imprecision explicitly or restrict
the scaling to the root. Do not hand-wave this.

**5. "Improve the worst-placed piece" terms.** Mobility (#98) partly delivers this — a piece with no moves
scores badly, so improving it is rewarded. Re-measure after #98.

## Triage strategy

**Do not design a fix for #76 until #99, #116 and #98 have landed and the metric has been re-measured.**
Three of the five candidates are substantially delivered as side effects of work already sequenced ahead of
this issue. Building a bespoke contempt mechanism first risks solving a problem that tapered eval and passed
pawns already solved, and doing it with the riskiest available tool.

## Measurement — the unusual part

This is the one item in the epic with a **direct behavioural metric**, not just an Elo number:

- **% of games exceeding 200 plies** (baseline 6.6%)
- **% of games drawn by threefold repetition** (baseline 12.0%)
- **% of draws agreed from a position where the winning side's eval exceeded +0.5**

All three are computable from the match PGNs the ELO harness already produces. Track them alongside Elo for
every candidate. A fix that reduces long-game and repetition-draw rates while leaving Elo flat is still
progress on the stated problem — and, importantly, a fix that improves Elo while leaving those rates
unchanged did **not** fix #76 and should not close it.

Issue #77 (move-quality profiling of ELO match games — eval-swing scan + external ACPL adjudication) is the
natural tooling home for computing these metrics repeatably rather than by hand. Consider doing that first if
this work gets more than one iteration.

## Files likely touched

Depends entirely on which candidate survives triage. Candidates 1/2/5: nothing new — they are measurements of
work done elsewhere. Candidate 3: `AIPerplex.cpp` (draw scoring, TT interaction) + `Eval.cpp`. Candidate 4:
`Eval.cpp` plus a decision about `Board`/`GameInfo` plumbing.

## Test ideas

- For candidate 4: the same position scores closer to 0 at `fiftyCount = 90` than at `fiftyCount = 0`.
- For candidate 3: a repetition available from a +1.0 position is scored worse than the +1.0 continuation.
- For candidates 1/2: assert the gradient directly — advancing a passer, or centralizing a king in an
  endgame, must strictly increase the score. If it does not, the upstream term did not deliver the gradient
  and the re-measurement above will be uninformative.
- Regression: the #125 mirror-symmetry cases must still pass. Contempt is inherently side-relative, so a
  contempt implementation *will* break naive symmetry — if candidate 3 is pursued, decide up front how the
  symmetry test should treat it rather than weakening the test after the fact.
