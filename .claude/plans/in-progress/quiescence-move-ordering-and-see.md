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
  and use `see_ge` to select the capture tier (PR 2).
- Give non-capturing promotions an explicit tactical tier, which they do not have today (PR 2).
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

### D2: `see_ge` selects the capture tier inside `ScoreMoves`; MVV-LVA remains the score within it

`ScoreMoves` partitions captures into winning (`mvv_lva > 0`), equal (`== 0`) and losing (`< 0`).
**Two of those three cells are unreachable.** `MoveHelper::Value()` subtracts the attacker at 1/16
weight, so a capture's victim contributes at least 100 — an en-passant victim is a pawn — while the
attacker deducts at most `900 >> 4` = 56, the king's weight being capped at a queen's since #320.
Every capture therefore scores at least 44, and every promotion at least `300 - 100 - 6` = 194.
`SortTests.cpp`'s header already notes "losing captures (none in this position)"; the accurate
statement is that there are none in any position.

So this is not a heuristic being sharpened. PR 2 **populates two tiers for the first time**. That is
a larger behaviour change than "substitute a term" suggests, and it is why PR 2 carries its own
SPRT rather than riding on PR 1's.

**Chosen**: a single `see_ge(mv, 0)` splits captures into two tiers, with `MoveHelper::Value()`
as the score *within* each. A boolean cannot order two captures that share a tier (D3), and the tier
base constants already leave room for a secondary score.

**Resulting order**, for both `ScoreMoves` callers (D10):

> hash → SEE >= 0 captures and all promotions (D11) → killer0 → killer1 → SEE < 0 captures →
> history quiets → generation order as the final tie-break

**This replaces the three-tier order first approved here**, which put SEE-equal captures below the
killers and SEE-losing captures below the quiets. That order was implemented, benched, and measured
at **+56% nodes**; the order above measures **−14%**. The full matrix and the mechanism are in
`PR 2 measurement` below, and the two placements are load-bearing in opposite ways:

- *SEE < 0 captures stay above the quiets.* Demoting them below is what costs most. Captures are
  never LMR-reduced wherever they sit, so postponing one saves no depth, while every quiet it steps
  over gets a lower legal-move number and a weaker reduction. `update_history` clamping to
  `[0, 16'384]` with no penalty path makes it worse rather than safe: a zero-scored quiet is
  *unmeasured*, not *demonstrated better than a losing capture*.
- *SEE-equal captures ride with the winning ones, above the killers.* Splitting them into a third
  tier beneath the killers costs a further ~18% and buys nothing measurable.

Killers are the only thing that jumps a losing capture, and killers are already LMR-exempt, so this
arrangement improves cutoff ordering without reallocating selective depth anywhere.

The remaining tier invariant is a spacing one: the losing tier sits at `700'000 + mvv_lva` and must
stay below killer1 at `800'000`. `MoveHelper::Value()` peaks at 1,694 (a queen capture that is also
a queen promotion), so the margin is ~98,000 — not tight, but it is the reason the constants are
not packed closer.

**Consequence, stated because it looks like an omission otherwise**: quiet moves are untouched by
PR 2. SEE is consulted only on the capture branches; the quiet branch falls through to history.
Extending SEE to quiets is #399.

### D3: `bool see_ge(Move, int threshold)`, not `int see(Move)`

Neither consumer needs a centipawn value. Both want the same predicate: D2's tier split and PR 3's
pruning are each exactly `see_ge(m, 0)`. The boolean form is the standard early-exit shape — it
bails the moment the result is decided instead of unwinding the full swap list.

(As first approved, D2 used two calls, `see_ge(m, 1)` and `see_ge(m, 0)`, to cut three tiers. The
revised two-tier order needs one. The argument for the boolean is unchanged and if anything
stronger, but the second consumer it cited no longer exists.)

**Rejected**: `int see(Move)` for debuggability — e.g. exposing exact exchange values through the
`eval` UCI command. No consumer wants it today, and it can be added beside `see_ge` if one appears.

The boolean form is also precisely why MVV-LVA survives inside the tiers (D2): the sign decides the
tier, and a boolean has nothing left to say about two captures that land in the same one.

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

**A king attacker terminates the swap.** The king's 10000 cp notional value would otherwise dominate
the arithmetic. The standard shape applies: if the side to recapture could only do so with its king
while the opponent still has an attacker, that recapture is unavailable and the swap ends there. A
useful consequence for the pseudo-legal problem #320 ran into — a king capture onto a defended
square is illegal, and SEE reports it as losing, so PR 2 sorts it last on evidence rather than on
the capped LVA weight PR 1 had to invent.

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

### D10: One tier policy for both `ScoreMoves` callers — no context parameter

`ScoreMoves` serves main `pvs()` and in-check quiescence, and PR 2 changes what its capture tiers
mean for both. **Chosen**: identical tiers in each, so in check a SEE-losing capture sorts below the
killers — reversing, for that one class of move, the all-captures-first order PR 1 produced. It
still outranks every quiet evasion, so the reversal is narrower than the three-tier order D2 first
approved would have made it.

**Rejected**: keeping PR 1's evasion order in check via a context parameter or a second scorer. That
is a second policy to maintain and a branch in a hot loop, bought for a list that averages a handful
of moves and is searched full-width, which is where ordering buys least. If measurement later says
evasions want their own policy, the parameter can be added then — on evidence, the standard D1 held
itself to.

This does not weaken D8: ordering is not pruning. In check every legal evasion is still searched,
losing captures included, so `!moveFound` still means checkmate.

### D11: Non-capturing promotions get a tactical tier; capture-promotions stay in the SEE tiers

A non-capturing promotion fails `IsCapture()`, so today it falls past both killer checks and is
scored by `history[side][from][to]` — ranked against ordinary quiets, with no credit for the queen
it makes. SEE tiering would not reach it either, since D2 consults SEE only on the capture branches.

**Chosen**: non-capturing promotions join the winning tier, scored by `MoveHelper::Value()`, with no
SEE test. `Value()` already ranks them by promotion gain — queen 794, rook 394, bishop and knight
194 — so under-promotions stay beneath a queen promotion without a second rule.

**No SEE test on them, deliberately.** SEE would score a queen promotion onto a defended square as
`+800 - 900 = -100` and file it below the quiets. That is wrong far more often than right: the pawn
was promoting anyway, and the exchange SEE models is not the one that decides the move.

Capture-promotions satisfy `IsCapture()` and stay in the SEE tiers, where D5 credits the promotion
at the root of the swap list — so they keep both the captured material and the promotion gain.

**Constraint this places on #398**: PR 3 prunes `SEE < 0` exactly (D7), and the worst a
capture-promotion can score is `100 - 100 = 0`, a promotion-capture of a pawn. No promotion is
prunable today, so PR 3 needs no guard. A margin would change that, and #398 must not make a
promotion prunable.

**Not in scope**: promotions also enter the killer and history tables — both exclude captures and
nothing else, and history is indexed by `from`/`to` alone, so all four promotion variants of one
push share a slot. That is a `pvs()`-wide defect needing its own measurement; filed as #406.

### D12: MVV-LVA and SEE are computed only on the branch that consumes them

`ScoreMoves` calls `Board::GetEffectiveMovPiece` and `Board::GetCapturedPiece` for every move,
including killers and quiets whose score never uses the result. PR 2 moves that computation inside
the capture/promotion branch.

The reason is not speed. The compiler may already sink it, and no speed claim is made without a
measurement. The reason is that `see_ge` must not be called on a quiet move at all, so the branch
has to move regardless. `Run-Bench` on PR 2 covers the cost question; no separate validation.

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
- Before PR 2, every capture and promotion scores `MoveHelper::Value() > 0`, so the losing tier is
  unreachable and PR 2 is what first populates it (D2).
- `MoveHelper::Value()` never exceeds 1,694, so the losing-capture tier at `700'000 + mvv_lva` stays
  below killer1 at `800'000` (D2).
- No promotion is reachable by SEE pruning (D11). For capture-promotions this is unconditional: the
  promotion is credited at the root, so the post-root swap is `100 - victim <= 0` and `see_ge(m, 0)`
  is always true. Non-capturing promotions are a different case — `see_ge` on one *can* return false
  (a queen promotion onto a defended square does) and they are safe only because `ScoreMoves`
  short-circuits on `!IsCapture()`. **PR 3's pruning site needs that same guard.**

## Validation

Engine tier throughout; `search-reviewer` dispatched on all three PRs.

**PR 1** — a regression test asserting a king evasion outranks a losing quiet on a known in-check
position, **falsified first**: revert the `ScoreMoves` swap and confirm the test fails before
trusting it. The new `SortMovesByValue` assert is exercised in a **Debug** build, since Release
elides it. Measurement: `-Sprt Custom -Elo0 -10 -Elo1 0`, `-Games 500`. Expected to hit the cap
without crossing a bound and **recorded as inconclusive, not as a measured zero** — the
justification is ordering correctness, not a claimed Elo win. The SPRT still earns its place: it
stops early if the change is genuinely bad, which is the risk being guarded.

**PR 2** — two groups of committed tests, kept deliberately small.

*`see_ge` against hand-computed swap positions*: defended QxP, defended RxN, an X-ray battery (rook
behind rook), en passant capture of a checking pawn, and a king capture onto a defended square
(D5 — the swap must terminate rather than let 10000 cp dominate). Plus a throwaway python-chess fuzz
over random positions during development, whose result goes in the PR body — hand-picked cases miss
X-ray and en-passant mistakes by construction, because you have to think of them first. No Python
dependency is added to the test suite.

*Ordering, in `SortTests.cpp`*: three tests, not one per tier boundary.

1. **The tier chain in one position**: a SEE-winning and a SEE-equal capture both above killer0,
   killer0 above a SEE-losing capture, that above a positive-history quiet. The equal capture is
   there to pin that it shares the top tier rather than getting one of its own.
2. **The intra-tier score**: two SEE-winning captures order by `MoveHelper::Value()` — the contract
   D2 and D3 exist to pin, and the one a boolean SEE would silently drop.
3. **The promotion tier** (D11): a non-capturing queen promotion lands in the winning tier and
   outranks the knight promotion from the same square.

Deliberately **not** added: a generation-order tie-break test, already pinned by PR 1; and a test
per documented tier placement. Placement is chosen policy that PR 2's own SPRT is judging (D2), and
pinning each boundary separately would make the next revision expensive for no extra coverage.

Measurement as PR 1.

**PR 3** — `Compare-SearchEquivalence.ps1` with `see_pruning_enabled = false` against PR 2's binary
(D9); tactical suite; `-Sprt Gain`. The strength-lab call is taken **after** that SPRT lands, not
before: `strength.yml` is a fixed pooled batch, not an SPRT, so it sizes a win rather than deciding
one, and at ~3 h it is not the first tool to reach for.

No `Run-PerftCheck` on any of the three (D6). `Run-Bench` on PR 2 and PR 3, which add per-node work.

## PR 2 measurement

D2 as first approved was implemented, benched, and **replaced**. This section records what was
measured, because the reasoning that produced the original order was sound and still reached the
wrong answer — the failure mode is worth keeping.

### Method

`Run-Bench.ps1`, the 8 bench positions, `Threads=1`, clang-cl Release. Baseline is `origin/main` @
`6a265bc` (PR 1 merged), built in its own worktree — same compiler, so the mixed-toolchain trap does
not apply.

Node counts are the measure here, not nps. Nodes at fixed depth are normally an *equivalence* check
and not a performance one, but that is precisely because they are a property of the search rather
than of the machine code: an ordering change is a change to the search, so tree size is its effect.
nps is the separate question of what SEE costs per node, and it is flat across every variant below —
worst case −1.0%. So `see_ge` and `AttackersTo` cost essentially nothing per node, D12's concern is
settled, and every difference in the table is tree size and nothing else.

### The matrix

The two placements are independent, so there are four combinations. Total nodes at depth 12,
against a baseline of 17,610,351:

| | **SEE < 0 above quiets** | **SEE < 0 below quiets** |
|---|---|---|
| **SEE-equal above killers** | **15,119,653 (−14.1%)** ← chosen | 23,385,421 (+32.8%) |
| **SEE-equal below killers** | 19,449,264 (+10.4%) | 27,513,514 (+56.2%) ← D2 as approved |

Both effects are first-order, and neither is a correction to the other: keeping losing captures
above the quiets is worth 29–35 points, and lifting equal captures above the killers 15–22.

**The original design measured the three worst cells and not the best one.** Not because the
reasoning was careless — D2 argued each placement separately and correctly against the constants
already in the file — but because it never varied the two together. The winning combination is the
one where nothing except the killers moves past a losing capture.

Depth-robustness of the chosen order:

| depth | baseline | chosen | Δ |
|---|---|---|---|
| 10 | 6,201,815 | 5,458,806 | −12.0% |
| 11 | 9,876,651 | 8,996,683 | −8.9% |
| 12 | 17,610,351 | 15,119,653 | −14.1% |

**All eight best moves match the baseline at all three depths.** The three rejected variants each
changed at least one.

Per position at depth 12: open-mid −22.6%, startpos −22.0%, closed-mid −21.3%, tactical-5 −19.0%,
kiwipete −8.6%, tactical-4 −7.0%, rook-endgm −3.8%, **piece-endgm +17.5%**. Seven of eight improve;
piece-endgm is the one position that costs, and it is consistently so (+13.6% at depth 11, −0.6% at
depth 10). Recorded because "every position improved" would be false.

### Mechanism

Captures are never LMR-reduced, wherever they sit in the order. So moving a losing capture later
saves no search depth — it only pushes quiets earlier, lowering their legal-move number and therefore
weakening the reduction `applyLMR` gives them. The tree pays for the promoted quiets and gets nothing
back.

D2 cited `update_history`'s clamp to `[0, 16'384]` with no penalty path as the reason a losing tier
below the quiets is *safe*. The clamp is real; the inference was backwards. With no penalty path a
zero-scored quiet is one history has never measured, not one history has judged better than a losing
capture, so the demotion buries a capture behind an arbitrary pile of unknowns in generation order.

Killers are the exception that makes the chosen order work: they are already LMR-exempt, so promoting
them past a losing capture reorders cutoffs without shifting any ordinary quiet's move number.

### Quiescence growth is downstream

Out-of-check quiescence still sorts through `SortMovesByValue`, which PR 2 does not touch; SEE
reaches quiescence only on the in-check path (D10). Restoring PR 1's ordering inside in-check
quiescence alone changed zero main-tree nodes and 6,283 of 3.4M qnodes — 0.18% — at depth 10. So the
quiescence column moves because a different main-tree order visits different leaves, not because
PR 2 reordered anything inside quiescence.

### Interaction with #363

#363 (keep one main-tree ply at the `lmr_min_depth` boundary) was spiked against both orderings and
is **not** part of PR 2. On the rejected D2 ordering it shrank the node gap slightly, which read as a
small independent improvement. On the chosen ordering it costs +15.0% at depth 10 and +9.4% at depth
12. It was compensating for bad ordering, not improving on good ordering. Numbers and the
recommendation to deprioritise are in #363; the transferable point is that **#363 cannot be measured
against a baseline whose move ordering is about to change.**

### Validation performed

- Full fast suite green (527 cases), Release and Debug.
- The three hand-written `see_ge` cases that pin decisions — mid-swap x-ray, en-passant victim off
  the destination square, king-terminates-swap — were each **falsified**: breaking the corresponding
  code fails exactly that test and nothing else. The first x-ray FEN did not falsify and was
  replaced (the white back rook is uncovered by the root move, before the swap starts; the black one
  only mid-swap).
- Independent fuzz against a python-chess reference SEE: **54,513 pseudo-legal capture and promotion
  moves, 0 mismatches** — including 76 en passant, 660 promotions, 1,157 king movers, and 2,251
  moves that are pseudo-legal only (1,553 leaving the king in check, 377 king-into-attack, 321
  pinned-piece). Pseudo-legal coverage is the point: `ScoreMoves` is fed
  `MoveGenerator::ComputeLegalMoves`, which is pseudo-legal despite its name, so those three classes
  are live inputs to `see_ge` and an earlier legal-moves-only fuzz never reached them. The harness is
  not committed; no Python dependency is added to the suite.

### Strength

`-Sprt Gain [0, 10]`, 500 games at 10+0.1 against the same `6a265bc` build: **inconclusive at the
game cap**, +32.05 +/- 22.83, LLR 1.99 of 2.94, LOS 99.72%, 159W/113L/228D.

The prediction above was that a −14% tree is roughly 0.2 of a doubling and therefore worth single
figures of Elo, below what 500 games can resolve. The observed score is well above that, and the
LLR climbed steadily rather than stalling — the shape of an SPRT whose true effect sits at or above
the tested band, which is why the cap was reached with a positive lean rather than with nothing.
The 95% interval [+9.2, +54.9] excludes zero.

It is recorded as inconclusive regardless: a cap-stopped SPRT is a stopped test, not a verdict, and
the interval is far too wide to size the change from. PR 2 merges on the node reduction plus this
lean, not on a claimed Elo number. Resolving it would cost roughly another 1000 games (~80 min);
sizing it needs the fixed batch, which the plan defers to after PR 3 lands.

## Harvest

Filled incrementally as each PR lands; the file is deleted in PR 3 once complete.

| Decision / rationale | Lands in | Status |
|---|---|---|
| D1 — why quiescence passes `EmptyMove()` as hash move | comment on `order_quiescence_moves()` | **done** |
| D1 — evasion ordering goes through the shared scorer | `CLAUDE.md` → Key Source Facts | **done** |
| `SortMovesByValue` precondition | the assert itself, plus the `Sort.h` declaration comment | **done** |
| Glossary: capture / quiet move / king evasion / capture of the attacker / interposition | `CONTEXT.md` | **done** |
| #320's mechanism, with the 10000-vs-900 arithmetic | `Docs/Changelog.md`, and the test comment | **done** |
| PR 1 SPRT result (H1 accepted) plus the fixed-batch estimate | `Docs/EloLog.md`, `Docs/Changelog.md` | **done** |
| D2 — the tier order, and that SEE selects the tier while MVV-LVA scores within it | comment above `ScoreMoves`' tier chain | **done** |
| D2 — why losing captures stay above the quiets, and equal captures above the killers | same comment, plus the PR 2 body | **done** |
| D3 — why `see_ge` is boolean | comment on `see_ge`'s declaration | **done** |
| D5 — a king attacker terminates the swap | source comment in `See.cpp` | **done** |
| D10 — one tier policy for both `ScoreMoves` callers | `CLAUDE.md` → Key Source Facts (extends the #320 entry) | **done** |
| D11 — non-capturing promotions are tactical and bypass SEE | comment at the promotion branch in `ScoreMoves` | **done** |
| D11 — no promotion is prunable; the constraint on a future margin | #398's issue body | **done** |
| D5 — pins ignored, X-rays handled | source comment in `See.cpp` | **done** |
| D6 — `AttackersTo` takes an occupancy so SEE can mutate it | comment on the declaration | **done** |
| SEE test coverage | `Docs/TestDesign.md` | **done** |
| PR 2 `Gain` SPRT result, recorded as inconclusive | `Docs/EloLog.md`, `Docs/Changelog.md` | **done** |
| D8 — `!in_check` is correctness, not tuning | source comment at the pruning site | PR 3 |
| D9 — what `see_pruning_enabled` is for | comment on the field in `SearchTuning` | PR 3 |
| PR 3 `Gain` result and measured effect | `Docs/EloLog.md`, `Docs/Changelog.md` | PR 3 |

**Approved decisions that changed during implementation**

- **PR 1**: the ordering was extracted into `AIPerplex::order_quiescence_moves()` rather than left
  inline at the call site. Not in the design as approved. The reason is validation, not style: the
  regression test has to exercise the branch the node itself takes, or reverting the fix would
  leave it passing. With the helper, the falsification check fails exactly the ordering test and
  nothing else — verified by reverting the in-check branch and re-running.
- **PR 1**: `MoveHelper::Value()` caps a **king**'s LVA weight at a queen's. The design scoped PR 1
  to the in-check ordering path alone. Routing that path through `ScoreMoves` exposed a pre-existing
  defect and would otherwise have shipped it as a regression: at 10000 cp the king's LVA penalty
  scored `KxR` as -125, landing it in the *losing*-capture tier below every quiet evasion. The fix
  is at `Value()` rather than inside `ScoreMoves` because both sorters compute the same wrong number
  from the same helper.
  **Two earlier framings of this were wrong and are recorded because the second nearly shipped.**
  The first was to leave it: that ships a regression, since the old flat sort ranked `KxR` above the
  king walks whenever the king was the only piece that could move. The second was to drop the LVA
  term outright, on the reasoning that a legal king capture is unopposed and therefore never
  recaptured. That reasoning is true of a *legal* king capture but the premise fails at the call
  site: `ComputeLegalMoves` is pseudo-legal, so `Value()` also scores king captures `DoMove` will
  reject, and dropping the term ordered an illegal one ahead of every legal move. Capping keeps the
  king behind cheaper attackers without asserting a legality the function cannot see.
  Consequence for validation: PR 1 also changes `pvs()` ordering, so it is **not** node-identical
  with its base and `Compare-SearchEquivalence.ps1` does not apply to it.
- **PR 2: D2's tier order was replaced after measurement.** The approved three-tier order
  (SEE-equal below the killers, SEE-losing below the quiets) cost +56% nodes; the two-tier order
  that shipped measures −14%. D2 and D3 above are rewritten to the order actually implemented, and
  `PR 2 measurement` records the full matrix. The primitive itself — `see_ge`, `AttackersTo` and
  their tests — was unaffected and needed no rework. D11 is unchanged and measured ~neutral.
- **Sequencing changed**: PR 1 is now stacked on #401 rather than forked from `main`. Routing
  quiescence through `ScoreMoves` moves an illegal king capture from the back of the sorted list to
  the front, which — while `pvs()` reduced by *list index* — silently shifted every legal move's LMR
  reduction. Measuring PR 1 against `main` would therefore have measured an ordering change plus an
  accidental pruning change. #401 fixes the index defect first; PR 1's SPRT is taken against a build
  of #401 so it measures ordering alone. A partial run of the confounded bundle reached +43 Elo over
  210 games (LLR +1.48 of +2.94) before being stopped — recorded here as the reason the split was
  worth the extra measurement, not as a result.
