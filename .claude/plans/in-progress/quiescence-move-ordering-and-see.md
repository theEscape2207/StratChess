# Quiescence move ordering and SEE — Design

**Issues:** #320 (PR 1), #86 (PR 2 and PR 3)

## Goal

Quiescence orders its moves with a capture-only sort, and since #306 it searches captures that lose
material to resolution. Both cost nodes on paths that are now hot.

Since #311 made quiescence legal while in check, an in-check node generates every legal evasion and
hands the whole list to `MoveSorter::SortMovesByValue`, whose second parameter is literally named
`captures`. `MoveHelper::Value()` scores a quiet move as `-PieceHelper::Value(mover) / 16`, so every
quiet evasion sorts below every capture and the **heaviest** quiet sorts last. The king is the
heaviest piece that can move and a king evasion is very often the only legal reply, so the move most
likely to be best is searched last.

Separately, #306 fixed `ComputeCaptures()` so it generates officer captures — it never had. Before
that fix quiescence resolved pawn captures and promotions only, and pawn captures are rarely losing,
so the absence of a static exchange evaluation cost little. Now quiescence searches every losing
officer capture to resolution: QxP defended by a pawn, RxN defended, and so on. Delta pruning cannot
filter these — it is an optimistic material bound tested against alpha, so it prunes captures that
cannot *raise* alpha, not captures that *lose* material. Nothing else in the search declines a bad
capture.

## Scope

**This change will:**

- Order in-check quiescence through `MoveSorter::ScoreMoves`, so quiet evasions are scored by
  history rather than by `-piece/16` (PR 1).
- Correct `SortMovesByValue`'s parameter name and assert its precondition (PR 1).
- Add `bool see_ge(Move, int threshold)` and the `MoveGenerator::AttackersTo` primitive it needs,
  and use `see_ge` for capture ordering (PR 2).
- Prune captures that lose material, out of check only (PR 3).

**This change will not:**

- Give quiet moves a SEE score. Filed as #399 — the useful semantics there is "is the destination
  square defended", not "is this exchange won", and it needs its own justification.
- Introduce a pruning margin. Exactly `SEE < 0`; margin tuning is #398.
- Model pins in SEE. Standard practice, and expensive to fix; see D5.
- Change move generation. `AttackersTo` is a new query, not a new generator, so no `Run-PerftCheck`.
- Wire `see_pruning_enabled` through `game_settings.json`. In-code default only until something
  needs to tune it.

## Decisions

### D1: In-check quiescence reuses `ScoreMoves` rather than getting a bespoke evasion order

**Chosen**: call `MoveSorter::ScoreMoves` — the scorer `pvs()` already uses — with
`Move::EmptyMove()` as the hash move plus this thread's killers and history.

**Rejected**: (a) a minimal split, running MVV-LVA over the capture prefix only and leaving quiet
evasions in generation order — fixes the reported symptom by replacing bad ordering with none;
(b) the explicit evasion order #320's body suggests (captures of the attacker, then interpositions,
then king evasions) — a bespoke heuristic that bakes in the assumption king evasions *are* best,
where history can measure it instead.

The decisive argument is D2: SEE substitutes into `ScoreMoves`' existing tiers, so reusing it here
means PR 2 extends this structure rather than replacing a heuristic built two PRs earlier — the
double work #320 warned about.

`Move::EmptyMove()` is not a shortcut. `TranspositionTable.h:258` records that quiescence never
reads `best_move`, because `store()` inherits a same-key entry's move across a phase change, so an
entry can hold a quiet move this generator would never produce. Mining it would turn that
inheritance from inert into a defect.

### D2: `see_ge` replaces the `mvv_lva` term inside `ScoreMoves`' tiers, rather than adding an axis

`ScoreMoves` already partitions captures into winning (`mvv_lva > 0`), equal (`== 0`) and losing
(`< 0`). That partition is a *guess* at exactly what SEE computes *exactly*, so SEE substitutes for
the term rather than adding a parallel signal.

**Consequence, stated because it looks like an omission otherwise**: quiet moves are untouched by
PR 2 with no branch required. The `mvv_lva` term is only consulted on the `isCapture` branches; the
quiet branch falls through to history. Extending SEE to quiets is #399.

### D3: `bool see_ge(Move, int threshold)`, not `int see(Move)`

Neither consumer needs a centipawn value. Pruning needs `SEE < 0`; the D2 tier split needs only the
sign, and `see_ge(m, 0)` with `see_ge(m, 1)` gives all three tiers. The boolean form is the standard
early-exit shape: it bails the moment the result is decided instead of unwinding the full swap list.

**Rejected**: `int see(Move)` for debuggability — e.g. exposing exact exchange values through the
`eval` UCI command. No consumer wants it today, and it can be added beside `see_ge` if one appears.

### D4: SEE lives in its own translation unit

`See.h` / `See.cpp`, not `MoveHelper` alongside `Value`/`DeltaGain`. `MoveHelper.h` is a header of
small predicates; SEE is a loop over a mutated occupancy copy with Magic lookups, and putting it
there drags `Magic.h` into every `MoveHelper.h` includer.

### D5: SEE handles X-rays, ignores pins

**X-rays: handled.** Sliders are recomputed through `Magic.h` against the occupancy after each swap.
Skipping them would be cheaper, but it is wrong in exactly the battery positions — rook behind rook,
queen behind bishop — where PR 3 would then prune a capture that actually *wins*. That is a
correctness cost paid in tactics, not a speed trade.

**Pins: ignored.** A pinned defender counts as a defender even though it could not legally recapture,
so SEE can report an exchange as losing when it is not. This is standard practice; modelling pins
requires legality checking inside the swap loop and rarely pays. Recorded in a source comment so the
next reader sees a decision, not an oversight.

**Promotions**: credited at the root of the swap list only, not recursively.

**En passant**: handled explicitly. The captured pawn is not on the destination square, so any test
shaped like `move.to() == victim_square` is wrong here.

### D6: `AttackersTo` is a new primitive, because none exists

The codebase's only attack query is `MoveGenerator::GetAttackBoard(board, color)`, which builds a
whole-side attack board — `AIPerplex.cpp:597` already orders it last in a condition chain on cost
grounds. SEE needs the opposite shape: attackers of one square, against a supplied occupancy.

`MoveGenerator::AttackersTo(bbBitBoards, square, occupancy)` returns attackers of both colours.
Reusing `GetAttackBoard` per swap iteration was rejected — that shape is what would make SEE cost
more than it saves.

### D7: Pruning threshold is exactly zero, in the first pruning PR

A margin is a second tunable landing in the same change, and it confounds the SPRT that PR 3 exists
to pass: a failed `Gain` run could not distinguish "SEE pruning is wrong" from "the margin is
mistuned". #398 tunes it against a merged baseline, where the answer means something.

### D8: The `!in_check` guard is a correctness requirement, not a tuning choice

Since #237 stage 2, in-check quiescence is a full-width evasion search and `AIPerplex.cpp:921`
reports checkmate when no evasion survives. Pruning evasions could empty that list and return a
**false mate score**. The guard is not behind `see_pruning_enabled` and must not become tunable.

### D9: A tuning flag for pruning only, not for ordering

`SearchTuning::see_pruning_enabled`, following `null_move_enabled`'s existing shape. It buys a real
gate: with the flag off, PR 3's binary must be node-identical to PR 2's, which makes
`Compare-SearchEquivalence.ps1` prove the SEE plumbing introduced no behaviour change beyond the
pruning itself. PR 3 otherwise has no equivalence check, since changing the tree is its purpose.

**Rejected** for PR 1 and PR 2: a flag there would mean keeping the old ordering as dead code
forever to toggle back to, and would buy no gate — an ordering change legitimately produces a
different tree, so equivalence can never apply to it.

## Assumptions I cannot verify from the code

**The #306 cost measurements are quoted from an issue comment, not re-measured on today's `main`.**
#86's comment records #306 shrinking the main tree 19% while growing the quiescence tree 34% at
startpos and 43% at kiwipete, at a net +28% wall-clock to depth 12. Those were taken against `main`
@ `d425b8e`; `main` is now at `ceebfd5`. They are the motivation for PR 3's size, not an acceptance
criterion, so they are not being re-measured — PR 3's own `-Sprt Gain` result is what decides it.
Would be settled by re-running the interleaved depth-12 comparison with a temporary qnode probe.

**Nothing else.** The rest of this design rests on code in this repository, cited inline.

## Invariants

- Quiescence never reads `best_move` from the transposition table, in either phase (D1).
- SEE pruning is unreachable while in check (D8), so an in-check node still searches every legal
  evasion and `!moveFound` still means checkmate.
- With `see_pruning_enabled = false`, PR 3 is node-identical to PR 2 at `Threads=1` (D9).
- `SortMovesByValue`'s sorted range contains only captures and promotions — asserted, not assumed.
- `AttackersTo` does not alter which moves are generated.

## Validation

Engine tier throughout; `search-reviewer` dispatched on all three PRs.

**PR 1** — a regression test asserting a king evasion outranks a losing quiet on a known in-check
position, **falsified first**: revert the `ScoreMoves` swap and confirm the test fails before
trusting it. The new `SortMovesByValue` assert is exercised in a **Debug** build, since Release
elides it. Measurement: `-Sprt Custom -Elo0 -10 -Elo1 0`, `-Games 500`. Expected to hit the cap
without crossing a bound and **recorded as inconclusive, not as a measured zero** — the
justification is ordering correctness, not a claimed Elo win. The SPRT still earns its place: it
stops early if the change is genuinely bad, which is the risk being guarded.

**PR 2** — committed unit tests for `see_ge` against hand-computed swap positions: defended QxP,
defended RxN, an X-ray battery (rook behind rook), and en passant capture of a checking pawn. Plus a
throwaway python-chess fuzz over random positions during development, whose result goes in the PR
body — hand-picked cases miss X-ray and en-passant mistakes by construction, because you have to
think of them first. No Python dependency is added to the test suite. Measurement as PR 1.

**PR 3** — `Compare-SearchEquivalence.ps1` with `see_pruning_enabled = false` against PR 2's binary
(D9); tactical suite; `-Sprt Gain`. The strength-lab call is taken **after** that SPRT lands, not
before: `strength.yml` is a fixed pooled batch, not an SPRT, so it sizes a win rather than deciding
one, and at ~3 h it is not the first tool to reach for.

No `Run-PerftCheck` on any of the three (D6). `Run-Bench` on PR 2 and PR 3, which add per-node work.

## Harvest

Filled incrementally as each PR lands; the file is deleted in PR 3 once complete.

| Decision / rationale | Lands in | Status |
|---|---|---|
| D1 — why quiescence passes `EmptyMove()` as hash move | comment on `order_quiescence_moves()` | **done** |
| D1 — evasion ordering goes through the shared scorer | `CLAUDE.md` → Key Source Facts | **done** |
| `SortMovesByValue` precondition | the assert itself, plus the `Sort.h` declaration comment | **done** |
| Glossary: capture / quiet move / king evasion / capture of the attacker / interposition | `CONTEXT.md` | **done** |
| #320's mechanism, with the 10000-vs-900 arithmetic | `Docs/Changelog.md`, and the test comment | **done** |
| PR 1 SPRT result, recorded as inconclusive | `Docs/EloLog.md`, `Docs/Changelog.md` | PR 1, pending |
| D3 — why `see_ge` is boolean | comment on `see_ge`'s declaration | PR 2 |
| D5 — pins ignored, X-rays handled | source comment in `See.cpp` | PR 2 |
| D6 — `AttackersTo` takes an occupancy so SEE can mutate it | comment on the declaration | PR 2 |
| SEE test coverage | `Docs/TestDesign.md` | PR 2 |
| D8 — `!in_check` is correctness, not tuning | source comment at the pruning site | PR 3 |
| D9 — what `see_pruning_enabled` is for | comment on the field in `SearchTuning` | PR 3 |
| PR 3 `Gain` result and measured effect | `Docs/EloLog.md`, `Docs/Changelog.md` | PR 3 |

**Approved decisions that changed during implementation**

- **PR 1**: the ordering was extracted into `AIPerplex::order_quiescence_moves()` rather than left
  inline at the call site. Not in the design as approved. The reason is validation, not style: the
  regression test has to exercise the branch the node itself takes, or reverting the fix would
  leave it passing. With the helper, the falsification check fails exactly the ordering test and
  nothing else — verified by reverting the in-check branch and re-running.
- **PR 1**: `MoveHelper::Value()` was changed to drop the LVA term for a **king** capture. The
  design scoped PR 1 to the in-check ordering path alone. Routing that path through `ScoreMoves`
  exposed a pre-existing defect and would have shipped it as a regression: at 10000 cp the king's
  LVA penalty scored `KxR` as -125, landing it in the *losing*-capture tier below every quiet
  evasion, where the old flat sort had ranked it first. The fix is at `Value()` rather than inside
  `ScoreMoves` because both sorters compute the same wrong number from the same helper, and the
  cause is a modelling error, not a tuning choice: a king capture is legal only onto an undefended
  square, so it is never recaptured and there is no attacker to subtract. Consequence for
  validation: PR 1 also changes `pvs()` ordering, so it is **not** node-identical with `main` and
  `Compare-SearchEquivalence.ps1` does not apply to it. The SPRT measures both halves together;
  they push ordering the same direction, so the reading stays interpretable.
