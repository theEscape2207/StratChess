# Automated (Texel-style) Evaluation Parameter Tuning

**Issue**: #117 · **Epic**: #110 Tier 5 · **Depth**: design sketch · **Status**: not started
**Depends on**: #129 (batch static eval — hard prerequisite), #125 (color symmetry — hard prerequisite),
and the full term set from Tiers 1-4

> Sketch, not an executable plan. Deliberately last in the epic, and the largest engineering lift in it.

## Goal

Fit every evaluation constant — PST tables, `DOUBLED_PAWN_PENALTY`, `ISOLATED_PAWN_PENALTY`,
`ROOK_ON_7TH_BONUS`, `HALF_OPEN_FILE`, `OPEN_FILE`, the mop-up weights, and every term added by this epic —
against a labelled corpus of quiet positions with known game outcomes, replacing hand-picked values.

## Why last, and why it may be the largest single gain

**None of `Eval.cpp`'s constants have ever been tuned against data.** Public precedent from Texel's own
history (v1.02 → v1.03) is ≈100 Elo gained purely from retuning existing terms, with *zero* new eval logic.
That plausibly makes this the highest expected-value item in the epic.

It is sequenced last because the tuner should run **once** over the full expanded term set, not repeatedly
after each addition. Tuning after every Tier 3 term would mean re-running the whole pipeline five times and
would also make each term's individual ELO measurement meaningless (is the gain the term, or the retune?).

## Approach

Standard Texel's Tuning Method:

1. **Corpus** — a few hundred thousand quiet positions labelled with the game result (1 / 0.5 / 0).
   Source: PGNs from `StratChessEvolved/logs/elo/` (this engine's own match history) plus a public game
   collection. Filter to quiet positions (not in check, no captures available / stand-pat stable).
2. **Objective** — minimise mean squared error between `sigmoid(K * eval)` and the game result, where `K`
   is a scaling constant fitted first.
3. **Optimiser** — local search over the parameter vector (the classic implementation) or gradient descent.
   Local search is simpler, needs no derivatives, and is what the original method used; start there.
4. **Apply** — regenerate the constants and tables in `Eval.h` / `defines.h`, then measure.

### Decisions that look settled

- **#129 is a hard prerequisite.** The tuner needs bulk static eval — hundreds of thousands of positions.
  Driving that through `go depth 1` is both wrong (`Evaluate()` is only called from `AIPerplex::quiescence`,
  so a depth-1 score is a qsearch result, not a static eval) and far too slow. #129's batch FEN-scoring mode
  is the interface.
- **#125 is a hard prerequisite.** An evaluator that scores a position differently from its color-mirror
  injects a systematic residual that every fitted parameter partially absorbs. Fix symmetry, then fit.
- **The tuner is a tool, not engine code.** It should not ship inside `StratEngine`. Options: a separate
  small C++ tool in the solution, or Python driving #129's batch mode. **Python is probably right** —
  iteration speed matters far more than tuner throughput, and the expensive part (static eval) is already
  in C++ behind #129.
- **Approved-dependency rule applies to the engine, not to an offline tool.** CLAUDE.md restricts external
  dependencies to spdlog and nlohmann/json — that governs `StratEngine`. A Python tuner using numpy is not
  an engine dependency, but say so explicitly in the plan so it does not read as a violation.
- **Decouple numerically-coupled parameters before fitting.** #118 flags that `MOPUP_CMD_WEIGHT` and the
  endgame King PST slope both operate on the same `CenterManhattanDistance` quantity — two parameters
  expressing one concept. A tuner will happily fit them against each other into an arbitrary split.
  #99's plan resolves this (option (a): suppress the winner's king PST inside the mop-up branch); verify it
  actually landed before fitting, and audit for other such couplings.

### Open questions

- **Corpus source and licensing.** Own-match PGNs are free and self-consistent but reflect this engine's own
  biases; a public collection is more diverse. Probably both.
- **Which parameters to fit.** Fitting all ~450 PST entries plus ~20 scalars is standard but needs enough
  data to avoid overfitting. Consider fitting scalars first (fast, high-confidence), then PSTs.
- **Overfitting guard** — hold out a validation split, and treat an ELO match as the real acceptance test,
  not the objective value. A tuner that reduces training MSE while losing games has fit the corpus, not
  chess.
- **Tapered parameters double the vector.** Post-#99, every tapered term has an mg and an eg value. That is
  expected and standard, but it doubles the fitting problem and needs the corpus to span game phases.

## Measurement

The one item in the epic where a large effect is plausible, so a fixed 500-game batch may genuinely resolve
it. Use SPRT (#130) `Gain` preset anyway — a decisive early accept is cheaper than 500 games.

**Guard against the classic failure**: tuned values that improve the objective and lose games. The ELO match
is the acceptance criterion; the MSE is only the search signal. If they disagree, the match wins.

## Files likely touched

`StratEngine/Eval.h` and `StratEngine/defines.h` (regenerated constants — a large but mechanical diff), a
new tuner directory outside `StratEngine/`, `Docs/EloLog.md` (result), `Docs/Changelog.md`, and a plan/README
documenting how to re-run the tuner so this is repeatable rather than a one-off.

## Test ideas

- All existing `[eval]` tests must still pass. This is the real safety net: they assert *directions* and
  relative magnitudes rather than exact centipawn values (a deliberate choice recorded in
  `Docs/TestDesign.md`), which is exactly what makes them survive a retune while still catching a sign flip
  or an inverted table.
- #125's mirror-symmetry cases must still pass — a tuner fitting per-square values can produce an
  asymmetric table, which is fine for correctness *after* #125's fix but must be verified rather than
  assumed.
- Regenerated tables stay within sane bounds (no PST entry larger than a pawn, no negative material values)
  — a cheap sanity assertion worth adding, since an optimiser bug produces nonsense values that otherwise
  only show up as lost games.
