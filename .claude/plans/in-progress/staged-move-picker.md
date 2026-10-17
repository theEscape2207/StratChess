# Staged move generation in pvs() — Design

**Issue:** #839

## Goal

`pvs()` generates and scores every pseudo-legal move before its loop (`AIPerplex.cpp:868-885`).
The #839 spike found that 38% of out-of-check generating frames cut, or finish, before the loop
reaches any quiet move. Generating and scoring those quiets for nothing costs a gross **4.04% of
runtime** (ceiling A). A further **1.96%** goes to frames whose only quiets reached were killers
(ceiling B). Almost all of it is at depth 1 and 2: A is 3.37% at d1 and 0.43% at d2. The change
generates captures first, and generates quiets only when the loop reaches them. **The tree must
stay node-identical**, so the whole gain shows up as nps, measured by a paired bench.

## Review focus

- **D3's equivalence argument.** Quiet scores move from frame start to the moment the quiet stage
  is generated. The claim is that, at the frames D2 admits, no history, continuation-history or
  killer entry the quiet stage reads can change in between. It rests on which subtrees reach a
  `pvs()` frame of the same side, including through singular extension under a lowered
  `SingularMinDepth`. If the claim is wrong, the result is a silent node change, which the on/off test in Validation
  would catch only on its positions.
- **D2's population against the cost.** The net gain is unknown. Every frame that transitions pays
  for two generator calls and an extra scoring pass, and in PR 1 that includes killer-only frames
  (see Assumptions). The gross at d1-d2 is 3.80%. Is restricting to no-hash frames at depth ≤ 2 the
  right cut?
- **D6 (PR 2): the killer validator's oracle.** It is exhaustive over encodings, but only on the
  positions it is run on.

## Scope

**This change will:**

- add `MoveGenerator::ComputeQuiets`;
- add a scoring entry point for appended moves and name the good-capture tier in `MoveSorter`;
- stage generation in `pvs()` at frames D2 admits, behind a non-UCI tuning bool, default on;
- (PR 2, optional, D6) try validated killers as a stage before the quiets.

**This change will not:**

- change move order, any score, pruning, reduction or the node count, at any tuning value;
- touch frames with a hash move. Hash-first is #741, parked: its validator cannot pay for itself at
  1.33M hash-bearing frames;
- touch in-check frames (`ComputeEvasions`) or quiescence;
- add a quiet-check generator for frontier nodes (ceiling C, 0.6%, informational);
- change `ComputeLegalMoves`, so perft, UCI move parsing and the other callers see no change;
- interact with IIR (#96). IIR reduces no-TT-move nodes, usually at depth ≥ 3 or 4, and if it ships
  it moves more frames into this population. Neither design depends on the other.

## Decisions

### D1: Generate in two passes into one list

At a staged frame, `move_list` first receives `ComputeCaptures` (captures, en passant, all
promotions), which has `nc` entries. When the loop needs quiets, `ComputeQuiets` **appends** to the
same list, at `[nc, n)`. `scored_idx`, the `searched` bitset and `penalize_searched_quiets` keep
indexing one list in sorted order, so none of them changes.

Rejected: two lists with a merged cursor. The bitset and `penalize_searched_quiets` would need a
two-list index mapping. That adds code and risk for no saving.

`ComputeQuiets` emits pawn pushes (promotion rank excluded, as `GeneratePawnNormalMoves` already
does), then knight, bishop, rook, queen and king moves to empty squares, then castling. It reuses
the masked helpers with `targets = ~occupied`. This is exactly the non-capture, non-promotion
subsequence of `ComputeLegalMoves`, in the same order. `ComputeCaptures` is already that
subsequence for captures and promotions: pawn captures, promotions, then each officer type masked to
`them`, and masking keeps the order. Neither generator asserts an empty list.

### D2: Stage only no-hash, out-of-check frames at depth 1, and at depth 2 unless singular can reach depth 1

The predicate is a free helper, unit-tested like `frontier_futility_eligible`:

```
staged_generation_eligible(depth, in_check, hash_move, tuning) =
    tuning.staged_generation_enabled && !in_check && hash_move.is_null() &&
    (depth == 1 || (depth == 2 && tuning.singular_min_depth >= 2))
```

- **No hash move.** Frames with a hash move keep today's path. A hash-bearing frame would need the
  hash move searched before generation, which is #741's validator problem. The spike's A-class
  frames with a hash move number 5,142, about 0.01% of runtime. This also leaves singular
  verification untouched: it requires a hash move.
- **Exclusion frames have no hash move** (they skip the probe), so they are staged when the depth
  fits. Skipping the excluded move works by move equality in either stage.
- **Depth ≤ 2 is what makes D3 hold.** It keeps 3.80 of the 4.04 points. Depth 3+ would add 0.23
  points and lose the equivalence argument (D3).
- **The `singular_min_depth >= 2` term** keeps the argument true under every accepted tuning (D3).
  At the default of 6 it is always true.

Rejected: staging every no-hash frame and accepting fresher history at depth ≥ 3. That is a
behaviour change, so it would need an SPRT to sell 0.23 points of runtime.

### D3: Quiet scores are read when the quiet stage is generated; at D2's frames that equals frame start

Quiets are scored by `history[side]` plus continuation rows `cont_key[ply]` and `cont_key[ply-1]`,
read at the columns of the side to move's pieces. Killers come from `killers[ply]`. All of these
are written only at a `pvs()` cut (`AIPerplex.cpp:1182-1187`); quiescence writes none of them.

- `killers[ply]` is written by this frame's own cut, which ends the loop, and by a singular
  verification at this ply, which runs before the loop and needs a hash move. So it is constant
  while the loop runs.
- **A child of the opposite side** writes `history[!side]`, and continuation-history columns of the
  opponent's pieces. These are disjoint from what this frame reads: a column encodes the coloured
  moving piece.
- **A same-side `pvs()` frame below this one** is the only writer that matters. From depth 1, every
  child is at depth 0, which is quiescence. From depth 2, each child is at depth 1, and its children
  are at depth 0, through every path: the first move, LMR, re-search, PV re-search and null move
  (`1 - 1 - R < 1`). The one exception is a singular extension that keeps a depth-1 child's child at
  depth 1. That needs `singular_min_depth <= 1`, which D2 excludes.

So at every staged frame, the quiet stage computes exactly the scores today's frame computes before
its loop. The equal scores also mean LMR's `scored_idx[si].first` is unchanged, though LMR does not
run at depth ≤ 2 (`lmr_min_depth` 3).

`Threads > 1`: history is per thread and `cont_history` is a per-thread allocation, so the argument
holds per thread. SMP is nondeterministic anyway.

### D4: The transition is "the next entry is below the good-capture tier, or the captures are spent"

- Name the tier: `MoveSorter::kGoodCaptureTier = 1'000'000`, beside `kLosingCaptureTier`, and use
  it in `ScoreUnordered`.
- Before the loop: `ScoreMovesBestFirst(move_list, nc, …)` as today. The loop's `si == 1`
  `OrderRemaining(1, nc)` is unchanged.
- At the loop top, after that call and before any `continue`: if quiets are still pending and
  (`si == nc` or `scored_idx[si].first < kGoodCaptureTier`), append `ComputeQuiets`, score `[nc, n)`
  with a new `MoveSorter::ScoreAppended(moveList, first, n, …)`, and call
  `OrderRemaining(move_list, scored_idx, si, n)`. `ScoreUnordered` gains the `first` parameter that
  `ScoreAppended` exposes.
- The loop condition becomes `si < n || quiets_pending`, so a frame with no captures, or one whose
  captures are all illegal, still generates its quiets. Mate and stalemate detection is unchanged.

**Why this order is identical.** Today's comparator is a strict total order:
score descending, then generation index. Tiers are disjoint:

| Tier | Score | Origin |
|---|---|---|
| good captures, all promotions | ≥ 1,000,000 | captures pass |
| killer 0, killer 1 | 900,000 / 800,000 | quiets pass |
| losing captures | 700,000 + MVV-LVA | captures pass |
| quiets | < 700,000, asserted in `ThreadData` | quiets pass |

Every good capture outranks every entry the quiets pass adds, so the prefix before the transition
is today's prefix. After the transition, one sort over the losing captures and the quiets gives
today's suffix. Ties occur only within one tier, and so within one generator, whose relative order
equals `ComputeLegalMoves`' (D1). Profile builds' seeded tie-break hashes the move, not its index,
so it holds there too. A killer that is a non-capture promotion scores as a promotion in both paths.

Rejected: a transition at the first losing capture only. That would put losing captures above
killers.

### D5: A non-UCI tuning bool, default on

`staged_generation_enabled` is a `TUNING_FIELD(bool, …, true, …, nullptr, true)`. It costs one read
per frame, already loaded alongside the other tuning reads. It exists for the on/off test in Validation, which
compares the two paths inside one binary. A default-true runtime bool is also the project's end
state for a search feature (the alternative being deletion). Rejected: a compile-time gate. It
cannot be tested within one binary.

### D6 (PR 2, optional): validated killers as a stage

This decision takes effect only after PR 1 ships, with its bench result in hand. Gross gain: +1.89
points at d1-d2.

- When the transition fires and every non-null killer is neither a promotion nor a castle, each
  killer passing `MoveGenerator::IsPseudoLegalQuiet(board, move)` is appended and scored as today
  (900k/800k). It is ordered with `OrderRemaining(si, n)`, so it runs before the losing captures.
  Quiet generation is deferred again, to the first entry below `kLosingCaptureTier`, or to the end of
  the list.
- Otherwise the frame transitions as in PR 1. A promotion killer is already in the captures pass. A
  castling killer would need the attack-board test, which is not worth a validator branch.
- **Contract:** `IsPseudoLegalQuiet(board, m)` is true exactly when `m` ∈ `ComputeQuiets(board)`,
  comparing the full 16-bit encoding (flags included), for every `m` that is neither a castle nor a
  promotion. It is safe on any encoding.
- When the quiets are appended later, entries equal to an appended killer are dropped in place,
  keeping the order. The killer's unique score makes its index irrelevant.

The budget, from the spike: about 300 cycles per try is break-even over 1.25M tries. The validator
reads a few bitboards and one slider-attack lookup, so it fits. The bench decides whether PR 2
ships.

## Assumptions I cannot verify from the code

- **The net nps gain is positive.** The spike's 4.04% (3.80% at d1-d2) is a gross `rdtsc` ceiling,
  not an amount PR 1 is known to avoid. Not verified; settled only by the Speedup gate in
  Validation. Two cost terms the spike did not measure:
  - **Transition count.** In PR 1, every frame that reaches the killer tier pays the second pass,
    killer-only frames included. The spike's raw output for D2's population (no hash, out of
    check, d1-d2) gives 1,155,879 killer-only, 305,648 frontier-check and 557,246 needs-quiets
    frames: at least 2,018,773 transitions, exclusion frames not counted. The spike's 0.8% overhead
    figure covered 1.34M frames across all depths, so it is not a bound for PR 1.
  - **Losing-capture cuts.** The spike's no-quiet class includes frames that cut on a losing
    capture. D4 generates quiets before the first losing capture, to find any killers, so those
    frames pay the quiet stage without saving it. The spike does not split them out.
- **The spike's population is representative of game play.** It measured 8 bench positions at depth
  13. Not verified beyond the per-position spread (A from 2.5% to 5.5%). The bench is the ship gate,
  and game-like depth does not change which frames D2 admits.

Everything else, D3 included, is argued from the code above and closed by the tests in Validation.

## Invariants

1. Every search at `Threads=1` is node-identical to `origin/main`: every `info` line and every
   `bestmove`, at default tuning and with `SingularMinDepth=1`.
2. `ComputeCaptures(b) ++ ComputeQuiets(b)` equals the stable partition of `ComputeLegalMoves(b)` by
   capture-or-promotion, for every position.
3. A staged frame with no legal capture still searches its quiets, and adjudicates mate or
   stalemate correctly.
4. No new write sits above the abort guard. Staging writes only frame-local state.
5. (PR 2) `IsPseudoLegalQuiet` matches membership in `ComputeQuiets` for every non-castle,
   non-promotion encoding.

## Validation

Engine tier. Expected to be node-identical, so the gates are equivalence plus nps (`Docs/Workflow.md`
table: under ~5% nps and node-identical → `Compare-Bench`). **No Elo match:** an identical tree
cannot change strength except through nps, which the bench measures more precisely than any match.

- **Inv 2:** `[movegen]` test. Walk the 131 committed perft-suite positions to depth 2. At every
  position reached, out of check and in check, assert the partition equality, move by move.
  `Run-PerftCheck.ps1` is not run: no generator on perft's path changes. This test is the
  move-generation gate.
- **Inv 1, D3 and D4, proven by test:** a `[search]` test runs fixed-depth searches with
  `staged_generation_enabled` on and off. Positions: the 8 bench FENs, plus a promotion-heavy one,
  a castling one, an en-passant one and a stalemate-in-one. Depth: 7. Run under default tuning, and
  under `singular_min_depth = 1` with `singular_extensions_enabled`. Assert equal total nodes, best
  move and score at every iteration. Falsify it: score quiets at depth 3 as well, and watch it fail.
  If no position diverges, add positions until one does, or report that the test cannot see a D3
  violation.
- **Inv 1, cross-binary:** `Compare-SearchEquivalence.ps1 -After <worktree build> -BaselineRef
  origin/main` → IDENTICAL.
- **Inv 3:** covered by the stalemate-in-one and mate positions above.
- **Inv 4:** review. The diff adds no write before the guard.
- **Gain:** the speed-claim procedure in `measure-strength` → regression-check. Build both trees
  as clang-cl Release, relink them with `New-OrderedBuildPair.ps1` so code placement is equalised,
  then run one `Compare-Bench.ps1 -Control` series on that pair, `-Rounds 12` fixed before launch,
  under the quiet-window rule. **Ship only on the script's Speedup verdict.** That verdict needs a
  lower bound above zero against both baseline copies, and above the steady A/A offset when that
  offset is non-zero. The first predeclared series is the answer. Report every series tried, with
  its interval line and `metadata.json`. No Speedup, or a pair that could not be relinked, is
  reported as an unconfirmed gain and does not ship without the owner's call. A Speedup whose
  lower bound is under +1% also goes to the owner, with the cost, before merge.
- **PR 2 adds:** an exhaustive validator test. For each perft-suite position walked to depth 1, try
  all 65,536 encodings and assert Inv 5. It also runs under Debug and the sanitizers in CI, which
  covers "safe on any encoding". Then the same equivalence tests, and the same Speedup gate on an
  ordered pair of PR 1 and PR 2.

## Cost

- **Size, PR 1:** 50–200 lines across `MoveGenerator.{h,cpp}`, `Sort.{h,cpp}`, `AIPerplex.{h,cpp}`
  and `SearchTuning.def`, plus two test files (~150 lines).
- **Size, PR 2:** about 100 lines of validator plus 60 lines of tests.
- **Blast radius:** Engine tier. `Docs/EngineContracts.md` gains one bullet (D3/D4), and the Moves
  bullet names the new generator. No script, skill or CI changes.
- **Review:** one `search-reviewer` pass per PR, plus open-pull-request's code review (170–270k
  tokens, 3–5 min each).
- **Measurement:** one ordered-pair relink and one 12-round `-Control` series per PR, local. No CI
  lab.
- **Optional:** PR 2 as a whole, priced above, for a gross +1.9 points.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D3: why quiet scores read at stage time equal frame-start scores, and the depth and singular bound | `Docs/EngineContracts.md` → Search internals, next to the lazy-ordering bullet; short source comment on `staged_generation_eligible` |
| D1: `ComputeQuiets` emits the quiet subsequence of `ComputeLegalMoves` | `Docs/EngineContracts.md` → Moves (extend the emission-order bullet); `MoveGenerator.h` comment |
| D4: the transition point and the tier table | source comment at the transition in `pvs()`; `kGoodCaptureTier` comment |
| D2: no-hash-only because of #741 | source comment on `staged_generation_eligible` |
| D6: the killer validator contract (PR 2) | `MoveGenerator.h` comment on `IsPseudoLegalQuiet` |
| Measured bench gain | `Docs/Changelog.md`, PR body, `Measurements/` |
