# Epic #110 (Evaluation Improvements) — Plan Index

Map from GitHub issue to plan doc, in execution order. Referenced from #110's body.

Created 2026-07-26 from a line-by-line review of `StratEngine/Eval.cpp` / `Eval.h` / `defines.h`, the
existing eval issues, and `Docs/EloLog.md`'s measurement history.

## Plan depth

Two depths, deliberately:

- **Full** — Goal / Design Decisions / Files Changed / Step-by-Step / Validation Plan / Key Correctness
  Properties, per CLAUDE.md's design-document requirements. Ready to hand to an implementer.
- **Sketch** — Goal, approach, settled decisions, open questions, measurement strategy, test ideas.
  For items far enough down the sequence that a full design written now would be stale before it was
  read. **Promote a sketch to a full plan when it comes up in the sequence** — do not implement from a
  sketch.

## Index

| # | Issue | Plan | Depth | Tier |
|---|---|---|---|---|
| 1 | #130 | [`elomatch-sprt-support.md`](elomatch-sprt-support.md) | Full | prerequisite |
| 2 | #125 | [`eval-color-symmetry-and-queen-pst-fix.md`](eval-color-symmetry-and-queen-pst-fix.md) | Full | 0 |
| 3 | #129 | [`uci-eval-command-term-breakdown.md`](uci-eval-command-term-breakdown.md) | Full | 1 |
| 4 | #127 | [`eval-context-restructure.md`](eval-context-restructure.md) | Full | 1 |
| 5 | #99 (+#118 item 4) | [`tapered-evaluation.md`](tapered-evaluation.md) | Full | 1 |
| 6 | #116 + #126 | [`passed-and-backwards-pawn-terms.md`](passed-and-backwards-pawn-terms.md) | Full | 3 |
| 7 | #111, #114, #115 | [`eval-quick-win-terms.md`](eval-quick-win-terms.md) | Full | 3 |
| 8 | #98 + #113 | [`mobility-evaluation.md`](mobility-evaluation.md) | Sketch | 2 |
| 9 | #97 | [`king-safety-evaluation.md`](king-safety-evaluation.md) | Sketch | 2 |
| 10 | #112 | [`minor-piece-outposts.md`](minor-piece-outposts.md) | Sketch | 4 |
| 11 | #128 (+#118 items 2, 5) | [`endgame-scale-factors.md`](endgame-scale-factors.md) | Sketch | 4 |
| 12 | #131 | [`pawn-hash-table.md`](pawn-hash-table.md) | Sketch | 4 |
| 13 | #117 | [`texel-tuning.md`](texel-tuning.md) | Sketch | 5 |
| 14 | #76 | [`eval-progress-incentive.md`](eval-progress-incentive.md) | Sketch | 5 |

**Landed**: #70 — [`mop-up-evaluation-pawnless-endgames.md`](mop-up-evaluation-pawnless-endgames.md).
Measured `+15.94 ± 27.62` over 480 games (`Docs/EloLog.md`, 2026-07-26), inside its own error bar. Open
follow-ups in #118, distributed across plans 5, 8 and 11 above.

**No plan of its own**: #118 is a follow-up register for mop-up, not a work item — its five items are
absorbed by the plans noted above. #119 (ELO-match resume support) is done, PR #121,
[`elomatch-resume-support.md`](elomatch-resume-support.md).

## Hard dependencies

Not just a preferred order — these will produce wrong or unverifiable work if violated:

```
#130 ──> everything in Tiers 3 and 4    (their effects are inside a fixed batch's noise floor)
#125 ──> #117                           (a tuner fitting an asymmetric evaluator absorbs the asymmetry)
#125 ──> #97                            (king safety introduces the first file-asymmetric PST)
#129 ──> #127                           (batch scoring is how byte-identity is verified)
#129 ──> #117                           (bulk static eval over a labelled corpus)
#127 ──> #99, #98, #97                  (they need shared per-call intermediates)
#99  ──> #116, #97, #113                (phase-scaled terms must not invent local stage checks)
#116 ──> #112, #131                     (span masks / pawn attack sets; terms that justify the cache)
```

Note the resolved near-circularity between #129 and #127: #129 ships in two phases — total score + batch
mode **before** #127, per-term breakdown **after** it. See `uci-eval-command-term-breakdown.md` D3.

## Cross-cutting conventions

Every plan in this set carries these; they are recorded once here so a reader can see they are deliberate.

- **Measurement is stated explicitly per item**: expected Elo magnitude, whether a 500-game batch can
  resolve it, and the bundling fallback when it cannot. `Docs/EloLog.md` measures ±25 Elo per 500 games
  for this engine, and most items in Tiers 3-4 are worth less than that — a plan that says "validate with
  `Run-EloMatch.ps1`" and stops is not usable. This is the single most important thing this review changed.
- **Refactors are validated by identity, not by matches.** #127 and #131 claim behaviour preservation;
  score/node identity over a corpus is stronger evidence than any ELO batch, and cheaper.
- **`eval-reviewer` dispatch before every PR touching `Eval.cpp`/`Eval.h`**, per CLAUDE.md's pre-PR
  checklist. Do not self-certify a diff as mechanical and skip the gate.
- **New `[eval]` cases plus the matching `Docs/TestDesign.md` §Evaluation entry.** That section enumerates
  every eval case, so it goes stale otherwise.
- **#125's mirror-symmetry test is a regression gate for every subsequent item.** Once color symmetry is
  established it must stay established; it is the cheapest available guard against a whole class of
  per-color eval bugs.
- **FEN hygiene in every test**: full side-to-move field (a missing one silently defaults to Black, #46)
  and verified-legal positions (illegal FENs are silently accepted, #45).
- **`EvalManager` stays stateless** unless #131 explicitly decides otherwise — the `Eval.h` class comment
  documents the Lazy SMP sharing contract that #109's review established.
