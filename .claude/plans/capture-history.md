# Capture history — Design

**Issue:** #871 (child A4 of #636)

## Goal

Captures are ordered with no learning at all. `MoveSorter::ScoreUnordered()` uses SEE to choose the
tier and static MVV-LVA to rank inside it, while quiets learn from butterfly and continuation
history. Step 0 (#871) ran at depth 12 over the 200 positions of `Tests/profile-screen.fen`.
Captures made 28.5% of late cuts. Of those, 93% came after an earlier capture had been searched and
failed, and 89% were a good capture cutting after a failed good capture: the ordering inside the
tier got them wrong. The nodes spent before those cuts are 11.8% of all late-cut work, about 4.5% of
total nodes. That is a loose ceiling, not an expected saving. The span counted runs from the start
of the move loop, so it includes the subtree of a hash move that failed, which capture history
cannot reorder. Learning which captures actually cut, per
moving piece, target square and victim, can win back part of it.

## Review focus

- **D2's weight.** A history entry at its bound is worth about ten pawns of victim value, so a
  well-fed entry overturns MVV-LVA inside a tier. This value is a judgement, not a tuned constant,
  and the lab measures only this one value.
- **D3's malus on quiet cuts.** Earlier captures are penalized when a quiet or killer cuts. The
  quiet-history rule does the opposite: captures do not penalize quiets. Check that the asymmetry
  is right.
- **The ceiling assumption** (see Assumptions): the gap between step 0's ceiling and what a learned
  table actually saves.

## Scope

**This change will:**

- add a per-thread capture-history table, updated at `pvs()` cutoffs;
- add it to the capture and promotion scores in `pvs()` ordering, inside the existing SEE tiers;
- add a runtime kill switch, `CaptureHistory`, that is node-identical to the merge base when off.

**This change will not:**

- read capture history in quiescence, in-check evasions included;
- use it to move the SEE good/losing threshold, or in any pruning or reduction decision;
- change the tier constants or the order of the tiers;
- replace MVV-LVA as the base inside a tier (D2's rejected option).

Each of these is a later arm or a separate issue, as #871 says.

## Decisions

### D1: Table `capture_history[kPieceSquares][8]` of `int16_t`, inline in `ThreadData`

The row is `continuation_index(board.GetEffectiveMovPiece(mv), mv.to())`. The column is
`board.GetCapturedPiece(mv) >> 1`: 0 to 5 for pawn to king, and 7 for `NO_PIECE`, which is a
promotion without a capture. Column 6 is never written. For a promotion, the effective piece is the
promoted piece, so a queen promotion and an under-promotion keep separate entries. The table is
12 KB, so it stays inline next to `history` in the hot section. It does not need to go on the heap
the way the 1.18 MB `cont_history` did.

Rejected:
- `[side][from][to][victim]`: 64 times larger, and the colour is already encoded in the piece.
- Leaving promotions out: the good tier mixes them with captures, and step 0 counted both.
- Packing the column to 7 entries: that saves 1.5 KB and costs a remap.

### D2: Score = `tier + HISTORY_MAX + kCaptureMvvLvaScale * mvv_lva + capture_history`, scale 16

This applies to both tiers, with the same expression in each.

- **Offset.** `HISTORY_MAX` keeps every score in a tier at or above that tier's constant. The
  staged-generation trigger (`scored_idx[si].first < kGoodCaptureTier`) depends on that.
- **Scale.** At 16, one pawn of victim value is 1,600 points. An entry at its bound, 16,384 points,
  is worth about ten pawns. Gravity does not keep frequent entries small. A capture that keeps
  cutting moves toward ±`HISTORY_MAX`, whatever the bonus. With only 6,144 cells, the common
  (piece, to, victim) triples get close to that bound even after halving. So a well-fed table can,
  by design, invert even PxQ against PxP inside the good tier (850 × 16 = 13,600, inside the
  32,768 spread). MVV-LVA still orders the rare triples.
- **When off**, the history term is 0 and the score is an increasing affine function of
  `mvv_lva`. That preserves the order, ties included, so the tree is node-identical.
- **Range.** `mvv_lva` is never negative in either tier: the smallest victim is a pawn at 100, and
  the largest attacker term is the queen's 900 >> 4 = 56. The maximum is a queen promotion that
  captures a queen, below 1,800. So a tier spans less than 2 × 16,384 + 16 × 1,800, about 61,600.
  That is under the 100,000 gap to the next tier.

Rejected:
- Replacing MVV-LVA with `victim value × k + history`, as Stockfish does. That changes two things
  at once, the base order and the learning, and one lab run could not attribute the result.
  It is a later arm.
- Dividing the history term by a tunable divisor: there is no evidence yet to tune against.

### D3: Updates at every `pvs()` cutoff, at the existing site after the abort guard

- **Bonus.** A capture or promotion that cuts gets `+history_bonus(depth)`, including the hash
  move.
- **Malus.** At a cutoff by any move, every capture or promotion searched before it gets
  `-history_bonus(depth)`. "Searched" means it has its bit in `searched`, which includes a hash
  move that failed. The cutting move can be a capture, a killer or a quiet.
- The update is the same gravity `apply_history()` that A1 uses.
- It runs in the same frames as the quiet updates, singular exclusion frames included.

Penalizing on a quiet cut follows Stockfish's practice, which I have not verified (see
Assumptions). The reason holds on its own terms: the malus is relative, so it changes only how a
capture ranks against other captures. A capture that failed has failed, whatever cut later.

Rejected:
- Malus only on capture cuts, mirroring the quiet rule. It would drop the evidence from every node
  where all captures failed and a quiet cut. Those are the nodes where demoting a capture that
  keeps failing still changes the move number of what follows.
- No malus at all. A1 measured +13.2 Elo from adding malus to quiet history (#651).

### D4: Lifecycle like the butterfly table

- Cleared in the constructor and in `reset_for_new_game()`.
- Halved in `age_history()`, before every iteration.

Halving `cont_history` only once per search was a choice made for a 1.18 MB table. It does not
apply to 12 KB. Rejected: never aging the table, which would let early-game entries dominate.

### D5: Read by `pvs()` only, through `MoveScoringContext`

The context gets `const int16_t (*capture_history)[8]`. A null pointer means no history term.

- `ThreadData` cannot see `tuning_`, so `scoring_context(ply, cont, bool capture_history)` takes
  an explicit third parameter with no default. It passes the table when the parameter is true and
  null otherwise. Having no default forces every caller to choose.
- `pvs()` (its `scoring_context` call before `ScoreMovesBestFirst`) passes
  `tuning_.capture_history_enabled`.
- In-check quiescence (its `scoring_context(ply, {})` call) passes `false`, so quiescence keeps its
  exact current order. Its scores move only by the offset and scale, which a monotone change leaves
  invisible.

The staged frames read capture scores at frame start, as they do today. `ScoreAppended` scores only
the quiets. So the staged-frame timing contract (`Docs/EngineContracts.md`) gains no new reader.

### D6: Kill switch `capture_history_enabled`, default true, UCI `CaptureHistory`

This is a `bool` entry in `SearchTuning.def`. When it is false, nothing reads the table and nothing
writes it. It is exposed over UCI, unlike `continuation_history_plies`, so a lab or local match can
turn it off on one binary.

The PR ships with the default `true`, because it is the lab vehicle. If the lab is negative, the PR
closes. Per the standing rule, the end state is either a default-true switch or deletion.

## Assumptions I cannot verify from the code

- **Stockfish weights a captured pawn at about 7 × 208 points against a capture-history bound of
  about 10.7k, and penalizes captures on quiet cuts.** This is from memory of its source and has
  not been checked. It supports D2's scale and D3's rule as plausible. It does not support any
  expected Elo. Verifying would mean reading a current `search.cpp` and `movepick.cpp`. Nothing
  depends on it: the lab decides either way.
- **The ceiling turns into saved nodes.** Step 0 bounds what perfect ordering inside a tier could
  save, and it overstates even that: the span includes a failed hash move's subtree, usually the
  largest. search-reviewer estimates a realistic saving of 1–2% of nodes. Nothing gates on the
  number, so it was not re-probed. It does not bound what a learned table achieves. The screen's late-cut work and the lab
  measure the real effect. The step-0 run has one tree and no seeds. It is spread across positions,
  not carried by a few: the reachable share per position has a mean of 10.0%, an SD of 6.6% and a
  maximum of 59.6%.

## Invariants

- The tier order is unchanged. Every capture or promotion score lies in `[tier, tier + 100,000)`,
  enforced by a `static_assert` on `2 * HISTORY_MAX + kCaptureMvvLvaScale * kMaxMvvLva`. A debug
  `assert` checks `mvv_lva >= 0` and `mvv_lva <= kMaxMvvLva` at scoring.
- `|entry| <= HISTORY_MAX`, through `apply_history()`.
- `CaptureHistory=false` is node-identical to the merge base at `Threads=1`.
- No capture-history write is reachable from an aborted child. The updates sit at the existing
  cutoff site, below the per-move `IsAborted()` guard.
- Each thread owns its table. Nothing is shared under Lazy SMP, and `Threads=1` stays
  deterministic.
- Staged and unstaged generation still search the same tree (`SearchStagedGenerationTests.cpp`).
- Quiescence ordering is unchanged.

## Validation

This is the Engine tier: a search change. The CI strength lab decides; its budget is the owner's
call. Nothing before the lab gates it (#636 Method).

- **Equivalence when off.** A throwaway build with the default set to `false`, run through
  `Compare-SearchEquivalence.ps1 -BaselineRef <merge base>`, must give IDENTICAL. The script
  passes no UCI options, so the default flip is how to test it, as #664 did.
- **`[search]` tests, each one falsified by a mutation:**
  - a capture cut raises its entry;
  - captures searched before a cut take the malus, on a capture cut and on a quiet cut;
  - a well-fed entry reorders captures inside a tier but never across tiers;
  - off writes nothing;
  - with the switch on and a fed table, in-check quiescence orders exactly as it does with the
    table empty;
  - the on/off staged-generation test passes with capture history on.
- **Screen.** `Compare-SearchProfile.ps1 -Seeds 8 -Depth 12 -Positions Tests/profile-screen.fen`
  against the merge base: late-cut work and nodes, with ±2 SE. A local cherry-pick of the step-0
  probe commit (`9b8599ec`) re-splits the capture late cuts. That build stays local and is not
  merged.
- **Cost.** A `Run-Bench.ps1` nps pass for the per-capture table read. Sorting is about 15% of CPU
  (#719).
- **Wall clock.** `Compare-WallClock.ps1` at depth 14 over the 200 positions, against the merge
  base.
- **Lab.** About 20k games at 10+0.1 against the merge base, 3 to 3.5 hours. For calibration, #664
  showed a flat screen gaining +6.9 Elo in the lab.

## Cost

- **Size:** 50–200 lines in the engine, plus tests. Files: `Sort.h`, `Sort.cpp`, `ThreadData.h`,
  `AIPerplex.cpp`, `SearchTuning.def`, a test file, the `ScoreMoves` bullet in
  `Docs/EngineContracts.md`, and `Docs/Changelog.md`.
- **Blast radius:** Engine tier. It changes the `ScoreMoves` score formula, which the
  `EngineContracts` capture-tier bullet and the `Sort.cpp` comment describe.
- **Review:** `search-reviewer` on this document and on the diff, plus the code review.
- **Measurement:** the screen takes about 25 minutes, the wall clock about 15 minutes, and the lab
  3 to 3.5 hours. The lab is the owner's call.
- **Optional part, the probe re-split on the screen:** a local cherry-pick and one extra screen run
  of about 25 minutes. It covers the risk that the gain, or the loss, comes from somewhere other
  than the cuts step 0 targeted. Drop it if the screen and the lab are enough.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 index, D4 lifecycle, the off-means-no-read-or-write rule | comment on `ThreadData::capture_history` |
| D2 offset and scale, and why the offset protects the staged trigger | comment in `ScoreUnordered()`, next to the existing tier comment |
| D3 malus on any cutoff, and its asymmetry with the quiet rule | comment at the cutoff site in `pvs()` |
| D6 kill switch | comment on its `SearchTuning.def` entry |
| Score formula inside the tiers | `Docs/EngineContracts.md`, the `ScoreMoves` bullet |
| Step 0, screen, wall clock and lab results | `Measurements/ci-per-change.md`, with a dated `Docs/Changelog.md` entry |
