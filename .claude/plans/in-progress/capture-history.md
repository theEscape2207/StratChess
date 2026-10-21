# Capture history — Design

**Issue:** #871 (child A4 of #636)

**v2.** v1 (`871 - capture-history v1.md`) added the table at full weight, an entry at its bound
worth about ten pawns of victim. Its lab was flat, -1.66 ±3.09 over 26,640 games (run
`39508612158`), on a flat screen, so v2 adds D7: a divisor on the history term, default 8. D1–D6
stand as built in `8908cbd`.

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

- **D7's divisor value.** 8 is chosen from the victim-class gaps, not tuned. The v1 lab measured
  divisor 1; v2's lab measures 8 and nothing in between.
- **D7's premise.** v1 being flat is read as "too much weight". The other reading, that the table
  learns too little to help at any weight, predicts v2 flat too, and then the PR closes.
- **D3's malus on quiet cuts.** Earlier captures are penalized when a quiet or killer cuts. The
  quiet-history rule does the opposite: captures do not penalize quiets. Check that the asymmetry
  is right.
- **The ceiling assumption** (see Assumptions): the gap between step 0's ceiling and what a learned
  table actually saves.

## Scope

**This change will:**

- add a per-thread capture-history table, updated at `pvs()` cutoffs;
- add it to the capture and promotion scores in `pvs()` ordering, inside the existing SEE tiers;
- add a runtime kill switch, `CaptureHistory`, that is node-identical to the merge base when off;
- divide the history term by `CaptureHistoryDivisor`, default 8, before it joins the score (v2).

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
- Dividing the history term by a tunable divisor: v1 had no evidence to tune against. v1's flat
  lab is that evidence, and D7 now takes this option.

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

### D7 (v2): Divisor `capture_history_divisor`, default 8, UCI `CaptureHistoryDivisor`

The capture score becomes `tier + HISTORY_MAX + 16 * mvv_lva + entry / divisor`. C++ division
truncates toward zero, which is monotone in the entry, so the table's order is kept and only its
weight against MVV-LVA changes. Learning is untouched: bonus, malus, gravity and aging still work on
the full-range entry.

**Why 8.** MVV-LVA is the victim's value minus the attacker's `value >> 4`: 6 for a pawn, 56 for a
queen. At a scale of 16, captures of adjacent victim classes (pawn 100, knight and bishop 300, rook
500, queen 900) sit 2,400 to 4,000 points apart once the attacker term is counted. QxN against PxP
is the narrowest. Captures two classes apart, and rook against queen, sit at least 5,600 apart. Two
entries at opposite bounds swing a pair of captures by `2 * 16,384 / divisor`:

| Divisor | Max swing | In pawns of victim | Can invert |
|---|---|---|---|
| 1 (v1) | 32,768 | ~20 | anything in a tier, PxQ against PxP included |
| 4 | 8,192 | ~5 | pawn against rook |
| **8** | **4,096** | **~2.6** | **adjacent classes only (pawn against a minor, a minor against a rook)** |
| 16 | 2,048 | ~1.3 | nothing across classes (2,048 < 2,400); reorders attackers and squares within a victim class |

8 lets a well-fed entry reorder a capture against the next victim class up or down, but never two
classes away. Step 0's late cuts were good captures cutting after a failed good capture. It did not
split those by victim class, so it does not say which row of the table they need. 8 is the middle
row that still crosses class boundaries. 16 is the fallback arm if 8 is flat and the owner wants
one more run (Validation).

**Shape.**
- `SearchTuning.def`: `TUNING_FIELD(int, capture_history_divisor, 8, 1, 16384, true,
  "CaptureHistoryDivisor", true)`, next to `capture_history_enabled`. The lower bound of 1 rules out
  division by zero. At 16,384 the quotient is in [-1, 1], so the term is nearly off. It follows
  the `LmrHistoryDivisor` precedent, a runtime division in the hot path.
- `MoveScoringContext` gains `int capture_history_divisor = 1`, read only when `capture_history`
  is non-null.
- `ThreadData::scoring_context(ply, cont, use_capture_history)` keeps its bool and gains a fourth
  parameter, `int capture_history_divisor`, with no default, for D5's reason. `pvs()` passes
  `tuning_.capture_history_divisor`. In-check quiescence passes `1`, which nothing reads, because
  its table pointer is null.
- The tier-span `static_assert` is unchanged: a divisor of at least 1 only shrinks the term.

Rejected:
- **A shift instead of a divisor.** It saves the integer division on each scored capture. But it
  departs from the `LmrHistoryDivisor` precedent and allows only powers of two. One 32-bit `idiv`
  per scored capture is about 10 to 20 cycles, a few per node, so the cost is expected below bench
  noise (#555: layout alone moved nps 3.9%). The bench pass catches only a gross cost. The lab,
  run on the shipping binary, decides.
- **Scaling MVV-LVA up instead.** A larger `kCaptureMvvLvaScale` gives the same order, but at 8 × 16
  the tier span would pass the 100,000 gap to the next tier.
- **Default 1, with the lab setting the option.** The default would then need a second change once
  the lab passed. Shipping 8 as the default makes the lab measure the binary as it would merge, and
  needs no proof that an option reached the engine.
- **Sweeping several divisors in the lab.** Each arm costs about 4 hours. A screen cannot pick
  between them: #664's flat screen gained +6.9 Elo in the lab.

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

- **(v2) v1 was flat because the term was too heavy.** Nothing in v1's measurements separates that
  from a table that learns too little. The screen moved captures' share of late cuts only from
  28.6% to 27.4%, which fits either reading. The practice D2 cites leans against the premise: the
  Stockfish recollection above puts an entry at its bound near 7 pawns, close to v1's 10, so divisor
  8 is about a sixth of that weight. "Learns too little" (D3's update rule, the bonus size, aging
  every iteration) is at least as likely. search-reviewer's prior for v2 is 0 to +3 Elo, direction
  uncertain. The v2 lab is the test (Validation, v2).

## Invariants

- The tier order is unchanged. Every capture or promotion score lies in `[tier, tier + 100,000)`,
  enforced by a `static_assert` on `2 * HISTORY_MAX + kCaptureMvvLvaScale * kMaxMvvLva`. A debug
  `assert` checks `mvv_lva >= 0` and `mvv_lva <= kMaxMvvLva` at scoring.
- `|entry| <= HISTORY_MAX`, through `apply_history()`.
- `CaptureHistory=false` is node-identical to the merge base at `Threads=1`.
- (v2) `CaptureHistoryDivisor=1` is node-identical to v1, `8908cbd`, at `Threads=1`.
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

### v2 (D7)

- **Equivalence at divisor 1.** A throwaway build with the default set to 1, run through
  `Compare-SearchEquivalence.ps1 -BaselineRef 8908cbd`, must give IDENTICAL. The off-switch check
  above is re-run against the merge base: the divisor must not leak into the off path.
- **`[search]` tests, each one falsified by a mutation:**
  - with entries at opposite bounds, PxP outranks PxR in the same tier at divisor 1 and does not
    at divisor 8 (the gap of at least 5,600 is wider than 8's 4,096 swing);
  - with PxN's entry at the upper bound and NxR's at the lower, PxN outranks NxR at divisor 8, so 8
    still crosses one class boundary;
  - `Validate()` rejects a divisor of 0.
- **Cost.** `Run-Bench.ps1` ×3 alternating against `8908cbd`, for the division per scored capture.
- **Screen.** `Compare-SearchProfile.ps1 -Seeds 8 -Depth 12` against the merge base, as in v1. It is
  informational and gates nothing.
- **Lab.** About 26k games at 10+0.1 against the merge base, with no options. **Ship rule, declared
  now:** the PR merges if Elo minus its error bar is above 0. Otherwise:
  - if v2's point estimate beats v1's -1.66 by at least one standard error (about 1.6 Elo at 26k
    games), the trend points at weight, and one more arm at divisor 16 is the owner's call. A second
    arm roughly doubles the false-pass rate, from about 2.5% to about 5%; that is accepted.
  - if not, the PR closes. Weight is then not the lever, and the next idea is D3's update rule or
    the aging, as a separate issue.

## Cost

- **Size:** 50–200 lines in the engine, plus tests. Files: `Sort.h`, `Sort.cpp`, `ThreadData.h`,
  `AIPerplex.cpp`, `SearchTuning.def`, a test file, the `ScoreMoves` bullet in
  `Docs/EngineContracts.md`, and `Docs/Changelog.md`.
- **Blast radius:** Engine tier. It changes the `ScoreMoves` score formula, which the
  `EngineContracts` capture-tier bullet and the `Sort.cpp` comment describe.
- **Review:** `search-reviewer` on this document and on the diff, plus the code review.
- **Measurement:** the screen takes about 25 minutes, the wall clock about 15 minutes, and the lab
  3 to 3.5 hours. The lab is the owner's call.
- **v2:** under 20 lines in `SearchTuning.def`, `Sort.h`, `Sort.cpp`, `ThreadData.h`,
  `AIPerplex.cpp`, plus tests. Bench and screen take about 45 minutes, and the lab about 4 hours.
  `search-reviewer` reviews D7 and the diff.
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
| D7 divisor, its value and the class-gap table's conclusion | comment on its `SearchTuning.def` entry; the "about ten pawns" line in `ScoreUnordered()` restated for the divisor, with the attacker-term gaps |
| Score formula inside the tiers | `Docs/EngineContracts.md`, the `ScoreMoves` bullet |
| Step 0, screen, wall clock and lab results | `Measurements/ci-per-change.md`, with a dated `Docs/Changelog.md` entry |
