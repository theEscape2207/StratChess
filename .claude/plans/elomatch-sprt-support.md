# Run-EloMatch.ps1 — SPRT Support

**Issue**: #130 · **Epic**: #110 prerequisite · **Depth**: full plan · **Status**: not started

## Goal

Let `StratChessEvolved/Scripts/Run-EloMatch.ps1` run a **sequential** match that stops as soon as the
evidence is statistically decisive, instead of always burning a fixed game count, by exposing
fastchess's built-in SPRT mode. Record the SPRT verdict (H0/H1 accepted, or inconclusive at the game
cap) in `Docs/EloLog.md` alongside the Elo point estimate.

**Scope limit**: wiring, reporting and documentation. fastchess implements the statistics — this plan
adds no new math, and must not reimplement any.

## Why this is a prerequisite for the epic, not a nice-to-have

`Docs/EloLog.md` establishes, by measurement rather than assumption, that 500 games ≈ **±25 Elo** at
95% for this engine (the two identical-build sanity batches landed at +25.1 and −27.9, pooling to
−1.4). The remaining #110 backlog is, by public engine precedent, worth roughly:

| Item | Rough expected gain |
|---|---|
| #111 Bishop pair | 10–20 Elo |
| #114 Connected rooks | 5–10 Elo |
| #115 Castling-done | 5–10 Elo |
| #112 Outposts | 10–20 Elo |
| #113 Queen activity | 5–15 Elo |

Every one is inside the noise floor. A fixed 500-game batch cannot distinguish any of them from zero,
so #110's claim that each item is "independently measurable via `Run-EloMatch.ps1`" does not hold for
most of its own contents.

This already happened once: the mop-up measurement returned `+15.94 ± 27.62` — a result equally
consistent with "+16 Elo", "no change", and "−10 Elo". The brute-force alternative (×4 games to halve
the bound) is ~2,000 games ≈ 4 hours per item, and the same environment stoppage that killed that
match at 491/500 games makes very long fixed batches operationally fragile even with `-ResumeDir`
(#119) available.

SPRT answers the question actually being asked — "is this term worth keeping?" — and typically needs
far fewer games than a fixed batch when the term is genuinely good, while returning an explicit
*reject* rather than an ambiguous "within error of 0" when it is not.

## Design decisions

**D1 — Pass through to fastchess `-sprt`; add no statistics.** fastchess supports
`-sprt elo0=<n> elo1=<n> alpha=<f> beta=<f>` natively and prints running LLR against the bounds. The
script's job is parameters in, verdict out.

**D2 — Two presets plus full manual control.** Most callers should not be choosing alpha by hand:

| Preset | Bounds | Use |
|---|---|---|
| `-Sprt NonRegression` | `elo0=-5 elo1=0` | Refactors, restructures (#127), anything expected neutral — "prove it did not make things worse" |
| `-Sprt Gain` | `elo0=0 elo1=10` | Small new terms (#111/#113/#114/#115/#112) — "prove it is worth ≥ ~10 Elo" |

with `-Elo0`/`-Elo1`/`-Alpha`/`-Beta` available to override any of it. Defaults `alpha=beta=0.05`.
Rationale for two presets rather than one: a non-regression test and a gain test are different
questions with different bounds, and conflating them is how a neutral change gets recorded as a win.

**D3 — `-Games` becomes a hard upper bound in SPRT mode, not a target.** SPRT must be able to stop
early *and* must not be able to run forever. Keep `-Games` as the cap, and validate at parameter-parse
time that `-Sprt` and `-Smoke` are not combined (a 20-game smoke run can never reach an SPRT decision;
silently allowing it would produce a meaningless "inconclusive" row).

**D4 — An inconclusive run is a first-class outcome, recorded as such.** Hitting the game cap without
crossing a bound is real information ("this term is smaller than `elo1`, or we ran out of budget"),
and must not be recorded in a way that reads like a measured Elo. `Docs/EloLog.md` gets a verdict
convention: `H1 accepted` / `H0 accepted` / `inconclusive @ N games`.

**D5 — Verify the log-parse regex against the pinned fastchess build; do not write it from memory.**
The existing script already carries a hard-won `'^\s*Elo:'` parse. The SPRT verdict line's exact text
varies across fastchess versions, and a regex that silently matches nothing degrades to "inconclusive"
on every run — a failure mode that looks like a *result*. Run one short SPRT match with deliberately
wide bounds (so it terminates in a handful of games), capture the raw log, and derive the pattern from
that actual output. This step is not optional and is why this plan does not quote a regex.

**D6 — Preserve `-ResumeDir` compatibility.** An SPRT run is *more* likely to be long than a fixed
batch, so resume matters more. `-ResumeDir` already bypasses all match-setup parameters by design
(fastchess's `-config` restores the tournament spec, SPRT settings included) — the requirement is to
confirm that holds and document it, not to add new resume logic.

## Files changed

| File | Change |
|---|---|
| `StratChessEvolved/Scripts/Run-EloMatch.ps1` | `-Sprt`/`-Elo0`/`-Elo1`/`-Alpha`/`-Beta` params; preset resolution; validation; `-sprt` args; verdict parse; verdict in the EloLog row |
| `Docs/EloLog.md` | SPRT procedure section; verdict convention; when to use SPRT vs a fixed batch; the bundling fallback |
| `CLAUDE.md` | One line in the validation-scripts table noting SPRT mode |
| `Docs/Changelog.md` | Dated entry |

## Step-by-step

### 1. Parameters and validation

Add to the `param()` block, following the existing commented-parameter style (every parameter in this
script carries a *why* comment — match that):

- `[string]$Sprt = ''` — `''` (off), `NonRegression`, `Gain`, or `Custom`.
- `[int]$Elo0`, `[int]$Elo1`, `[double]$Alpha = 0.05`, `[double]$Beta = 0.05`.

Resolve the preset to concrete bounds after the param block. Validate:

- `-Sprt` with `-Smoke` → error and exit 1 (D3).
- `-Sprt Custom` without both `-Elo0` and `-Elo1` → error.
- `elo0 >= elo1` → error (the bounds would be inverted).

Emit the resolved bounds in the existing `Write-Host "==> …"` banner, so the log records what was
actually tested rather than which preset name was typed.

### 2. fastchess invocation

In the fresh-match branch (around `Run-EloMatch.ps1:227`), build the SPRT tokens conditionally and
splat them, mirroring how `$candidateEngineArgs` already handles optional tokens:

```powershell
$sprtArgs = @()
if ($Sprt -ne '') { $sprtArgs = @('-sprt', "elo0=$Elo0", "elo1=$Elo1", "alpha=$Alpha", "beta=$Beta") }
```

then add `@sprtArgs` to the existing `& $fastchess` call. `-rounds $rounds` stays as-is — that is what
makes `-Games` the cap (D3). Do not touch the resume branch (D6).

### 3. Verdict parse and reporting

Extend the existing parse block (`:243-261`). Keep the current `Elo:`/`Games:` extraction unchanged —
the point estimate is still wanted, it is just no longer the whole answer — and add the SPRT verdict
extracted per the pattern derived in D5. Where no verdict line is found *and* SPRT was requested,
record `inconclusive @ N games`; where SPRT was not requested, the row is unchanged from today.

### 4. `Docs/EloLog.md`

Add a **"Choosing SPRT vs a fixed batch"** section stating plainly:

- Fixed batch: when a point estimate with an error bound is what is wanted (baselines, sanity runs,
  large expected effects like the Lazy SMP threads=4 row at +128.55 ± 28.36).
- SPRT: when the question is accept/reject and the effect is expected to be small — i.e. most of #110.
- **The bundling fallback**: for terms too small to resolve even under SPRT at a practical game count,
  combine several into one measured PR. Say explicitly that bundling is a deliberate measurement
  decision, not sloppy scoping — otherwise the next person reads a three-term PR as a process failure.

Extend the measurement-history table's Notes convention with the verdict, and update the
"Interpreting results" section, which currently frames everything in fixed-batch terms.

## Validation plan

This is a PowerShell script change, so the engine test suite proves nothing about it. Validate the
script itself:

1. **Syntax** without executing:
   `[System.Management.Automation.Language.Parser]::ParseInput($content, [ref]$tokens, [ref]$errors)`
   (per CLAUDE.md — and edit the `.ps1` via a Python helper script, not inline `sed`, which reliably
   mangles Windows backslashes and PS line-continuation backticks).
2. **Parameter validation paths** — each rejection in step 1 exits 1 with a clear message:
   `-Sprt Gain -Smoke`, `-Sprt Custom` with no bounds, `-Elo0 10 -Elo1 0`.
3. **A real short SPRT match** with deliberately wide bounds (e.g. `-Sprt Custom -Elo0 -200 -Elo1 200`)
   against the pinned reference. This must terminate in far fewer games than `-Games` and is also where
   the D5 parse pattern comes from. Confirm the `Docs/EloLog.md` row carries the verdict.
4. **Regression check on the default path**: a `-Smoke` run with no `-Sprt` must behave exactly as
   before, including its EloLog row format. This is the change's main risk — the script is the
   instrument every other #110 item depends on, so breaking the existing path costs more than the
   feature gains.
5. **Resume interaction** (D6): start an SPRT match, kill it after a checkpoint, resume with
   `-ResumeDir`, and confirm it continues under the original SPRT bounds.

Revert the `Docs/EloLog.md` rows produced by steps 3-4 if they are throwaway, or mark them `smoke` —
do not leave calibration runs in the history table looking like measurements.

## Key correctness properties

1. **Default path unchanged**: with no `-Sprt`, every argument passed to fastchess and every line
   written to `Docs/EloLog.md` is byte-identical to today's.
2. **Bounded**: an SPRT run never exceeds `-Games`.
3. **No silent inconclusiveness**: a missing/unmatched verdict line must be reported as inconclusive
   *and* visibly flagged in the console output — never rendered as if a decision was reached.
4. **Verdict and estimate agree in sign**: an `H1 accepted` row with a negative point estimate means
   the parse is wrong. Worth an explicit consistency check in the script rather than trusting it.
5. **Resume preserves SPRT settings**: resuming does not silently downgrade a sequential test to a
   fixed batch.
6. **No statistics reimplemented in PowerShell**: LLR and bound-crossing come from fastchess output
   only.
