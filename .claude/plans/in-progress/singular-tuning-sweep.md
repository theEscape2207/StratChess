# Singular-extension tuning sweep — Design

**Issue:** #702

## Goal

Singular extensions ship at min depth 8, TT depth margin 3 and margin factor 2: +22.9 ± 3.6 Elo
against off (#95). Nobody knows whether that point is good or just the first one tried. The
strength lab answers one question per run: one candidate against one reference, ~3 h, ±3.6 Elo at
20k games, one run at a time repository-wide. Walking the knobs one point per run costs a working
day for three points, and most of those runs will confirm "about the same". This design gets the
most decision-relevant information per lab-hour: a cheap local pre-screen, two multi-arm lab
screens, and a full-precision run on held-out openings only for a setting that earns it.

## Review focus

- **D4: the adaptive rule and the holdout.** Does the screen-2 mapping cover every screen-1 outcome
  with a valid, distinct arm? Is the confirmation independent of the selection?
- **D1: the workflow change.** Assigning arms per shard touches the instrument every Elo claim in
  this repository rests on. The default path (no arms, offset 0) must stay byte-for-byte the
  current behaviour.
- **D2: screening at ±5.4 Elo per arm.** The likely effect of tuning is single-digit Elo, so a
  screen may return "nothing distinguishable". D4 must still turn that into a decision.

## Scope

**This change will:**

- Add an optional multi-arm mode and an opening offset to `strength.yml` (one PR, Build tier).
- Pre-screen the candidate settings locally for tree cost, as a description only (Stage 0).
- Run two three-arm lab screens, the second chosen by the first's outcome, and then at most one
  confirmation on held-out openings (D4).
- Record the results. If the confirmation wins, change the default in a separate Engine-tier PR.

**This change will not:**

- Tune `SingularTtDepthMargin`, unless screen 1 comes out flat (D4 (c)). It is the least-understood
  lever, with no cost hypothesis behind it.
- Add double or negative extensions, or multi-cut. Those are new features, not tuning.
- Build SPSA or any continuous tuner (D5).
- Change the lab's pooling formula, failed-shard rule or calibration.

## Decisions

### D1: Multi-arm lab runs: arms assigned per shard, pooled per arm

New `strength.yml` input `candidate_arms`: `;`-separated option sets, each in today's
`candidate_uci_options` syntax.

- With K arms, shard *i* plays arm *i* mod K. Interleaving spreads every arm through the openings
  the run uses, instead of handing each arm one contiguous slice.
- A new `.github/scripts/plan_arms.py` owns the routing, matching the convention of the existing
  scripts: it maps shard to arm and groups shard logs by arm for pooling, with a `--self-test`. The
  workflow calls it rather than doing the arithmetic in bash.
- **`setup`** parses the arms. It fails unless every arm is non-empty and well-formed, the arms are
  pairwise distinct, `shards` is a multiple of K, and `candidate_arms` and `candidate_uci_options`
  are not both set.
- **`build`** validates every arm with `validate_uci_options.py` against the freshly built candidate,
  before any match starts. This is where today's option validation already runs; `setup` has no
  binary. Reference options are still validated against the reference binary.
- Each shard's fastchess engine name carries its arm label, so logs and PGNs identify the arm
  without needing the shard index. The label is in fastchess's `name=` field, which the pinned
  v1.8.2-alpha writes into the PGN headers and game log (verified in source by review).
- The pool step runs `pool_pentanomial.py` once per arm, on the logs `plan_arms.py` assigns to it,
  with `--expect-shards` = shards/K. It prints one table per arm, each with its option set and shard
  IDs. `pool_pentanomial.py` itself is unchanged: an arm is just a smaller batch.
- The failed-shard rule applies to the **whole run**. The existing guard runs before any pooling,
  so one failed shard leaves no result for any arm.
- New input `opening_offset` (default 0, non-negative decimal): shard *i* starts at opening
  `offset + i*ROUNDS + 1`. The existing book-size check counts the offset. The book has 242,201
  openings; a 26,640-game screen uses 13,320.
- Empty `candidate_arms` and `opening_offset=0` (the defaults) take exactly today's path.

**Rejected: a fastchess gauntlet in every shard (all arms against the reference on the same
openings).** It would pair arms on identical openings. But each arm is already paired against the
reference by colour-swapped pairs, so cross-arm matching would remove only the opening variance
between arms, whose size has not been measured. It would also mean parsing multi-pair fastchess
output, which the calibrated pooling script does not do.

**Rejected: sequential single-candidate runs.** No workflow change, but ~3 h per point. Six arms
plus a confirmation would take seven runs instead of three.

### D2: Screen first, then confirm: K=3 arms × 6 shards, 26,640 games (~4 h)

The owner approved ~4 h per screen. `games=26640` gives 1,480 games per shard and 8,880 per arm,
so about **±5.4 Elo per arm**. That is the ±3.6 of a 20k-game run scaled by √(20,000 / 8,880). An
arm-vs-arm difference is about ±7.6. At the default 20k, each arm would get ±6.2: the extra hour
narrows the bars by ~13%, because precision grows only with √games. These are expected
half-widths, not measured ones.

Projected shard time is 229–260 min: 229 scales the workflow's documented 1,110-game shard, 260
scales #95's 3 h 15 min run. Both are inside the 340-min job timeout. Neither is measured for a
factor-1 arm, which searches more.

K=3 is the largest arm count that keeps each arm able to see a ~5-Elo effect. K=6 (±7.6 per arm)
could only find cliffs. K=2 (±4.4) would buy precision by dropping a question (D3).

The multi-arm run does not create information: total games are fixed. It trades precision for
breadth, which pays off when a coarse screen can drop the settings that are flat or worse. The
setting that matters gets its claim from the held-out confirmation (D4).

### D3: Screen-1 arms: both sides of the margin factor, plus min depth 10

All arms run against the shipping default, so each one reads directly as "change the default or
not":

| Arm | Setting | Question |
|---|---|---|
| A | `SingularMarginFactor=1` | More extensions: does the curve still rise past today's point? |
| B | `SingularMarginFactor=4` | Fewer extensions, chosen by margin: the cheaper point #95 slice A favoured |
| C | `SingularMinDepth=10` | Fewer extensions, chosen by depth. On the 8-position bench, it cut the feature's overhead from +50.1% to +27.5% (total wall clock −15%) |

A and B bracket the current point on the axis where the most leverage is expected, so the screen
shows the slope's sign rather than one point. C tests whether the cheaper lever is depth rather than
margin.

**Pinned configuration.** Every run of this sweep, the confirmation included, builds one engine SHA
on both sides (`reference_ref` = that SHA). Every dispatched option set, on both sides, names all
four singular options. The reference is always the shipping point:
`SingularExtensions=true SingularMinDepth=8 SingularTtDepthMargin=3 SingularMarginFactor=2`. Each arm
is that same set with only its named settings changed. "Same arm" means an identical four-option
set.

### D4: Two adaptive screens, then one confirmation on held-out openings

The owner approved three lab runs, back to back: screen 1 (~4 h), screen 2 (~4 h) and a
confirmation (20k games, ~3 h). Screen 2 goes where screen 1 points. Its rule is fixed now, so the
adaptation is reproducible and not discretionary. It is still subject to selection bias: a repeated
arm was repeated *because* it did well, so a pooled estimate that includes screen 1 is
exploratory, and only the confirmation supports a gain claim.

Each arm's estimate *e* is against the default (SE ≈ 2.8 Elo per screen). An arm is **rejected**
when its upper bound is below zero: it is never repeated, combined or confirmed. The **leader** is
the arm with the highest *e*. Exact ties go to the arm with fewer extensions, in the order C, B, A.

**Screen 2 arms.** Each row covers exactly one screen-1 outcome, checked top to bottom:

| Screen-1 outcome | Screen-2 arms |
|---|---|
| (a) leader A, *e* ≥ +3 | A again · factor 0 · factor 1 + min depth 6 |
| (a) leader B, *e* ≥ +3 | B again · factor 6 · factor 4 + min depth 10 (factor 3 if C was rejected) |
| (a) leader C, *e* ≥ +3 | C again · min depth 12 · min depth 10 + factor 4 (min depth 9 if B was rejected) |
| (b) B ≥ +1 and A ≤ −3 | B again · factor 6 · factor 8 |
| (b) A ≥ +1 and B ≤ −3 | A again · factor 0 · factor 1 + min depth 6 |
| (c) anything else | TT depth margin 1 · TT depth margin 5 · min depth 6 |

Row (a)-A combines with min depth 6, not C, because it continues A's direction (more extensions).
Every row's arms are distinct from each other and valid in their domains. Factor 0 is in range: it
sets the verification bound at the TT value itself. Min depth 12 is active in lab games, which reach
depth 13–16 (lab PGNs).

An arm repeated from screen 1 plays screen 2's disjoint openings, so its two screens pool into one
estimate of ~±3.8. That is the ordinary pentanomial half-width for 17,760 games, not a guaranteed
coverage for a selected arm.

**Confirmation.** Take the arm with the highest *e* over both screens (pooled where it ran twice;
same tie rule). If *e* ≥ +3, run it alone against the default: 20k games, the pinned configuration,
on held-out openings. Change the default only if the confirmation's own interval excludes zero.
Screen games never enter that verdict, and no screen estimate is ever quoted as the gain. If no arm
reaches +3, skip the confirmation, keep the defaults, record both screens and close #702.

**Every run plays its own openings.**

| Run | `opening_offset` | Openings |
|---|---|---|
| Screen 1 | 0 | 1–13,320 |
| Screen 2 | 13,320 | 13,321–26,640 |
| Confirmation | 26,640 | 26,641–36,630 |

A confirmation on a screen's openings would reuse the opening draw that may have helped its arm win
the selection. Each run's report states its range, and the confirmation is accepted only after the
three ranges are checked to be disjoint.

### D5: Rejected: SPSA or another continuous tuner

SPSA gets the most out of every game for continuous parameters. But fastchess has no SPSA mode, so
it would mean a new harness, and these knobs are small integers with only a handful of useful values
each. A grid screen answers the same question with no new tooling.

### D6: Rejected: a shorter time control to buy more games

Singular extensions fire only at depth ≥ min depth, and their value depends on depth. A shorter time
control would bias the result toward cheaper settings, and 5+0.05 also runs into the time manager's
100 ms floor. The sweep stays at the 10+0.1 the feature was accepted at.

## Stage 0: local pre-screen (no lab time)

For the default and every screen-1 arm: one pass of `Tests/profile-screen.fen` at `Threads=1`,
depth 12, recording edges, verified, extended and wall clock. One pass is enough, because node
counts are deterministic and #95 slice A measured time per edge as flat. If screen 2 runs a
min-depth-12 arm, it is screened at depth 14: at depth 12 it is never eligible.

Stage 0 is a description of tree cost and never changes an arm. Equal aggregate counters would not
prove two settings equivalent: the margin moves the verification window, which can change which
moves are extended without changing how many. It predicts nothing about Elo: #95's fixed-depth
break-even estimate (35–85 Elo) did not predict the measured +22.9.

## Diagnostic per run: completed depth

Each lab PGN move comment carries the depth of the engine's last completed iteration
(`{+1.10/12 0.400s}`; verified in the engine's UCI reporting and fastchess source by review). Per
arm, report mean completed depth for each side, over all non-book moves that carry a depth,
aggregated per game and then averaged. This is a diagnostic of how deep each setting got at
10+0.1. It is not a causal cost measure: the two sides search different positions, and an extension
changes how much work a nominal depth represents. It is read once per run with a scratchpad script,
and is never a gate.

## Assumptions I cannot verify from the code

None open. Both earlier assumptions (the fastchess `name=` field and PGN depth semantics) were
verified against the pinned fastchess v1.8.2-alpha source and `UCIHandler.cpp` in review. The
smoke dispatch exercises both end to end.

## Invariants

- A dispatch without `candidate_arms` and with `opening_offset=0` runs today's job graph and
  fastchess invocation, and produces today's pooled report.
- With `opening_offset=N`, shard *i* starts at opening `N + i*ROUNDS + 1`.
- Each arm's result is pooled only from its own shards, and one failed shard leaves no result for
  any arm.
- An arm the candidate does not advertise fails `build` before any match. A malformed arm list,
  duplicate arms, non-divisible shards or both inputs set fail `setup`.

## Validation

**Build tier** for the workflow PR: `Get-ChangeTier.ps1` classifies `.github/*` as Build. Its gates
run as that tier prescribes. No engine source changes in this PR.

- `pool_pentanomial.py --self-test` passes, unchanged.
- **`plan_arms.py --self-test`** with fixtures: six shard logs with distinct pentanomial counts and
  three arms. It asserts the exact shard IDs and summed counts for every arm. It also asserts a
  missing log is an error, along with non-divisible shards, duplicate arms and an empty arm.
- **Smoke dispatch with arms:** `games=72 shards=6`, three arms, `opening_offset=1000`. It passes
  if the summary shows three tables with the right shard IDs, each shard's fastchess command line
  carries its arm's options and label, and shard 0's first PGN starts from book line 1001.
- **Smoke dispatch without arms:** `games=36 shards=6`. Its fastchess command lines and pooled report
  are diffed against the same dispatch on `origin/main`, with run IDs, times and results
  normalized. They must otherwise match.
- **Negative dispatches:** 18 shards with 4 arms, both inputs set, a malformed offset and an offset
  that exhausts the book must each fail in `setup`. An option the engine does not advertise must
  fail in `build`.

That `setoption` reaches the search is already pinned by `UCITests.cpp` (#700).

A default change after a winning confirmation is a separate **Engine-tier** PR to
`SearchTuning.def`, with `search-reviewer` and that tier's validation. It is a deliberate tuning
change, so no equivalence check applies. The confirmation is its Elo gate.

Results go in `Measurements/ci-per-change.md`: one row per arm per screen, a pooled row (marked
exploratory) for a repeated arm, and the confirmation row.

## Cost

- **Size:** workflow PR: `strength.yml` ~60–100 lines, new `plan_arms.py` ~100 lines including
  its self-test, and a few lines each in `Docs/CI.md` and `measure-strength/reference/strength-lab.md`.
  Four files, Build tier.
- **Review:** one code review run.
- **Smoke dispatches:** about six small dispatches, minutes each.
- **Lab time:** two ~4 h screens and a ~3 h confirmation, back to back (~11 h, 18 of 20 CI slots
  throughout). These are projections, not guaranteed completion times. The confirmation is skipped
  if no arm reaches +3.
- **Default change, if any:** a one-line Engine-tier PR.
- **Optional: the completed-depth diagnostic.** A scratchpad script of ~30 lines, no commit. It
  explains a result; it is not the verdict. Drop it if the screens are flat.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| `candidate_arms` and `opening_offset` semantics, shard interleaving, whole-run discard rule | `strength.yml` comments, `Docs/CI.md` → Strength lab input table |
| When to use a multi-arm screen, per-arm precision, holdout openings for a confirmation | `measure-strength/reference/strength-lab.md` |
| Screen and confirmation results, opening ranges | `Measurements/ci-per-change.md`, #702 comment |
| A changed default, if any | `SearchTuning.def`, `Docs/Changelog.md` |
