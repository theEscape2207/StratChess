# King Safety Evaluation — Design

**Issue:** #97 · **Epic:** #110 Tier 2 · Replaces the sketch in `not-started/king-safety-evaluation.md`

## Goal

The evaluator scores the king with a rank-only middlegame PST and `eval_castling`, a rights-plus-file
proxy. Neither knows whether the pawns around the king are still there, whether the files beside it
are open, or whether anything is attacking it. Two positions that differ only in a shattered
kingside score identically, so the engine trades into and pushes pawns in front of its own king
without a static reason not to. This adds a real king-safety term — shelter/storm, king-file
openness, and attack pressure — tapered to nothing in the endgame, and resolves the overlap with the
two existing proxies by measurement rather than by assumption.

## Scope

**This change will:**

- Move non-pawn piece attack generation into `BuildContext` and keep compact aggregates in
  `EvalContext` (one all-attacks union per color, per-type mobility counts, per-type king-zone
  attacker counts, connected-rook pairs), so mobility, `eval_rooks` and king safety all read one
  generation pass.
- Add four per-color contributions — `king_shelter`, `king_storm`, `king_files`, `king_attack` —
  each with its own `EvalBreakdown` row.
- Normalize the king zone so a king on an edge file does not look safer for having a smaller ring.
- Cap the non-linear attack contribution explicitly.
- Ablate `eval_castling` and the middlegame king PST **after** the term is measured, and keep
  whichever combination measures best.

**This change will not:**

- Add a 64-entry per-square attack board or any per-node heap allocation.
- Change what any existing term contributes to a **searched** score. PR 1 is behaviour-preserving
  for `Evaluate()` and gated on exact equivalence. The one exception is deliberate and is not a
  scored path: on a dead-drawn material class the `Breakdown()` mobility and rooks rows now read 0
  (D2), for a position `Evaluate()` scores `Draw` without consulting either term.
- Add a king-tropism (pure distance) bonus. Concrete attacks first; distance only if an ablation
  asks for it.
- Re-generate slider attacks with the king removed from occupancy to get a legal flight-square
  count, or generate safe checks. The first version accepts the pseudo-safe approximation (D6).
- Add pawn-hash caching (the `not-started/pawn-hash-table.md` sketch) — shelter is recomputed per
  node like every other pawn term today. If nps says otherwise, that is a separate change.
- Delete `eval_castling` or the king PST speculatively. PR 4 decides that on evidence.
- Retune the tables. #117 (Texel) owns weights; the values shipped here are literature-standard
  starting points, deliberately not hand-fitted.

## Decisions

### D1: Attack data lives in `EvalContext` as compact aggregates, not as attack boards

`BuildContext` runs one loop per piece type per color and accumulates:

```
BITBOARD attacks_all[NUM_COLORS];        // every piece's attacks, pawns and king included
int      mobility_count[NUM_COLORS][4];  // sum of (popcount(att & usable) - BASE), by type
int      zone_attackers[NUM_COLORS][4];  // pieces of that type attacking the ENEMY king zone
int      zone_attacks[NUM_COLORS];       // total zone squares attacked, counting overlaps
int      connected_rook_pairs[NUM_COLORS];
BITBOARD king_zone[NUM_COLORS];          // normalized, see D3
```

They arrive with their consumers, not all at once. **PR 1 adds only
`mobility_count` and `connected_rook_pairs`** — the two that have a reader the
moment they exist — as a `PieceAggregates` struct held by `EvalContext`. The
remaining four are computed in the same loop when PR 3 introduces the term that
reads them, so their per-node cost is measured against the signal that pays for
it rather than banked in a refactor that cannot show a benefit.

`PieceAggregates` is returned by value from a function taking the bitboards it
reads, rather than filled through an `EvalContext&`. That is not style: writing
through the reference measured about 0.4% nps slower, because the optimizer
cannot then prove the stores do not alias the bitboards being read back through
the same context.

**Per-type attack unions are deliberately absent.** The issue text asks for them, but no consumer
in this design reads one: mobility counts, zone-attacker counts and connected-rook pairs are all
accumulated inside the generation loop, where the per-piece attack board is still in a register, and
the only union anything reads back is `attacks_all` (for pseudo-safe king moves, D6). Storing six
more bitboards per color per node buys nothing. If a later term needs them, they are one line to add.

The `[4]` arrays are indexed by a **dense, eval-local** knight/bishop/rook/queen index, not by
`ePieceType` — that enum is sparse (`PAWN = 0, KNIGHT = 2, … KING = 10`, `defines.h`) and has no
count constant, so indexing by it would silently need an 11-entry array.

Rejected: keeping generation in `eval_mobility` and letting king safety regenerate (doubles the
hot work, which is the reason #238 was not fixed standalone); and storing a per-square attacker
count board (`int[64]` per color per node, for a signal three counters carry).

Weights stay out of the context: it holds counts, the term functions turn counts into `ScorePair`s.
That is what keeps `Evaluate()` `const` and the terms pure (mobility design D7), and it is what
lets #117 retune without touching `BuildContext`.

`mobility_count` stores the base-subtracted sum per type, which is *exactly* what `eval_mobility`
multiplies by its per-type weight today — so the refactor is an identity, not an approximation.
`connected_rook_pairs` is counted in the same rook loop, which is where the duplicated
`RookAttacks()` call in `eval_rooks` disappears.

### D2: Attack generation is skipped when `endgame_scale == 0`

`Evaluate()` already returns `GameValues::Draw` without computing any term for a dead-drawn material
class. Generating attacks unconditionally in `BuildContext` would reintroduce that cost on exactly
the path the early-out exists for. `BuildContext` therefore computes `endgame_scale` first and
leaves the attack aggregates zeroed when it is 0.

This is visible only through `Breakdown()`, where the mobility and king-safety rows then read 0 for
a position whose total is `Draw` anyway. The reconstruction invariant still holds: every row and the
`endgame_adjustment` figure derive from the same context, so the rows plus the adjustment still sum
to `total`. Rejected alternative: a second "full" build for `Breakdown()`, which would make the
debug path compute something the search never did.

### D3: The king zone is a clamped 3×3 block plus one forward rank — exact definition

Board layout is `defines.h`'s: square 0 = a8, 63 = h1, `Rank()` is the row index (0 = rank 8, 7 =
rank 1), and forward for White is *decreasing* row.

```
kf     = clamp(File(king_sq), 1, 6)          // b..g
kr     = clamp(Rank(king_sq), 1, 6)          // never the outermost row
anchor = kr * 8 + kf
zone   = g_bbKingMoves[anchor] | bit(anchor)             // the 3x3 block, 9 squares
zone  |= shift_one_rank_forward(that 3x3 block)          // 3 more, clipped off-board
```

`g_bbKingMoves` is the 8-square ring **without** its centre, hence the explicit `| bit(anchor)`.
Both clamps are load-bearing: the file clamp is what makes the zone constant-width, and the rank
clamp is what keeps the 3×3 block wholly on the board so the block is always exactly 9 squares.

The zone is **12 squares everywhere except** when the forward rank falls off the board — for White
that is `kr == 1`, i.e. a king on rank 8 or rank 7, where it is 9. That exception is stated rather
than clamped away: a king on the enemy back rank has nothing in front of it, and inventing squares
for it would be worse than a smaller zone.

Worked examples (White; the Black cases are these mirrored by `^ 56`, which the symmetry test
asserts):

| King | anchor | zone |
|---|---|---|
| `Kg1` | g2 | f1–h1, f2–h2, f3–h3, f4–h4 |
| `Kh1` | g2 | identical to `Kg1` — the point of the file clamp |
| `Kg2` | g2 | identical to `Kg1` |
| `Ke4` | e4 | d3–f3, d4–f4, d5–f5, d6–f6 |
| `Kg8` | g7 | f8–h8, f7–h7, f6–h6, f5–h5 (Black: forward is the other way) |

Without the file clamp, a king stepping g1→h1 loses three ring squares, every zone count drops, and
retreating into the corner reads as *safer*. Stockfish hit that discontinuity and fixed it by
extending the ring horizontally so it stays a constant eight squares
([official-stockfish/Stockfish#1512](https://github.com/official-stockfish/Stockfish/pull/1512)) —
the citation is for the discontinuity and the clamp-style remedy, not for the 12-square shape here,
which is ours and is verified by the enumerated masks above rather than by their result.

Rejected: king ring only (the discontinuity above, and it misses the rank attackers actually arrive
on); a per-square generated zone table (no new table is needed — this is two operations on an
existing one).

### D4: Shelter and storm are indexed by the pawn's own relative rank, not by its distance from the king

One scan over the three files `kf - 1, kf, kf + 1` (with `kf` the clamped file from D3, so all three
are always on the board) produces both signals. All ranks below are **defender-relative**: rank 1 is
the scanning side's back rank, rank 8 the enemy's, so the tables are written once and used by both
colors.

For each file, considering only squares **ahead of or level with the king** (own pawns behind the
king are no shelter; enemy pawns behind it are not storming it):

- the nearest **own** pawn gives `r ∈ [2, 7]`, indexing `SHELTER[d][r]`;
- the nearest **enemy** pawn gives `r' ∈ [2, 7]`, indexing `STORM[d][r']`;
- `d = |file - kf| ∈ {0, 1}` — three files means the king's own and one either side, so there is no
  distance 2 and the tables have two rows, not three. (The draft said `{0, 1, 2}`, which does not
  follow from the three-file scan it describes.) **Index 0 means "no such pawn on this file"** in
  both tables, which is unambiguous because no pawn can ever stand on rank 1 or rank 8.
- A storm pawn is **blocked** when the nearest own pawn on the same file sits at exactly `r' - 1` —
  directly in the storming pawn's path. Its `STORM` value is halved (integer division) in that case.

Rejected: indexing by *rank distance from the king*, which was the earlier sketch. Two defects, both
fatal. A pawn level with the king has distance 0, which collides with the "no pawn" sentinel; and
"a pawn on its start square shelters best" stops being expressible once the king steps off its own
back rank, because the same shield then indexes a different bucket. Relative rank has neither
problem: rank 2 is simply the best `SHELTER` entry, whatever the king does.

Storm was included on the project owner's call: shelter alone is blind to opposite-side-castling
attacks, which is a large share of what this term exists to see. The cost is one more table to
mis-tune, contained by keeping both mg-only and bounded (D7). The two share this one scan but are
**separate contributions with separate breakdown rows** (D8), so an over-weighted storm table cannot
hide inside a shelter result.

Table values are literature-standard shapes, not fitted here — #117 owns them, and the doc comment
above them says so rather than implying a provenance they do not have.

"Nearest ahead" needs a most-significant-bit scan for White (forward is decreasing square index),
which the codebase does not have — `Board::GetFirstPiece` is lsb-only. Add a `Bits`-level
`GetLastPiece`/`countl_zero` helper alongside it rather than open-coding a reverse scan in `Eval.cpp`.

### D5: King-file openness uses whole-file, pawn-only definitions, stated here rather than inherited

A king file is **half-open** when our side has no pawn anywhere on it, and **open** when neither side
does. Pawns only, consistent with #126.

This deliberately does *not* inherit `eval_rooks`' forward-span-for-own-pawns / whole-file-for-enemy
asymmetry, which that term documents as inherited-not-principled. A pawn *behind* the king is no
shelter, so "ahead only" is arguably right — but it is also what the shelter table already measures,
and the thing this sub-term exists to price is a lane an enemy rook can use, which is a whole-file
property. Penalty is larger for the king's own file than for the two adjacent ones.

The overlap with D4 is real and acknowledged: "no own pawn on this file" scores in both places.
It is kept as a separate, separately-ablatable sub-term precisely so PR 4 can measure whether it
earns its place on top of shelter.

### D6: Attack pressure is quadratic in a weighted attacker count, with no attacker-count gate

```
danger    = Σ_type ATTACK_WEIGHT[type] * zone_attackers[enemy][type]
          + ZONE_SQUARE_WEIGHT * zone_attacks[enemy]
          + FLIGHT_WEIGHT * (FLIGHT_BASE - pseudo_safe_king_moves)
clamped   = clamp(danger, 0, KING_DANGER_MAX)
penalty   = min(KING_DANGER_CAP, clamped * clamped / KING_DANGER_DIVISOR)
```

**The clamp to zero is load-bearing, and not for overflow.** `FLIGHT_BASE -
pseudo_safe_king_moves` goes negative for a king with more flight squares than the base, which is a
safety *bonus*; squaring it would turn that straight back into a penalty and make the curve
non-monotone at the safe end. Clamping first is what makes `penalty` a monotone non-decreasing
function of `danger`. `KING_DANGER_MAX` additionally bounds the multiplication, which with sane
weights is unreachable in `int` — it is there so the bound is a property of the code rather than of
the current table values.

`pseudo_safe_king_moves` = king ring squares neither occupied by our own pieces nor in
`attacks_all[enemy]` — the only reader of `attacks_all`, and the only use of the enemy king's
attacks, which is otherwise not a weighted attacker. **It is pseudo-safe, not safe**: the shared
slider attacks are generated against the current occupancy, which still contains our own king, so a
square *behind* the king along a checking ray reads as unattacked. The error is one-directional — it
under-states danger, never over-states it — and the alternative is a second unconditional sliding
pass with the king removed, at a cost the signal does not justify. The name carries the caveat so no
later reader mistakes this for a legal king-move count.

Quadratic-with-cap rather than a hand-written danger table: it is non-linear by construction (two
attackers cost four times one, which is the property the term exists for), monotone after the clamp,
and has three tunables instead of a hundred. A table's extra shape is not something we can currently
fit — #117 can replace the formula with one later if the data supports it.

Rejected: a hard "fewer than two attackers scores zero" gate, which the earlier draft justified on
nps. That justification does not survive: by the time the gate could fire, generation, zone
intersection and aggregation have all already happened, so it saves one multiply and one divide.
What it costs is real — a discontinuity at the threshold, and zero danger for a lone queen sitting
next to the king controlling half its flight squares. The quadratic already makes a single small
attacker nearly free, which is the effect the gate was reaching for. If a later measurement argues
for a gate, it should key on queen presence rather than a raw attacker count, and state explicitly
whether pawns count.

### D7: Middlegame-only, and bounded by construction

All four contributions have `eg = 0` and are blended by the existing phase, so they fade continuously
to nothing as pieces come off — the reason #99 was a hard prerequisite.

Each is bounded, and the bound is **absolute and per color**, not an average: shelter and storm by
their table extrema times three files, king files by its two penalties times three files, attack
pressure by `KING_DANGER_CAP`. `KING_SAFETY_MAX_PENALTY` is their sum, declared in `Eval.h` beside
the constants and tied to the tables by `static_assert` over the table extrema — so a later retune
that pushes past the declared bound fails to compile rather than quietly outweighing a piece. A
runtime test asserts the same bound over the eval corpus, which is what catches a bound that is
correct arithmetically and wrong about which entries are reachable.

### D8: Four separately-attributable contributions and four breakdown rows

`king_shelter`, `king_storm`, `king_files`, `king_attack` — four `EvalBreakdown` rows. The first
three come out of ONE pawn scan over the king's three files, produced by one function
(`eval_king_pawn_cover`) returning three `ScorePair`s; what matters is that they land in separate
rows and can be zeroed independently, not that each has its own loop.

The file-openness sub-term was folded into that scan rather than given its own, on measurement: it
walks the same three files and needs the same clamped king file, and splitting it out cost about
1.8% of nps for nothing. `eval_king_pawn_cover` also returns early at phase 0, which is exact rather
than an approximation — every contribution is a `{x, 0}` pair, and `BlendPhase` of one at phase 0 is
0 for every `x`.

The ablation in PR 4 is the whole point of the split. Merging shelter and storm into one row would
leave a PR 2 regression unattributable between a mis-scaled shield table and an over-weighted storm
table, which are corrected in completely different ways. Cost is at most three extra `BlendPhase`
truncations (≤ 1 cp each, deterministic), which `RawWhitePov` already documents as the accepted
price of a reconstructible breakdown.

### D9: Four PRs, gated separately

| PR | Content | Gate |
|---|---|---|
| 1 | Attack aggregates in `EvalContext`; mobility, rooks rewired | Exact equivalence + nps. **No Elo run.** |
| 2 | Shelter, storm and king-file openness (3 rows) | Local SPRT `NonRegression`, then `Gain` |
| 3 | `eval_king_attack` + cap | Local SPRT `NonRegression`, then `Gain` |
| 4 | Ablation of `eval_castling` and the mg king PST | See Validation |

Bundling 2 and 3 would make a regression unattributable between a mis-tuned pawn table and a
mis-tuned danger curve, which are fixed in completely different ways.

PR 2's three rows land together on **one** SPRT, not a nested shelter / +storm / +files ladder. The
ladder triples PR 2's match budget to buy attribution for a regression that may not occur, and the
four separate rows (D8) mean the information is still recoverable if it does: on a failure or an
inconclusive result, fall back to the ladder then, with the row that moved as the first suspect.

## Assumptions I cannot verify from the code

- **The edge-file discontinuity is worth fixing.** Taken from Stockfish PR #1512, not measured here,
  and their remedy is not the zone shape adopted in D3. Verified instead by construction: tests pin
  the exact zone mask for `Kg1`, `Kh1`, `Kg2`, a central king and their mirrors, so the g1→h1 failure
  mode cannot exist regardless of whether their measurement transfers.
- **Literature-standard shelter/storm magnitudes are a sane starting point.** Not verified. If PR 2's
  `NonRegression` SPRT fails, the first response is to halve the tables, not to abandon the term —
  a mis-scaled structural table is the expected failure, not a wrong idea.
- **The planning band (+10 to +40 Elo) is a hypothesis, not a claim.** It rests on mobility's
  measured +38.34 ± 4.26 for a comparably absent whole-board term. Settled only by PRs 2–3.
- **The ~1.7 Elo per 1% nps conversion** is the project's existing calibration, reused rather than
  re-derived.
- **`BuildContext` is the only construction site**, so the test fixture and production cannot drift
  — verified from `Eval.h`, which states it, and from `StratChessTests/EvalTestFixture.h` and
  `EvalTermTests.cpp` calling it.

## Invariants

- **PR 1 changes no searched score.** Identical evaluation on the corpus, and identical node counts
  and best moves at `Threads=1`. The contract is over `Evaluate()`, not over `Breakdown()`: for a
  dead-drawn class the mobility and rooks rows change from the counts nothing consumed to 0, which
  D2 accepts rather than making the debug path compute something the search never did.
- Color symmetry: mirroring a position negates the score, including with the file-asymmetric shelter
  and storm tables (#125's mirror property, which is why those tables are safe to introduce here).
- `EvalContext` remains a per-call stack local; `Evaluate()` stays `const` and allocation-free, and
  no term reads state another term wrote.
- Every king-safety contribution is a `ScorePair{x, 0}` and is 0 at phase 0.
- The combined king-safety contribution for one color never exceeds `KING_SAFETY_MAX_PENALTY`, for
  any legal position and any number of attackers.
- `penalty` is monotone non-decreasing in `danger` — the clamp in D6 is what guarantees it, and no
  input can make a safer king score worse.
- A kingless board (default-constructed or failed parse, `king_sq == NO_SQUARE`) scores 0 from all
  four contributions — the same guard `eval_pst`, `eval_mopup` and `eval_castling` already carry.
- `EvalBreakdown` rows plus `endgame_adjustment` still reconstruct `total`.

## Validation

**PR 1 — behaviour-preserving.** `Compare-SearchEquivalence.ps1 -After <exe>` (identical node counts
and best moves at `Threads=1`), the `[eval]` suite, and alternating `Run-Bench.ps1` runs with the
per-position spread. An Elo run here would be measuring nothing and is explicitly not done.

**Measured: identical on all six equivalence positions, and no reproducible nps difference** (10 runs
per side, order-balanced against thermal drift; medians within 0.1%, means within 0.3%, both inside
the run-to-run spread).

Getting there took one wrong turn worth recording. An intermediate version filled the aggregates
through an `EvalContext&` and left `all_pieces` in the context after its last reader had gone; that
measured ~0.6% *slower*, order-balanced and well outside the spread. Returning `PieceAggregates` by
value and deleting the dead field removed the deficit. The lesson is that this context is close
enough to a cache-line budget for a single unused 8-byte field to be measurable — worth remembering
when PR 3 adds the zone aggregates.

The predicted small *gain* from removing `eval_rooks`' duplicate `RookAttacks()` also did not appear.
That premise looks wrong: within one inlined `RawWhitePov` the compiler could already common up the
two calls on the same square, so there was little duplicate left to remove. The refactor's value is
what it makes possible in PR 3, not a speed-up here.

**PR 2 measured: -5.4% nps** (5 alternating runs per side; before median 3.010 M, after 2.844 M,
means -5.4%, well outside the ~1% run-to-run spread). That is roughly -9 Elo of speed at the
project's 1.7 Elo per 1% conversion, and it overruns the 2% budget below on PR 2 alone — before PR 3
adds anything. Folding the file-openness loop into the shelter scan recovered 1.8 of the original
7.3%; what remains is the scan itself, run for both colours at every leaf. The two ways out are a
pawn hash (#131, `not-started/pawn-hash-table.md`) — shelter is a pure function of the pawns and the
king square, which is exactly what that cache exists for — or dropping sub-terms the PR 4 ablation
cannot justify. Which of those, and whether the SPRT runs before or after, is the project owner's
call.

**PRs 2 and 3 — strength.** `Run-Bench.ps1` first: the incremental budget for the whole feature is
2% nps (≈3.4 Elo of speed). Then a local SPRT against a **merge-base build** (never the anchor):
`NonRegression` first, `Gain` second. New `[eval]` cases, with matching `Docs/TestDesign.md` entries:

- **exact zone masks** for `Kg1`, `Kh1`, `Kg2`, a central king and their mirrors — the enumerated
  table in D3, not merely "three files wide";
- intact / pushed / missing shield with identical non-king material;
- a pawn on the king's own rank versus no pawn on that file — the sentinel case D4 exists to
  separate;
- an enemy pawn *behind* the king contributes nothing to storm;
- blocked versus unblocked storm pawn, pinning the exact `r' - 1` condition;
- open versus half-open versus pawn-closed king file;
- one versus two attackers (assert the inequality, not tuned values), and **queen-only pressure**
  scoring nonzero — the case the rejected two-attacker gate would have zeroed;
- monotonicity of the danger curve across a sweep, including a negative pre-clamp `danger`;
- flight-square loss, and a `pseudo_safe_king_moves` case documenting the x-ray blind spot rather
  than asserting legality;
- every reachable `SHELTER`/`STORM` bucket and its color mirror;
- phase fading, `KING_SAFETY_MAX_PENALTY`, kingless board, mirror symmetry, breakdown
  reconstruction.

Every new test is falsified by reverting the term and watching it fail.

**PR 4 — ablation, and where CI is worth its cost.** The three configurations (drop `eval_castling`;
flatten the mg king PST; drop both) differ by well under 25 Elo, where a local fixed batch cannot
resolve them and an SPRT gives a verdict rather than a number. This is the one place the **CI
strength lab** earns its ~3 h and 18 of 20 job slots. Recommendation, for the project owner to
approve rather than for an agent to start:

- **One lab run after PR 3**, with `reference_ref` set to an explicit **pre-series tag** cut from
  `main` before PR 1 merges, to put a number (±5 Elo at ~20k games) on the complete feature. The
  default `reference_ref = merge-base` is *wrong* for this run and would silently answer a different
  question: by then PRs 1 and 2 are already in `main`, so the merge base contains them and the run
  would measure PR 3's attack term alone while being recorded as the feature's Elo
  (`Docs/CI.md` → `reference_ref`). The tag is **`king-safety-pre`**, cut in PR 1 at `e0eb564` — the
  `main` this series forked from — so it cannot be lost. It
  also means unrelated `main` commits landing during the series contaminate the comparison; note in
  the row what else merged in between.
- **One lab run in PR 4** for the single most promising ablation — with `reference_ref` set to the
  post-PR-3 commit, *not* `merge-base`, since the comparison is candidate-vs-candidate. Use local
  `Custom -Elo0 -10 -Elo1 0` SPRTs to shortlist which single ablation is worth that run; wider bounds
  cost roughly 4× fewer games than `NonRegression`.
- No lab run for PR 1 (equivalence is exact) or PR 2 (an SPRT verdict is the decision being made).

Results are recorded per `Measurements/README.md` — lab rows in `Measurements/ci-per-change.md`,
local rows appended to `local.md` by the script, and never compared across the two files.

If no configuration clears `NonRegression` after re-scaling the tables once, keep only the
sub-terms that individually do, or close #97 with the SPRT evidence rather than shipping an
intuitive term.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Attack aggregates are counts, not weights; why per-type unions are absent and why the `[4]` index is eval-local, not `ePieceType` (D1) | comment on the new `EvalContext` fields |
| Attacks are skipped at `endgame_scale == 0`, and what that means for `Breakdown()` (D2) | comment in `BuildContext` |
| The two clamps, the 12-vs-9-square exception, and the discontinuity they prevent (D3) | comment on the zone helper in `Eval.cpp` |
| Relative-rank indexing with 0 reserved for absence, and the blocked-storm condition (D4) | comment above the tables in `Eval.h` |
| Shelter/storm tables are unfitted literature shapes owned by #117 (D4) | comment above the tables in `Eval.h` |
| King-file definition differs from `eval_rooks` on purpose (D5) | comment on `eval_king_files` |
| Quadratic-with-cap over a danger table; why the clamp is about sign, not overflow (D6) | comment on `eval_king_attack` |
| `pseudo_safe_king_moves` is not a legal king-move count, and the error is one-directional (D6) | the value's name plus a comment where it is computed |
| `KING_SAFETY_MAX_PENALTY` and its derivation from the table extrema | `Eval.h` constant + `static_assert` + corpus assert in `EvalTermTests.cpp` |
| Which sub-terms and overlap ablations survived, with measured Elo | `Docs/Changelog.md`, issue #97, `Measurements/ci-per-change.md` |
| New `[eval]` coverage | `Docs/TestDesign.md` |
| `GetLastPiece` helper contract (D4) | comment beside `Board::GetFirstPiece` |

Delete this file once PR 4 lands and the table above is discharged. Nothing durable should survive
only here.
