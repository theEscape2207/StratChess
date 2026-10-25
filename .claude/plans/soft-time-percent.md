# Clock soft-time percent — Design

**Issue:** #908 (evidence: #103, #905, #906; triage
[#908 comment](https://github.com/theEscape2207/StratChess/issues/908#issuecomment-6394865763))

## Goal

At 10+0.1 the engine still spends more time per move than pays. Floor 50 (#103) gained
**+12.37 ± 3.04** by cutting mean time per move over moves 9–40 to 0.91x. #905 showed that a
uniform soft-limit cut at matched time plays level with it (**+0.92 ± 2.72**, run `40142881571`).
So the gain is spending less, not the stability signal choosing where. Nothing has tested whether
cutting further, on top of floor 50, gains again. `compute_budget`'s constants (horizon 30, 80% of
the increment, 50 ms overhead, 1.5x hard) have never been tuned, and no runtime knob moves any of
them. This change adds one knob and measures it in up to two lab arms, plus a harm check at a
second time control. It ships the knob's default only if both pass, and deletes it otherwise.

## Review focus

- **D1, the lever:** a soft-only scale at the stop check instead of a `compute_budget` constant.
  It leaves the hard limit, the horizon and the increment share alone. The question is whether a
  null result on it really says the budget has no headroom left, or only that this shape doesn't
  find it.
- **D5, the reading rule, and A2:** the runs are sized for about ±3. The plausible remaining gain
  is 0 to about +6 (floor 50 already took the < 2 s share from 48.8% to 20.9%). Check that the park
  and pass thresholds are honest at that power, and that a 30+0.3 non-inferiority check at −8
  (about ±5.5) is enough to ship a tuning found at 10+0.1.
- **D2, the shipped form:** the default flip keeps #905's exact arithmetic at the same site,
  rather than moving the scale into `compute_budget`.

## Scope

**This change will:**

- add the UCI option `SoftTimePercent` (spin 10–100). It multiplies the stop-check percent for
  clock searches, at the site and with the arithmetic of #905's throwaway `UniformSoftPercent`
  (commit `79cc6c3f`);
- keep it at default 100 (identical to `main`) on the PR branch while the lab runs;
- run up to three owner-approved lab runs from the PR branch under D5's rule: arm A, arm B
  (conditional) and a 30+0.3 harm check;
- on a pass, set the default to the winning U in the same PR and record the rows. On a park, close
  the PR and record the row on #908.

**This change will not:**

- touch `compute_budget`: not the horizon, the increment share, the overhead, the 1.5x hard factor
  or the 100 ms floor. Each would be its own follow-up, opened only if this one ships (D1);
- touch the hard limit, the changed-move extension, `StabilityTimeFloor` or its schedule
  (`soft_limit_percent`);
- change `movetime`, `depth`, `nodes`, `infinite` (with or without a clock) or the default-time
  fallback. The existing `soft_scalable` gate already excludes them;
- measure `Threads>1` (A4; optional, priced below);
- run a third arm. Going past B is a new owner decision taken from the ledger.

## Decisions

### D1: The lever is a uniform soft-only scale

A stop-check multiplier that applies to every clock search, stacked on floor 50.

- **It changes one variable.** A horizon or soft-scale change inside `compute_budget` would move
  soft and hard together (`hard = clamp(1.5 · soft, soft, cap)`), so a null or a harm result could
  not say which one did it.
- **It is the shape #905 measured.** The only open question is how far it goes.
- **It cannot raise forfeit risk.** The worst-case spend per move is today's hard limit.

Rejected for now:
- **Horizon:** its cut falls early, where floor 50 already cuts most (0.85x over 9–20), so that is
  where the curve most likely turns over.
- **Increment share:** at 2 s left the increment is 55% of the soft limit, so its effect depends
  on the increment-to-base ratio, and a 10+0.1 result transfers worst across TCs. (A uniform U
  cuts the increment part of soft too, so starving the low-clock band is not what separates the
  levers. D4 check 3 watches that band for both.)
- **`StabilityTimeFloor` below 50:** an option-only run on `main`, no code. But the floor bites
  only at `move_run ≥ 3`, so even floor 10 probably cannot reach arm B's 0.80x. It also keeps the
  stability shape that #905 found adds nothing measurable. U reaches both targets and cuts
  unsettled searches as well, which is the shape #905 matched.

A null here does not prove those levers are dead. It shows that less time per move, spread this
way, stops paying past floor 50. That is the main reason any of them was a candidate.

### D2: Form — `SoftTimePercent`, multiplied into the stop-check percent

One entry: `TUNING_FIELD(int, soft_time_percent, 100, 10, 100, true, "SoftTimePercent", true)`.
At `AIPerplex.cpp:539`:

```cpp
const int soft_percent = Engine::soft_limit_percent(state.move_run, tuning_.stability_time_floor) *
                         tuning_.soft_time_percent / 100;
```

This is `79cc6c3f`'s arithmetic, renamed. `SearchControl::ShouldStopIteration` already forces
100 when `soft_scalable_` is false, so non-clock searches and `go infinite` are unaffected with no
new code. A changed move (run 1) gets `100 · U / 100 = U`: the cut is uniform, as #905 measured.
The changed-move extension still keys on `move_changed`, not on the percent, so it is unchanged.
A lower U makes the soft check fire at shallower depths, though, where the move changes more often.
So the one extra depth is granted more often. That partly offsets the cut and adds to the
nonlinearity in U. The calibration absorbs it, and a flat calibration curve is not by itself a
wiring failure.

The option stays after shipping, as `StabilityTimeFloor` did. That makes a later retune at another
TC or thread count an option-only lab run on `main` instead of a code change.

Rejected:
- **A soft scale inside `compute_budget`.** It reaches `go infinite wtime …` (budgeted through the
  clock branch, but `soft_scalable == false`), which #103's D4 forbids shortening. Its floor clamp
  would also re-raise exactly the low-clock soft limits the measured arm cut. And its rounding
  differs from the measured arm, so it would need its own lab run.
- **Cherry-picking `79cc6c3f` onto a throwaway branch** (the triage's vehicle). The shipped form
  would then be a re-implementation that needs its own equivalence argument. Measuring the PR
  branch makes the merged binary differ from the tested one only in a default, as in #103.

### D3: Arms, calibrated on measured time, on top of floor 50

| Arm | Candidate | Reference | Target ratio, moves 9–40 | Prior for U (unverified) |
|---|---|---|---|---|
| A | `SoftTimePercent=U_A` | `merge-base` (`main`, floor 50) | **0.90 ± 0.03** vs floor 50 | 45–60 |
| B | `SoftTimePercent=U_B` | PR head, `SoftTimePercent=U_A` | **0.80 ± 0.03** vs floor 50, so about **0.89 ± 0.03** vs A | 30–45 |

The soft percent only decides whether the next depth *starts*, so time is strongly nonlinear in U.
At floor 100, #905 measured U=75 cutting almost nothing and U=55 cutting about 10%. U is therefore
chosen from measured time, never from the percent.

**Calibration probe** (local, clang-cl, 10+0.1, 200 games per point, under `measure-strength`'s
quiet-window rule, about 1–1.5 h). Run `SoftTimePercent=U` against the defaults on the same binary,
for U ∈ {70, 55, 40}, and add points as needed. Read the mean recorded time per move by build over
moves 9–40, plus 9–20 / 21–30 / 31–40, with a 95% game-bootstrap interval. Pick the U whose
interval lies inside the target, interpolating between points. The 200-game Elo is not read. U_B is
calibrated only if A passes. The chosen U, its measured ratio and its interval go in an issue
comment **before** the dispatch it gates.

A step of 0.90x repeats floor 50's own step (0.91x), and 0.80x is the next step of the same size.
The runs are sequential and full-size rather than a multi-arm screen: a ±4.3-per-arm screen cannot
separate arms a few Elo apart, and its winner shrinks on confirmation.

### D4: Validity checks, read before Δ, on every run

1. **Completion:** a batch with a time loss, illegal move or disconnect on either side is
   discarded, never reported (`Docs/Workflow.md`). A candidate time loss is also a blocking finding
   against that arm. The arm is not re-run without an owner call, whatever the Elo.
2. **Time matched:** the 9–40 ratio vs the run's reference is inside the D3 target. If not, the run
   is reported as answering a different question, and nothing re-runs automatically.
3. **Low-clock band** (`Scripts/analyze_move_quality.py` clock-band split, > 8 / 5–8 / 2–5 / < 2 s,
   by build, the method in `Measurements/move-quality-tier1.md`): the candidate's < 2 s share must
   not exceed the reference's. It fails only when the 95% game-bootstrap interval of the
   difference (candidate − reference) lies entirely above 0, so noise in a small band cannot block
   a ship. A soft-only cut banks clock, so a real rise means the saving went somewhere unexpected.
   The arm does not ship on that run, whatever the Elo.
4. **Reported, not gated:** the > 8 s share, the mean clock left at game end, and contested-blunder
   counts by clock band (direction only, unclustered). An over-cut shows up as clock banked and
   never spent.

At 30+0.3 the bands are absolute seconds, so check 3 compares candidate against reference only. No
absolute threshold is read.

### D5: Lab runs and the reading rule, declared before any dispatch

Δ = candidate − reference, in Elo, 95% interval. All runs use `strength.yml` with `--ref` set to
the PR branch, Threads=1, concurrency 3. Each run's `opening_offset` is the number of pairs the
earlier #908 runs consumed.

- **Run A**: 10+0.1, `games=26640`, 18 shards (about ±3, about 4 h), `opening_offset=0`.
  - **Headroom:** lower bound > 0. Calibrate U_B and request run B.
  - **Park:** upper bound < +3. The remaining gain is too small to pay for the TC risk. Record it,
    close the PR, close #908 as measured.
  - **Otherwise, inconclusive:** record it, run nothing more without a new owner call, and do not
    ship. It closes the PR the same way a park does: the branch is deleted, and the code stays
    reachable at `refs/pull/<n>/head`. Under the prior this is likely. For a true gain of +1 to +3,
    the chance of an inconclusive result is about 50–65%.
- **Run B** (only after headroom on A): 10+0.1, `games=26640`, 18 shards, `opening_offset=13320`.
  - **B wins** if its lower bound is > 0.
  - **Otherwise A wins.** The curve has turned, and nothing more aggressive is run here. If the
    owner declines run B, A is the winner.
- **Second TC** (winner vs `merge-base`, `candidate_tc=reference_tc=30+0.3`): `games=8000`, 20
  shards (400 per shard, about ±5.5), `opening_offset` = pairs consumed so far.
  - **Non-inferiority at −8:** the winner ships only if the lower bound is > −8, which needs a
    point estimate above about −2.5. A true 0 passes about 80% of the time, and a true −5 is
    blocked about 80% of the time. This is not a screen for small harm: a true −3 passes about half
    the time. #906's rule (harm only if the whole interval is below 0) is rejected here. It would
    block a true −5 only about 43% of the time, and #906 was checking a shipped default, not
    admitting a new one.
  - Otherwise the result is recorded and nothing ships.
  - D4 applies here too, with time matched at the winner's 10+0.1 ratio ± 0.05: the ratio is
    expected to drift with TC, and a larger drift is reported, not disqualifying.

The 26,640-game runs detect a true +5 about 90% of the time and a true +3 about half the time.
Park at upper bound < +3 means a park reads as "below about +3 here", not "zero". Both looks' nominal
intervals are not simultaneous coverage; the rule is fixed here so it is not chosen after a
result.

## Assumptions I cannot verify from the code

- **A1: the two cuts stack predictably enough to calibrate.** #905 measured U at floor 100. At
  floor 50, settled searches already stop at 50–63% of soft, and U multiplies that. The calibration
  probe measures the stacked time directly. Verified per arm by D3, before each dispatch.
- **A2: local calibration transfers to the lab.** The lab is GCC on 2-core SMT runners; calibration
  is local clang-cl. In #905, local 0.991x landed at lab 0.992x. That is one data point, rechecked
  every run by D4 check 2.
- **A3: 30+0.3 runs inside the 340-min job timeout at 400 games per shard.** The lab has only ever
  run 10+0.1. #905 played 1,660 games per shard in about 280 min, so 400 games at about 3x length
  should take about 200 min. Not verified; the first 30+0.3 dispatch settles it. A timed-out shard
  makes the batch incomplete, so it is discarded and the run re-sized.
- **A4: Threads=1 only.** A uniform cut does not depend on `move_run`, so the helper effect #906
  saw (floor 50 bites harder with helpers) should not amplify U. Not verified. A `Threads=4` harm
  run is optional (Cost).
- **A5: saved clock is spent later.** `compute_budget` recomputes `usable / 30` every move, so
  banked clock raises later budgets. Whether it is spent before the game ends is what D4's
  reported > 8 s share and end clock show.

## Invariants

- At `SoftTimePercent=100` every search is identical to `main`: `p * 100 / 100 == p` exactly.
- The hard limit, the stop latch, `compute_budget` and the changed-move extension are untouched.
- Only clock searches with `soft_scalable` are scaled. `movetime`, `depth`, `nodes`, `infinite`,
  `go infinite` with a clock, and the default-time fallback behave as on `main` at any U.
- The merged binary differs from the measured one only in the default of `SoftTimePercent`.
- The effective percent stays in [1, 100]: floor ≥ 10 and U ≥ 10, so the stop threshold never
  exceeds the soft limit.

## Validation

**Tier:** Engine. It changes stop decisions under a clock, so it is a search change: `search-reviewer`
reviews this document and the diff.

- **Unit (`StratChessTests`):** `SearchTuningTests` gets the option line, the default and range,
  and a `SAME_MEMBER` entry. The `soft_scalable` gate already has its own tests
  (`SearchControlTests`, `SearchLimitsTests`), so nothing new is needed there.
- **Default identity:** `Compare-SearchEquivalence.ps1 -After <worktree build>` passes (identical
  nodes and best moves). The clock path at 100 is identical by exact integer arithmetic.
- **Wiring:** the D3 calibration probe is the wiring check. If the option did not reach the stop,
  the measured ratio would be 1.00 at every U.
- **No bench pass:** one multiply per completed iteration, nothing per node.
- **Elo:** D5's runs. **Needed:** the change exists only to gain strength.
- **Records:** each run gets a `Measurements/ci-per-change.md` row with its ratio, D4 checks and
  forfeit count. The shipped arm gets the D4 clock-band table in
  `Measurements/move-quality-tier1.md`.

## Cost

- **Size:** under 50 lines: `SearchTuning.def`, `AIPerplex.cpp`, `SearchTuningTests.cpp`, plus
  `Docs/Changelog.md` and the measurement rows.
- **Blast radius:** Engine tier. Only the main thread decides iteration stops, so Lazy SMP helpers
  are unaffected. No gates, scripts or skills change.
- **Review:** `search-reviewer` on this document and the diff, plus one code review (170–270k
  tokens).
- **Measurement:** about 1–1.5 h of local calibration per arm. Lab: run A about 4 h; run B about
  4 h (only on headroom); 30+0.3 about 4 h (only before a ship). Each run is the owner's call.
- **Optional, priced alone:**
  - **Time-ratio report in `analyze_move_quality.py`** (about 60 lines plus a self-test, its own
    Scripts-tier PR before run A). This is the fourth ad-hoc computation of the D4 check 2 ratio
    (#103, #905, #906). It covers a hand-computed gate number being wrong.
  - **`Threads=4` harm run (A4):** `threads=4`, `concurrency=1`, `games=6660`, about 3 h, about
    ±6. Tag "(idle-lab candidate)".

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2 option, range, default and why it is uniform (#905) | `SearchTuning.def` entry comment |
| D2 why the scale lives at the stop check, not in `compute_budget` (`go infinite`, floor clamp) | source comment at the `AIPerplex.cpp` multiply |
| D5 results, ratios and D4 checks | `Measurements/ci-per-change.md` rows; Changelog headline |
| D4 clock-band table for the shipped arm | `Measurements/move-quality-tier1.md` |
| D1 untested levers (horizon, increment share) | #908 closing comment; a follow-up issue only if this ships |
