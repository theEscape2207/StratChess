# Move-quality scan — Tier 1

Runs of `Scripts/analyze_move_quality.py` over strength-lab PGNs: where the engine's own
judgement moved, bucketed by phase, piece, clock and material class. The instrument — what each
column means, how it is regenerated and what it cannot see — is
[`../Docs/MoveQuality.md`](../Docs/MoveQuality.md). This file holds only what each run measured.

**These are profile tables, not verdict rows.** See the carve-out in [`README.md`](README.md):
a move-quality run is a diagnosis, not a strength decision, so it carries no `Verdict`. The rule
above it still binds — a profile is only ever read against other profiles in this file, and never
against a Tier 2 profile of a *different* run. The licensed cross-tier comparison is the `self`
columns inside a Tier 2 table, which score exactly the rows the oracle judged.

---

## Run 33215162562 — baseline

| | |
|---|---|
| Run | [33215162562](https://github.com/theEscape2207/StratChess/actions/runs/33215162562), 2026-08-28 |
| Builds | `candidate-ff5d2f5` (`worktree-lmr-depth-clamp`) vs `reference-be5ca11` (merge-base) |
| Result | pooled **+9.64 ± 3.63 Elo**, 9990 pairs, score 51.39% |
| Corpus | 19,980 games · 2,230,649 annotated moves · 15,528 mate scores · 0 games skipped |
| Toolchain / TC | GCC on ubuntu-24.04, Release, Threads=1, 10+0.1, `UHO_4060_v3.epd` |

### Game outcomes

Decisive 12,550 (62.8%, White 41.1% / Black 21.7%) · drawn 7,430 (37.2%) · ended by adjudication
13,589 (68.0%) · over 200 total plies **1,615 (8.1%)**.

Draws by the reason fastchess recorded, against what the better-placed side reported on its **last**
move:

| Draw reason | Games | Of all | Last score ≥ 150 cp |
|---|---|---|---|
| 3-fold repetition | 4,393 | 22.0% | 0.1% |
| Insufficient mating material | 1,404 | 7.0% | **47.4%** |
| Adjudication (both sides ≈ 0.00) | 1,094 | 5.5% | 0% |
| Fifty-move rule | 507 | 2.5% | 0% |
| Stalemate | 32 | 0.2% | 0% |

### Blunder profile, contested positions only

Self-swing ≥ 150 cp, mover within ±150 cp before moving. Pooled over both builds, which differ by
under 0.05 percentage points in every cell.

| Phase | Moves | mean \|swing\| | ≥150 cp (95% clustered) | ≥300 cp |
|---|---|---|---|---|
| opening | 522,989 | 11.1 | 0.154% [0.144, 0.165] | 0.03% |
| middlegame | 522,161 | 12.8 | 0.293% [0.277, 0.308] | 0.09% |
| endgame | 393,243 | 9.9 | 0.215% [0.200, 0.231] | 0.07% |

| Piece | Moves | ≥150 cp (95% clustered) |
|---|---|---|
| bishop | 227,637 | 0.160% [0.144, 0.178] |
| knight | 195,017 | 0.200% [0.180, 0.220] |
| pawn | 289,165 | 0.204% [0.188, 0.220] |
| king | 241,601 | 0.216% [0.198, 0.236] |
| queen | 175,410 | 0.262% [0.238, 0.287] |
| rook | 309,563 | 0.277% [0.258, 0.295] |

Both sets of intervals are disjoint at the extremes — middlegame is 1.9× opening, rook 1.7× bishop —
so neither is flat, but both sit inside one narrow band, with heavy pieces at the top. Nothing here
resembles the 34%-of-blunders king-move story the unrestricted scan tells.

By the clock bucket the mover was in when it started thinking rather than what was left afterwards;
the two differ for 5.0% of moves:

| Phase | > 8 s | 5–8 s | 2–5 s | < 2 s |
|---|---|---|---|---|
| opening | 0.03% | 0.17% | 0.41% | 0.30% |
| middlegame | 0.04% | 0.11% | 0.30% | **0.47%** |
| endgame | — | 0.00% † | 0.12% | 0.25% |

† zero blunders in 1,293 moves — the engine is almost never in an endgame with that much clock left.

### Eval calibration

Observed score for the side to move, by the score it reported, split on whether both sides still hold
at least three pawns. The split is the control separating "the endgame eval is optimistic" from "the
eval is blind to drawish material".

| Reported | opening (rich) | middlegame (rich) | endgame (rich) | middlegame (poor) | endgame (poor) |
|---|---|---|---|---|---|
| +100–150 | 0.735 | 0.750 | 0.744 | 0.692 | 0.631 |
| +200–250 | 0.889 | 0.895 | 0.908 | 0.831 | 0.749 |
| +250–300 | — | — | — | 0.888 | 0.789 |
| +300–350 | 0.959 | 0.965 | 0.983 | 0.882 | **0.764** |
| +500–550 | 1.000 | 0.991 | 0.997 | 0.997 | **0.897** |

Pawn-rich, the three phases agree within noise. Pawn-poor (either side ≤ 2 pawns) the endgame column
falls away and stops being monotonic.

### Drawish material classes

Counted once per game at the first position reaching the class, against what the stronger side
reported there. Games, not plies — plies over-weight exactly the long drawn games.

| Class | Reported | Games | Of all | Observed (95% clustered) | W/D/L |
|---|---|---|---|---|---|
| K + minor vs K | ≥ +250 | 656 | 3.3% | **0.500** [0.500, 0.500] | 0/656/0 |
| KR+minor vs KR (pawnless) | ≥ +250 | 262 | 1.3% | **0.645** [0.618, 0.672] | 76/186/0 |
| KR vs K+minor (pawnless) | +100–249 | 58 | 0.3% | **0.509** [0.500, 0.526] | 1/57/0 |
| KR vs K+minor (pawnless) | ≥ +250 | 151 | 0.8% | 0.801 [0.762, 0.838] | 91/60/0 |
| KR vs K+minor (pawnless) | ≥ +100 cumulative | 209 | 1.0% | 0.720 [0.684, 0.751] | 92/117/0 |
| Opposite-coloured bishops | ≥ +250 | 143 | 0.7% | 0.881 [0.846, 0.916] | 109/34/0 |
| Opposite-coloured bishops | +100–249 | 279 | 1.4% | 0.642 [0.615, 0.668] | 82/194/3 |
| Opposite-coloured bishops | ≥ +100 cumulative | 422 | 2.1% | 0.723 [0.697, 0.748] | 191/228/3 |

### Cross-build noise floor

Mean `|s_X(t) + s_Y(t+1)|` between two builds differing only by the LMR depth clamp: **9.1 cp**
opening, **14.8 cp** middlegame, **17.8 cp** endgame. A run whose builds differ in evaluation should
exceed this; one that does not has not changed what the engine believes.

A clean version exists for a subset: identical FENs that **both** builds actually scored, no ply
offset. Within a shard there are 46,389, and the gap is **5.4 cp** — but 93% are opening positions
and only 469 endgames, because the builds rarely reach the same middlegame. It cross-checks the
opening row (9.1 against 5.4, the difference being the one-ply offset); it cannot replace it.

### Row detail

*Correction 2026-09-08:* the long-game figure was published as 1,204 (6.0%), which counted recorded
moves and so silently required over 216 total plies for this fullmove-9 corpus. Recounted over the
same run with the setup offset included; the superseded value is not comparable to the 6.6% and 7.3%
tracers, the corrected one is.

**68.0% of this run's games ended by adjudication**, so its endgame rows read the position at the
moment of adjudication rather than played-out technique. `Docs/MoveQuality.md` → Limits has what
that costs.

**Figures the tables above do not carry.** Findings 2, 3, 5, 6 and 7 in `Docs/MoveQuality.md` cite
further cuts of this run's report that were never published as rows here: 26.8% of
insufficient-material draws still had a pawn two plies earlier and 44.1% six plies earlier; 99.9% of
repetition draws had both sides under 50 cp on their last move; 21% of contested middlegame
decisions started with under 2 s on the clock; 1,462 and 1,344 games saw a build announce a forced
mate, all won; and the pawn-poor endgame curve reaches 0.998 at +575, a band above the `+500–550`
row. They come from the same scan of the same corpus, but nothing in this ledger reproduces them —
regenerate the run to check one.

---

## Runs 33429454765 and 33568346899 — level-material rook endings

| Class | Reported | 33429454765 | 33568346899 |
|---|---|---|---|
| KR vs KR (pawnless) | < +100 | 368 games, **0.500** [0.500, 0.500] | 361 games, **0.500** [0.500, 0.500] |
| KR vs KR (pawnless) | ≥ +250 | 309 games, **1.000** [1.000, 1.000] | 307 games, **1.000** [1.000, 1.000] |
| KR vs KR (pawnless) | level, no side | 224 games, drawn 100.0% | 289 games, drawn 100.0% |

The class separates completely, on corpora sharing no games, and the band that matters is the low one
— the reverse of every other class here. **Below +100 the reported edge did not predict a decisive
result once**, and the games entered dead level were drawn too. At ≥ +250 the score does predict one,
because it comes from a leaf where the rook has already fallen and the material is no longer this
class. Nothing lands in between.

Read that as calibration, not absence of information: the magnitude in the low band carries no signal
about the result, but whether its ordering still steers the search usefully is a separate question
these rows cannot answer. That is why #436 discounts the class rather than zeroing it. The low band
is the PST, rook-file and mobility terms surviving on a level-material board — median 22 cp, maximum
64, over 400 positions sampled from the first run.

The second run shows the scale itself: `|score|` reported from inside the class has p90 **13**
unscaled and **3** scaled, 4/16 to the digit. **No steering effect is visible** — the builds enter the
class 777 and 784 times and spend 6.6 and 6.8 plies there — and the low band offers none to find,
being already saturated at 0.500.

### Row detail

`RvsR` (pawnless K+R vs K+R) was added to `material_classes()` after the baseline, so these rows come
from later, post-#128 runs and are **not comparable ply-for-ply with run 33215162562's tables**.
Two independent corpora of 19,980 games each; the second carries the #436 scale on one side only.

**A per-build split measures the builds' scales, not their play.** Run `33568346899`'s `< +100` band
splits **145 candidate / 216 reference** — 3.7 standard errors, and it looks exactly like the #436
scale's intended effect. It is not: the skew sits entirely in the `|cp| 1–24` bucket the scale
compresses, and from `|cp| ≥ 25` up it is 150/142. Run `33429454765`, where neither build scaled the
class, splits 202/166 the other way. That is why the rows above are pooled rather than split.

The [baseline run](#run-33215162562--baseline) predates the level line, so its
dead-level `OCB` entries are not broken out at all: they sit in that run's unpublished `< +100`
band rather than in any row above.
