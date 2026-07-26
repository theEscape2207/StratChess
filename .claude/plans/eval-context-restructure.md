# EvalContext Restructure

**Issue**: #127 · **Epic**: #110 Tier 1 (enabler) · **Depth**: full plan · **Status**: not started

## Goal

Reshape `EvalComplex::Evaluate` (`StratEngine/Eval.cpp:75`) from one ~130-line function built around a
12-way `switch` into a small `EvalContext` holding shared intermediates plus one function per
evaluation term — **with byte-identical scores** on a position corpus before and after.

**Scope limit**: pure restructure. No new term, no changed weight, no changed behaviour. Anything that
moves a score by 1 cp is out of scope and belongs to the issue that owns that term.

## Why now, before the Tier 2 terms

The current shape works for the six terms it has. It is the wrong shape for the next three, each of
which needs state that a `switch` case body has nowhere to put:

- **#99 Tapered Eval** — a phase value and a midgame/endgame score *pair* threaded through every term,
  not a boolean stage consulted ad hoc in three places.
- **#98 Mobility** — occupancy and own-piece masks available per piece, and per-piece-type bitboard
  loops rather than a `switch` on a mailbox lookup.
- **#97 King Safety** — per-color king zones and attacker counts *accumulated across piece types*,
  i.e. state that must survive the whole loop.

Bolted onto today's structure, each adds another `if (gameStage == …)` and another captured local to
what is already the hardest-to-review function in the engine. And #117 then has to reach into whatever
that becomes and parameterise it — a term-per-function layout with named constants in one place is
dramatically easier to tune than constants scattered through `switch` cases.

Secondary benefit that unblocks better testing: `EvalTests.cpp` can currently only assert
whole-position scores and A-vs-B deltas between hand-built FENs, because no term is separately
callable. Per-term functions make term-level unit tests possible.

## Design decisions

**D1 — Byte-identical is the acceptance criterion, and it is checked mechanically.** Use #129's batch
FEN-scoring mode: score a corpus before the change, score it after, diff. Not "spot-checked a few
positions" — a full diff. This is the whole reason #129 is sequenced first (see that plan's D3, which
splits #129 into a total-score phase before this work and a breakdown phase after it).

Corpus: FENs harvested from existing assets rather than invented — `Tests/perft_test_cases.json`,
`tactical_test_cases.json`, the `EvalTests.cpp` FEN constants, plus positions extracted from a PGN under
`StratChessEvolved/logs/elo/` for realistic middlegame material. A few thousand positions spanning all
game stages is ample and costs nothing to score.

**D2 — Preserve the two structural quirks exactly, then comment them.** Both are easy to "clean up"
into a behaviour change:

- The generic PST add is gated on `pieceType != (KING >> 1)`, and the king's PST is instead applied
  inside its own `switch` case from `g_Eval_Bitboards[5]` or `[6]` by stage. Kings must still receive
  exactly one PST contribution, from the stage-selected table.
- The final material term comes from `Board::GetMaterialScore`, which **includes the king at 10000 cp**
  (`g_iPieceValues`, `defines.h:161`). It cancels in the difference. Do not "fix" it here — #99 needs
  it understood, not changed (see D4).

**D3 — Term functions take `(const EvalContext&, eColor)` and return a score for that color.** Uniform
signature, individually unit-testable, and no term touches `bonusScore[]` directly — the caller sums.
Today's per-color accumulation into `bonusScore[2]` inside a single loop is what makes it impossible to
attribute a score to a term.

Proposed context (final shape is the implementer's call; this is the required content):

```cpp
struct EvalContext {
    const Board&             board;
    std::span<const BITBOARD> boards;      // Board::GetBitBoards()
    BITBOARD                 all_pieces;
    BITBOARD                 pawns[NUM_COLORS];
    BITBOARD                 occupied[NUM_COLORS];   // ALL_WHITE_PIECES / ALL_BLACK_PIECES
    eSquare                  king_sq[NUM_COLORS];
    int                      material[NUM_COLORS];
    EvalManager::PlayState   stage;                  // becomes a phase int in #99
};
```

built once per `Evaluate()` call. Everything in it is already computed or trivially available today —
this is not new work per node, it is the same work named.

**D4 — Do not touch phase detection.** `iMinScore <= 11500` stays exactly as-is, including the
king-value-inclusive comparison and the `min()` over both sides. It is wrong in two ways (documented in
#127 and in the #99 plan) and fixing it changes scores, which violates D1. Carry the *explanation* into
a comment on the context field so the next reader understands the `11500` without re-deriving it, and
leave the behaviour to #99.

**D5 — Per-piece-type bitboard loops replace the mailbox+`switch` pass.** Iterate each piece bitboard
directly (`boards[WHITE_KNIGHT]`, …) with `Board::GetFirstPiece` + `Bits::clearLsb`, dropping the
`board.GetPiece(square)` mailbox lookup per piece. Incidental perf win; the real gain is that a term
function for knights sees only knights.

Watch the accumulation-order question: reordering `int` additions cannot change the result (no overflow
at these magnitudes, no floating point), so per-type loops are safe under D1. Confirm no term reads
state another term mutates — none does today, and none may be introduced here.

**D6 — Keep `EvalManager` stateless.** `Eval.h`'s class comment documents the Lazy SMP sharing
contract: no mutable members, `Evaluate()` `const`, safe to share unsynchronized across every helper
thread. `EvalContext` is a per-call stack local, which preserves that. Nothing in this restructure may
become a member. (#131's pawn hash is the change that would break this contract — see that plan.)

**D7 — `EvalSimple` is out of scope.** It is a separate material+PST evaluator used for testing.
Leave it alone; folding it in expands the diff for no benefit.

## Files changed

| File | Change |
|---|---|
| `StratEngine/Eval.h` | `EvalContext` struct; per-term static function declarations |
| `StratEngine/Eval.cpp` | `Evaluate()` becomes context construction + term summation; terms extracted |
| `StratChessTests/EvalTests.cpp` | Term-level cases now that terms are callable |
| `Docs/TestDesign.md` | §Evaluation case list |
| `Docs/Changelog.md` | Dated entry |

## Step-by-step

Work in small, individually-verifiable steps — after **each** one, re-run the corpus diff (D1). A
restructure verified only at the end gives no information about which step broke identity.

1. **Capture the baseline.** Score the corpus with the current binary; save the output. This artifact is
   the acceptance criterion for every step below.
2. **Introduce `EvalContext`** and populate it at the top of `Evaluate()`, leaving the existing loop
   reading from it instead of its own locals. No logic moves yet. Corpus diff → identical.
3. **Extract the pawn terms** (doubled, isolated) into `eval_pawns(ctx, color)`. Corpus diff.
4. **Extract the rook terms** (7th rank, half-open/open file). Corpus diff. Note the open-file test's
   `all_black`/`all_white` bug (#126) is preserved verbatim here — fixing it is #126's job, in the
   #116 PR. Add a `// see #126` comment rather than a silent fix; a reviewer who spots it mid-restructure
   will otherwise "helpfully" correct it and break identity.
5. **Extract the PST term**, preserving the king exclusion and the stage-selected king table (D2).
   Corpus diff.
6. **Extract the mop-up term** into `eval_mopup(ctx)` — it is already well-isolated at the tail of
   `Evaluate()` and moves nearly verbatim. Corpus diff.
7. **Convert to per-piece-type loops** (D5). Corpus diff — this is the step most likely to break
   identity, which is why it is last and separate.
8. **Add term-level tests** now that terms are individually callable.

## Validation plan

```powershell
.\build.ps1 all                      # Level4 + /WX
.\build.ps1 run-tests "[eval]"
.\build.ps1 run-tests                # full fast tier
.\build.ps1 extended-tests           # includes [tactical_full]
```

**Primary evidence — corpus score identity.** The before/after diff must be empty. Report the corpus
size and the empty diff in the PR body; "tests pass" is not the claim being made here.

**Secondary evidence — node-count identity.** At a fixed depth on a handful of positions with
`threads=1`, node counts must match exactly. Identical scores at every node imply identical search
trees, so any node-count difference means a score changed somewhere the corpus did not cover. Keep
`threads=1`: Lazy SMP is nondeterministic and would mask the signal (see
`feedback_thread_count_test_methodology`).

**ELO**: expected exactly zero, by construction. Score+node identity is strictly stronger evidence than
an ELO match for a pure refactor (`Docs/EloLog.md` says as much), so no match is required. If SPRT
(#130) is available and a check is wanted anyway, `-Sprt NonRegression` is the right form — but do not
substitute it for the identity checks.

**Pre-PR**: `Scripts\Validate-PrePR.ps1`, then dispatch the `eval-reviewer` subagent per CLAUDE.md.
This is a large `Eval.cpp` diff and exactly the case the reviewer gate exists for — do not self-certify
it as mechanical (see `feedback_dispatch_specialized_reviewers`).

Before merging, strip any step-N / gate-N scaffolding references from comments (see
`feedback_no_task_refs_in_comments`); point durable design notes at this plan by filename instead.

## Key correctness properties

1. **Score identity**: `Evaluate()` returns the same value for every position in the corpus, before and
   after. Non-negotiable.
2. **Node-count identity**: fixed-depth, single-threaded searches produce identical node counts.
3. **King PST applied exactly once**, from the stage-selected table (D2).
4. **Material still includes the king symmetrically** and still cancels (D2).
5. **Phase detection untouched** — `11500`, the king inclusion, and the `min()` all preserved (D4).
6. **`EvalManager` remains stateless**; the Lazy SMP sharing contract in `Eval.h` still holds verbatim
   (D6).
7. **No term reads state another term writes** — terms are pure functions of the context.
8. **#126's open-file bug is preserved, not fixed** — a deliberate non-change, commented as such.
