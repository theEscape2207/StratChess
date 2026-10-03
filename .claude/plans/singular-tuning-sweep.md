# Singular-extension tuning sweep — Design

**Issue:** #702

## Goal

Singular extensions ship at min depth 8, TT depth margin 3 and margin factor 2: +22.9 ± 3.6 Elo
against off (#95). Nobody knows whether that point is good or just the first one tried. The
strength lab answers one question per run: one candidate against one reference, ~3 h, ±3.6 Elo at
20k games, one run at a time repository-wide. Walking the knobs one point per run costs a working
day for three points, and most of those runs will confirm "about the same". This design gets the
most decision-relevant information per lab-hour: a cheap local pre-screen, one multi-arm lab run
that screens several settings at once, and a full-precision run only for a setting that earns it.

## Review focus

- **D2: screening at ±5.4 Elo per arm.** The likely effect of tuning is single-digit Elo, so the
  screen may return "nothing distinguishable" for every arm. Check that the decision rule (D4) still
  turns that outcome into a decision rather than another run.
- **D1: the workflow change.** Assigning arms per shard touches the instrument every Elo claim in
  this repository rests on. The default path (no arms) must stay byte-for-byte the current
  behaviour.
- **D3 and D4: the choice of arms.** Is a margin-factor bracket plus min depth 10 the right three
  for screen 1? Does D4's outcome table send screen 2 somewhere useful in every case, including
  pooling a repeated arm across two runs?

## Scope

**This change will:**

- Add an optional multi-arm mode to `strength.yml`: one run, several candidate option sets, each
  pooled separately against the shared reference.
- Pre-screen the candidate settings locally for tree cost (`Tests/profile-screen.fen`, one pass
  each).
- Run two three-arm lab screens, the second chosen by the first's outcome, and then at most one
  full confirmation (D4).
- Record the results, and change a default only on a confirmed win.

**This change will not:**

- Tune `SingularTtDepthMargin`, unless screen 1 comes out flat (D4 (c)). It is the least-understood
  lever, with no cost hypothesis behind it.
- Add double or negative extensions, or multi-cut. Those are new features, not tuning.
- Build SPSA or any continuous tuner (D5).
- Change the lab's pooling formula, failed-shard rule or calibration.

## Decisions

### D1: Multi-arm lab runs: arms assigned per shard, pooled per arm

New `strength.yml` input `candidate_arms`: `;`-separated option sets, each in today's
`candidate_uci_options` syntax, e.g.
`SingularMarginFactor=1;SingularMarginFactor=4;SingularMinDepth=10`.

- With K arms, shard *i* plays arm *i* mod K. Interleaving spreads every arm across the whole
  opening book instead of handing it one contiguous slice.
- `setup` fails fast unless `shards` is a multiple of K, every arm passes
  `validate_uci_options.py` against the candidate engine, and `candidate_arms` and
  `candidate_uci_options` are not both set.
- Each shard's fastchess engine name carries its arm label, so logs and PGNs identify the arm
  without needing the shard index.
- The pool step runs `pool_pentanomial.py` once per arm, on that arm's shard logs, with
  `--expect-shards` = shards/K. It prints one table per arm, each with its option set. The script
  itself is unchanged: an arm is just a smaller batch.
- The failed-shard rule applies to the **whole run**: one failed shard discards every arm, for the
  same survivorship reason as today.
- Empty `candidate_arms` (the default) takes exactly today's path.
- A second new input, `opening_offset` (default 0), is added to every shard's start opening. With
  it, a later run can play openings disjoint from an earlier run's, so an arm repeated across
  screens pools as independent games (D4). The existing book-size check counts the offset. The book
  has 242,201 openings; a 26,640-game screen uses 13,320.

**Rejected: a fastchess gauntlet in every shard (all arms against the reference on the same
openings).** It would pair arms on identical openings. But each arm is already paired against the
reference by colour-swapped pairs, so the extra variance it removes is small. It would also mean
parsing multi-pair fastchess output, which the calibrated pooling script does not do. Not worth
touching the calibrated path for.

**Rejected: sequential single-candidate runs.** No workflow change, but ~3 h per point. Six arms
plus a confirmation would take seven runs instead of three.

### D2: Screen first, then confirm: K=3 arms × 6 shards, 26,640 games (~4 h)

The owner approved ~4 h for the screen. `games=26640` gives 1,480 games per shard (~229 min, inside
the 340-min job timeout) and 8,880 per arm, so about **±5.4 Elo per arm**. That is the ±3.6 of a
20k-game run scaled by √(20,000 / 8,880). An arm-vs-arm difference is about ±7.6. At the default
20k, each arm would get ±6.2: the extra hour narrows the bars by ~13%, because precision grows only
with √games.

K=3 is the largest arm count that keeps each arm able to see a ~5-Elo effect. K=6 (±7.6 per arm)
could only find cliffs. K=2 (±4.4) would buy precision by dropping a question (D3).

The multi-arm run does not create information: total games are fixed. It trades precision for
breadth. That pays off here because most settings are expected to be flat or worse, and a coarse
screen rejects those for a third of the cost each. The point that matters gets full precision in
the confirmation run (D4).

### D3: The arms: both sides of the margin factor, plus min depth 10

All three arms run against the shipping default, so each one reads directly as "change the default
or not":

| Arm | Setting | Question |
|---|---|---|
| A | `SingularMarginFactor=1` | More extensions: does the curve still rise past today's point? |
| B | `SingularMarginFactor=4` | Fewer extensions, chosen by margin: the cheaper point slice A favoured |
| C | `SingularMinDepth=10` | Fewer extensions, chosen by depth: −45% bench cost, with the surviving extensions near the root |

A and B bracket the current point on the axis where the most leverage is expected, so the screen
shows the slope's sign rather than one point. C tests whether the cheaper lever is depth rather than
margin.

The reference side sets the shipping defaults **explicitly**
(`SingularMarginFactor=2 SingularMinDepth=8 SingularTtDepthMargin=3`) through
`reference_uci_options`, so a later default change cannot silently move the baseline of a re-run.
Both sides build the same SHA.

**Arm substitution:** if Stage 0 shows an arm's verified and extended counts identical to the
default's on `profile-screen.fen`, that arm tests nothing. Replace it with the next point out on the
same axis (factor 0 for A, factor 6 for B, min depth 12 for C).

### D4: Two adaptive screens, then one confirmation, with the rules fixed before screen 1

The owner approved a budget of three lab runs, run back to back: screen 1 (~4 h), screen 2 (~4 h),
and a confirmation (20k games, ~3 h). Screen 2's arms depend on screen 1's outcome, so the second
run goes where the first one points. Its rule is fixed now, so the choice cannot follow the noise.

Read each arm's estimate *e* against the default (SE ≈ 2.8 Elo per screen). The **leader** is the
arm with the highest *e*.

**Screen 2 arms, by screen 1 outcome:**

| Screen 1 outcome | Screen 2 arms |
|---|---|
| (a) Leader *e* ≥ +3 | The leader again; the next point beyond it on its axis; the leader combined with the better setting from the other axis |
| (b) No leader ≥ +3, but one axis slopes (one side ≤ −3, the other ≥ +1) | The improving arm again; two points further out in the improving direction |
| (c) Flat: no arm ≥ +3 and no slope | `SingularTtDepthMargin=1`; `SingularTtDepthMargin=5`; `SingularMinDepth=6` (more extensions by depth, mirroring C) |

"Next point out" on each axis follows these steps: margin factor 0, 1, 2, 3, 4, 6, 8; min depth
6, 8, 10, 12. An arm repeated from screen 1 plays a disjoint slice of the book (`opening_offset`,
D1), so its two screens pool into one estimate (~±3.8).

**Confirmation:** take the best arm over both screens, using the pooled estimate where an arm ran
twice. If its *e* ≥ +3, confirm it with a single-candidate 20k-game run against the default. Change
the default only if that run's interval excludes zero. A screen estimate is never quoted as the
gain, because the best of several noisy arms is biased upward. If no arm reaches +3, skip the
confirmation, keep the defaults, record both screens and close #702.

**Every run plays its own openings:** screen 1 uses `opening_offset=0`, screen 2 uses 13,320 and
the confirmation uses 26,640. A confirmation on the screen's openings would reuse the opening
draw that may have helped its arm win the selection, so it would not be independent of it.

**Reject** any arm whose upper bound is below zero. It is never carried into screen 2.

### D5: Rejected: SPSA or another continuous tuner

SPSA gets the most out of every game for continuous parameters. But fastchess has no SPSA mode, so
it would mean a new harness, and these knobs are small integers with only a handful of useful values
each. A grid screen answers the same question with no new tooling.

### D6: Rejected: a shorter time control to buy more games

At 5+0.05 the lab plays twice the games. But singular extensions only fire at depth ≥ min depth, and
their value depends on depth, so tuning at a shorter time control biases the result toward cheaper
settings. The sweep stays at the 10+0.1 the feature was accepted at.

## Stage 0: local pre-screen (no lab time)

For the default point and every arm: one pass of `Tests/profile-screen.fen` at `Threads=1`, depth
12, recording edges, verified, extended and wall clock. One pass is enough: time per edge is flat,
and node counts are deterministic (#95, slice A).

This sets the expected cost of each arm and catches a degenerate arm (D3's substitution rule). It
predicts nothing about Elo; the #95 cost model was 2–4x too pessimistic.

## Free extra signal per run: depth reached

Every lab PGN move comment carries the search depth (`{+1.10/12 0.400s}`). Comparing an arm's mean
depth with the reference side's, in the same games, measures the cost the arm **actually paid at
10+0.1**, which the bench cannot. Combined with the Elo result, this separates "cheaper but blinder"
from "no cheaper at all". It is read once per run with a scratchpad script and reported with the
result. It is diagnosis only, never a gate.

## Assumptions I cannot verify from the code

- **fastchess accepts a distinct `name=` per shard, and it shows in the log and PGN headers.** Not
  verified. It is settled by the smoke dispatch in Validation.
- **The depth in a PGN comment is the depth of the completed iteration.** Not verified. If it is
  instead the depth of an iteration that was cut off, the comparison is still paired and valid, but
  it reads as relative depth, not absolute. Settled by matching one `info depth` line against its
  PGN comment in a local fastchess game.

## Invariants

- A dispatch without `candidate_arms`, and with `opening_offset=0`, runs exactly today's job graph,
  inputs and pooled output.
- With `opening_offset=N`, shard *i* starts at opening `N + i*ROUNDS + 1`.
- Each arm's result is pooled only from its own shards, and one failed shard discards the whole run.
- An arm whose options the engine does not advertise fails `setup`, before any build or game.

## Validation

**Tooling tier** (workflow and docs only). No engine code changes, so no equivalence check or bench
applies.

- `pool_pentanomial.py --self-test` passes, unchanged.
- **Smoke dispatch with arms**: `games=72 shards=6`, three arms. Pass if the summary shows three
  tables of 2 shards each, and each shard's log shows its arm's options on the fastchess command
  line and its arm label as the engine name. It runs with `opening_offset=1000`, and passes only if
  each shard's echoed start opening is 1000 higher than the no-offset formula gives. That `setoption` reaches the search is already pinned
  by `UCITests.cpp` (#700).
- **Smoke dispatch without arms**: `games=36 shards=6`. Pass if it gives a single table, unchanged.
- **Negative checks in `setup`**: 18 shards with 4 arms, an arm option the engine does not
  advertise, and both inputs set. Each must fail in `setup`.

The Elo verdicts come from the two screens and the optional confirmation, recorded in
`Measurements/ci-per-change.md` (one row per arm and screen, and a pooled row for a repeated arm).

## Cost

- **Size:** `strength.yml` ~60–100 lines; `Docs/CI.md` and `measure-strength/reference/strength-lab.md`
  a few lines each. Three files.
- **Blast radius:** Tooling tier. Only the lab workflow changes, and it is dispatch-only.
- **Review:** one code review run.
- **Lab time:** two ~4 h screens and a ~3 h confirmation, run back to back (~11 h, 18 of 20 CI
  slots throughout). The confirmation is skipped if no arm reaches +3.
- **Optional: the depth-from-PGN readout.** A scratchpad script of ~30 lines, no commit. It covers
  the "why" of a result, not the verdict. Drop it if the screen is flat.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| `candidate_arms` semantics, shard interleaving, whole-run discard rule | `strength.yml` comments, `Docs/CI.md` → Strength lab input table |
| When to use a multi-arm screen, and per-arm precision (scales with √(games per arm)) | `measure-strength/reference/strength-lab.md` |
| Screen and confirmation results | `Measurements/ci-per-change.md`, #702 comment |
| A changed default, if any | `SearchTuning.def`, `Docs/Changelog.md` |
