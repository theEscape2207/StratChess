# Why measurement is hard here

Each of these has cost this project a run, a wrong conclusion, or both. Sizing and book mechanics
are in `SKILL.md`; this file is the traps that survive getting those right.

## Resolution: what 500 games can see

500 games ≈ **±25 Elo** at 95% confidence, given this engine's ~37% draw ratio. That is measured,
not assumed — the ±15 rule of thumb assumes a higher draw rate than this engine produces. Error
scales as 1/√N, so halving the bound costs 4× the games: ±25 at 500 → ±12.5 at 2,000 → ±6 at 8,000.

Most single eval terms are worth 5–20 Elo. **A 500-game fixed batch cannot distinguish any of them
from zero** — the mop-up row (`+15.94 +/- 27.62`) is a result equally consistent with "+16 Elo",
"no change" and "−10 Elo".

**Single batches genuinely wander.** The two identical-build sanity batches landed at +25.1 and
−27.9, both at the edge of their own error bars, pooling to −1.4. Treat any single-batch result near
the bound as unresolved: re-run and pool before acting on it.

## The anchor measures the sum, not your change

`elo-reference-v1` and `-v2` are **fixed** anchors, so every row against one measures cumulative
progress since it — the engine's standing, not the PR's delta. That is the right instrument for
tracking the project and the wrong one for deciding whether one small change earned its place:

- **An SPRT against the anchor tests the sum.** H1 means "`main` + this change beats the anchor by
  more than `elo1`", which can be true on `main`'s pre-existing margin alone. A verdict on a sum
  licenses no claim about one addend. The 2026-07-29 `candidate-08d4ef8` row in
  `Measurements/local.md` accepted H1 in 23 minutes on a comparison that says nothing about the
  three terms it was run for.
- **The delta cannot be recovered by subtraction.** Differencing two anchor rows compounds their
  errors, so the result is less constrained than either input — and most rows are individually
  inconclusive to begin with.

So to decide whether a change helps, build the merge base and pass it via `-ReferenceExe`, with
`-ReferenceTag` naming that commit. Keep the anchor run too when the cumulative figure is wanted;
the two answer different questions and both belong in the ledger, labelled as to which is which.

`Run-EloMatch.ps1` enforces this rather than trusting it: `-Sprt` exits 1 when the reference comes
from the tag lookup, because such a reference is a fixed anchor by construction. `-AnchorSprt`
overrides the refusal for a cumulative reading asked for on purpose, and labels the row accordingly.

## The 100 ms per-move floor

`compute_budget()` floors every move at 100 ms regardless of how much clock is left, so at any
increment below that the engine loses ground every move once its clock drains, and eventually
forfeits. Measured: **3 time losses in 4 games at 5+0.05, none at 5+0.1**; at 2+0.02 the handicapped
side flagged in all four.

Two consequences. A handicap run must halve the **base** time and leave the increment alone —
halving both tests the time manager's floor rather than whatever is under test. And the standard
10+0.1 sits *exactly* on the floor: its 100 ms increment repays the minimum move cost and no more,
so there is no margin on a slower or contended machine. Tracked as #204.

## When even SPRT cannot resolve a term

Some terms are too small to decide at any practical game count. The fallback is to **bundle several
into one PR and measure them jointly**. That is a deliberate measurement decision, not sloppy
scoping — but say so explicitly in the row's detail section, so a later reader does not attribute
the whole delta to whichever term the commit message happens to mention first.
