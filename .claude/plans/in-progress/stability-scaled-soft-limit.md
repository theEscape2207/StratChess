# Stability-scaled soft limit — Design

**Issue:** #103 (evidence: spike #886)

## Goal

Under a clock, the engine spends the same soft budget on a move whether its choice has been settled
for six depths or has just flipped. The only adaptive rule today is one extra depth after a changed
move (`continue_iteration`). Spike #886 showed on stock `main` that how long the root move has
held predicts whether the next depth will change it. The change rate falls from about 30% after
one depth to 5–7% after six or more (AUC 0.71–0.74 in two halves of `Tests/profile-screen.fen`;
[#103 comment](https://github.com/theEscape2207/StratChess/issues/103#issuecomment-6341547178)).
Stopping earlier on settled moves leaves time on the clock. `compute_budget` recomputes from the
remaining clock every move, so later moves get that time back. Whether this gains Elo is unknown
until the lab measures it.

## Review focus

- **D2 and D6, the 50% floor and what the gate can tell:** the decision most likely to be wrong.
  - The floor is chosen from fixed-depth traces, not tuned, and one lab arm tests one value.
  - About two-thirds of moves get scaled, so a pass shows the policy gains Elo, not that the
    signal beats a uniform cut. Check that this is enough to ship.
- **Assumption A1:** the warm-TT game probe says the signal carries over to real game searches.
  It is still fixed-depth, not clock-limited. If it does not hold under the clock, the change costs
  strength in exactly the positions it rushes.
- **D4:** the scale must reach clock-derived budgets only. If it leaks into `movetime`, `depth`,
  `nodes` or `infinite`, those searches change behaviour silently.

## Scope

**This change will:**

- count, in `Engine::IterationState`, how many consecutive completed depths have chosen the same
  root move;
- scale the soft limit by a percentage derived from that count, for clock-derived budgets only;
- expose the floor as the UCI option `StabilityTimeFloor`, shipped at 100 (off) until the lab
  rules;
- run one strength-lab arm, and flip the default to the tested value only if the lab passes (D6).

**This change will not:**

- extend time beyond the soft limit for unstable moves. The changed-move extension stays exactly
  as it is: two variables at once would answer neither question;
- touch `compute_budget`, the hard limit, the 1.5× hard/soft ratio or the overhead;
- use score deltas, node growth or depth. #886 measured these as uninformative (AUC 0.49–0.55)
  or no better than the move run;
- add root node-share or second-best-margin signals. Both need instrumentation and would be their
  own spike;
- predict whether the next depth can finish before it starts.

## Decisions

### D1: The signal is the move run length

`IterationState` gains `int move_run = 0`. `assess_iteration` updates it on `COMPLETED` only: it
becomes `previous.move_run + 1` when the completed move equals `previous.last_iteration_move`,
otherwise 1. `ACCEPTED_INTERRUPTED` and `REJECTED` leave it unchanged, as they already do for
`last_iteration_move`. An interrupted search is ending anyway.

There is no discount for TT-replayed depths. In a game the TT is warm, so the early depths replay
almost free and the run is long before any expensive depth completes. The warm-TT probe (A1)
found the signal still predicts on the depths that do real work, so the raw count is kept.

Rejected: #886's original signal (3 same moves with score steps ≤ 20 cp). Its score clause
contributed nothing (AUC 0.60/0.64 against 0.63/0.66 without it). Also rejected: a decayed count
of move changes. It ranks the same as the run length (0.74/0.70 against 0.74/0.71) and needs a
floating-point accumulator.

### D2: Schedule: full budget through run 2, linear to the floor at run 6

`Engine::soft_limit_percent(int move_run, int floor_percent)`, a pure function in
`IterationPolicy`:

| move_run | ≤ 2 | 3 | 4 | 5 | ≥ 6 |
|---|---|---|---|---|---|
| percent | 100 | 100 − (100−F)/4 | 100 − (100−F)/2 | 100 − 3(100−F)/4 | F |

Integer arithmetic: `100 - (100 - F) * (min(run, 6) - 2) / 4` for run > 2. At F = 50 that gives
100, 100, 88, 75, 63, 50.

Run 1 and 2 keep the full budget, because their change rates (30% and 13–28%) are the
highest-risk band. Run ≥ 6 is where the rate flattens at 5–7%. **F = 50** is the tested value: a
settled move still gets half its normal budget. A step function (100 → F at run 6) was rejected
because rates fall steadily from run 3 to run 5 (11–16%), and a cliff wastes that gradient.
Rejected for now: tuning F by sweep. That is optional and priced below.

### D3: Where the scale is applied

`chess::TimeManager::should_stop_iteration(int soft_percent = 100)` compares elapsed time against
`soft_limit_ * soft_percent / 100`. That stays ≤ `soft_limit_` ≤ `allocated_time_`, so
`TimeBudget`'s invariant holds. The stop latch still wins.
`SearchControl::ShouldStopIteration(int soft_percent = 100)` takes the percentage and forwards it.
The default keeps the existing callers (`SearchControlTests`, `TimeManagerTests`) unchanged. In `iterative_deepening` the driver computes it from
`assessment.next_state.move_run` and `tuning_.stability_time_floor` before calling
`continue_iteration`, so the changed-move extension sees exactly what it saw before. A changed
move has run 1, so it always gets 100%. `continue_iteration`'s signature and contract are
unchanged.

Rejected: rescaling the soft limit inside `TimeManager` after each depth. That would mutate the
timer from the iteration driver and break `TimeManager`'s single-writer `start()` contract.

### D4: Clock-derived budgets only

`Engine::ResolvedLimits` gains `bool soft_scalable`, which is true only on the `limits.clock`
branch of `resolve_limits` and only when `!limits.infinite`. `go infinite wtime …` sets both
fields and already takes the clock branch (`UCIHandler.cpp:381-393`, `SearchLimits.cpp:58`). That
the clock already budgets such an analysis is a pre-existing quirk; this change must not shorten
it further, and fixing it is out of scope. `SearchControl::ApplyLimits` stores it, and `ShouldStopIteration`
forces 100% when it is false. `movetime` has soft == hard and means "use this time". Depth, node
and infinite searches use the one-hour sentinel, so scaling would turn an `infinite` analysis
into a timed stop at 30 minutes. The default-time fallback is a fixed budget too.

### D5: `StabilityTimeFloor`: UCI spin 10–100, JSON-bound, default 100

One `TUNING_FIELD(int, stability_time_floor, 100, 10, 100, true, "StabilityTimeFloor", true)`
entry. At 100 every percentage is 100 and `soft_limit_ * 100 / 100 == soft_limit_` exactly, so the
default is behaviour-identical. The minimum of 10 rejects floors that would stop nearly every
settled search at its next completed depth: values no lab arm would test.

### D6: Lab rule, declared before the run

The run: `strength.yml` on the PR branch, `reference_ref=merge-base`,
`candidate_uci_options="StabilityTimeFloor=50"`, at the lab's standard 10+0.1 and `Threads=1`.
Each run is about 20k games, giving a 95% interval of roughly ±4 Elo. The domain reviewer's prior
is +3 to +12, so a true +3–4 sits right at the gate. The rules:

- **Pass:** the 95% interval's lower bound is above 0. The same PR then sets the default to 50
  before merge. The merged binary differs from the tested one only in that default.
- **Positive but inconclusive:** the point estimate is above 0 but the interval includes 0. One
  confirmation run, pooled with the first; the pooled interval then decides as above. That second
  run is the owner's budget call; without it, the result counts as a fail. It keeps the first
  run's candidate and reference SHAs, options, time control and toolchain. Its one deliberate
  change is fresh openings: `opening_offset=9990` (run one consumed openings 1–9,990 as 9,990
  pairs; use the actual consumed range if the game count differs). Only complete, valid batches
  of that exact comparison are pooled; a source change makes a new experiment. The two looks'
  nominal 95% intervals are not simultaneous coverage; the rule is fixed here so it is not
  chosen after the first result.
- **Fail:** the point estimate is ≤ 0, or the pooled result does not pass. The PR does not merge.
  The result goes to the ledger, and the code is deleted, not shipped dark (the end state of a
  search feature is on by default or deleted). A failed F = 50 does not rule out F = 70. Testing
  70 would be a new decision for the owner, from the ledger row, not an automatic next arm.
- **Discarded:** a batch reporting a time loss is discarded, never reported. A forfeit is a
  blocking finding, whatever the Elo.

**What the gate answers.** In games about two-thirds of searches reach run ≥ 6 at their first
real-work depth (A1), so F = 50 acts on most moves. A pass shows that the policy gains Elo. It
does not show that the stability signal beats a uniform soft-limit cut of the same mean size.
That attribution is optional and priced below. Shipping does not depend on it.

**Predicted time use (A3).** The scaled limit only gates whether the next depth starts, and the
branching factor is about 2. So a scaled move saves between 0 and 50% of its time, and part of
that saving is depths that today run into the hard limit and are wasted. Prediction: over moves
9–40, the candidate's mean time per move is 0.75–0.95× the reference's. This is a diagnostic
forecast, not a validity test: the PGN mean also mixes in clock balances that differ between the
sides, depth granularity and which games reach move 40. A ratio outside the range is
investigated, not grounds to discard the result. Whether the option acted is settled before the
lab by the wiring check (Validation).

## Assumptions I cannot verify from the code

- **A1: transfer.** #886's traces are fixed-depth and cold-TT, on move-9 book exits plus 24
  endgames. **Partly verified by a warm-TT probe, 2026-10-22.**
  - Method: one side of each of 12 games from lab run `39689240396` shard 0, replayed move by
    move up to ply 80 (465 searches). Each side ran in one persistent process: no `ucinewgame`
    between its moves, `Hash=16`, `Threads=1`, `go depth 14`.
  - Result: the move changes at d+1 in 32% of cases at run 1, 20% at run 2, 18% at run 3, 9% at
    run 4, 6% at run 5 and 4% at run ≥ 6. That counts only depths that did ≥ 2% of the search's
    final work (1,741 pairs); over all of d8–13 the rates are the same within 1–2 points. The
    cold-TT rates were 30% and 5–7% at the two ends, so the signal holds, slightly sharper.
  - Runs are inflated, as the domain review predicted: 59% of searches are already at run ≥ 8
    at their first real-work depth.
  - Still unverified: fixed depth is not the clock, and the opponent's moves come from the
    recorded game. The lab decides.
- **A2: saved time buys depth later.** The redistribution mechanism is verified in code:
  `compute_budget` spreads `usable / 30 + 0.8 × increment` from the remaining clock, recomputed
  every move. Whether the extra time per later move yields enough depth at 10+0.1 to pay for the
  shortened searches is exactly what the lab measures. Not verified.
- **A3: the lab plays the clock path.** At 10+0.1, fastchess sends `wtime`/`btime`, so the
  budget comes from `limits.clock` and D4 lets the scale apply. The lab fails fast if the option
  is not advertised. That proves the option exists, not that it acted. That it acts on the clock
  path is checked before the lab by the wiring check (Validation). The PGN time ratio after the run
  is a diagnostic only (D6).
- **A4: `Threads=1` only.** With helpers, the main thread completes depths faster through the
  shared TT. Runs then grow faster and the scale bites harder. `Threads=4` is untested and
  stays so in this change.

## Invariants

- At `StabilityTimeFloor=100`, every search is identical to `main`: same nodes, same moves, same
  stop decisions.
- `soft_limit_percent` returns a value in [F, 100], never increasing in `move_run`, and 100 for
  `move_run ≤ 2`.
- Only clock-derived budgets are scaled. `movetime`, `depth`, `nodes`, `infinite` and the default
  time behave as on `main` at any floor.
- The hard limit, the stop latch and the changed-move extension are untouched. A changed move
  always gets 100%.
- `go infinite` with a clock is never scaled.
- `move_run` is per search, main thread only, and reset by constructing `IterationState`, as the
  other fields already are.

## Validation

**Tier:** Engine. It changes stop decisions under a clock: a search change, so it gets a
`search-reviewer` pass on this document and on the diff.

- **Unit (`StratChessTests`):**
  - `IterationPolicyTests`: `move_run` increments on a repeated completed move, resets to 1 on a
    change, and is untouched by interrupted and rejected samples. The `soft_limit_percent` table
    for F = 50, 100 and 10, plus monotonicity over runs 0–20.
  - `TimeManagerTests`: `should_stop_iteration(50)` fires at half the soft limit; `(100)` matches
    today.
  - `SearchLimitsTests`: `soft_scalable` is true only for clock limits without `infinite`.
  - `SearchControlTests`: with `movetime` limits, `ShouldStopIteration(10)` does not fire at a
    tenth of the budget; with clock limits it does.
- **Wiring check (before the lab, local, scratch script):** the clock path really reaches the stop
  decision. 40 positions from `Tests/profile-screen.fen`, one fresh process each,
  `go wtime 10000 btime 10000 winc 100 binc 100`, at `StabilityTimeFloor=100` and `=50`, under
  `measure-strength`'s quiet-window rule (a few minutes of engine time). Only the floor differs, so
  the F = 50 searches must use less time on aggregate and stop at a shallower completed depth on
  at least a quarter of the positions. Report the counts and denominators. A failure blocks the
  lab: it means the multiplier does not reach the stop.
- **Default identity:** `Compare-SearchEquivalence.ps1 -After <worktree build> -BaselineRef 0c434b0d` passes (identical
  nodes and best moves). That covers the fixed-depth paths. The clock path's identity at 100
  follows from exact integer arithmetic and is pinned by the `TimeManagerTests` case.
- **No bench pass:** the change adds one integer update and one multiply per completed iteration,
  and nothing per node, so there is no nps question to answer.
- **Elo:** one lab run under D6's predeclared rule. **Needed:** this change exists to gain
  strength, and only the lab can show it.
- **Time use (diagnostic):** after the lab, compare the mean recorded time per move between
  candidate and reference from the run's PGNs, over moves 9–40 and by game phase, against D6's
  predicted 0.75–0.95×.
- **Allocation (retained candidate only, #103's acceptance):** run the Tier 1 move-quality scan
  (`Scripts/analyze_move_quality.py`) on the lab PGNs: pre-move clock and phase distributions,
  blunder rates in contested positions only, split by side. Recorded in
  `Measurements/move-quality-tier1.md`. It shows whether the candidate left the low-clock band
  that motivated #103. It explains allocation and is not a ship threshold. Existing PGNs only, no
  new match.

## Cost

- **Size:** 50–200 lines, about 40 of engine code and 80–120 of tests. Files: `SearchTuning.def`,
  `IterationPolicy.h/.cpp`, `SearchLimits.h/.cpp`, `SearchControl.h/.cpp`,
  `Utils/TimeManager.h`, `AIPerplex.cpp`, four test files, `Docs/Changelog.md` and a
  `Measurements/ci-per-change.md` row.
- **Blast radius:** Engine tier. Lazy SMP helpers are unaffected, because only the main thread
  decides iteration stops. No gates, skills or scripts change.
- **Review:** `search-reviewer` on this document and on the diff, plus the code review
  (170–270k tokens, 3–5 min per run).
- **Lab:** one run, about 3 h, holding 18 of 20 CI slots. The owner's call.
- **Optional, priced alone:**
  - **Confirmation run (D6, positive but inconclusive):** about 3 h, only on that outcome.
  - **Uniform-cut control (attribution):** a temporary option scaling every clock search's soft
    limit by a fixed percentage. Set it to the candidate's measured time ratio and run one lab arm
    against the same reference: about 10 lines and about 3 h. It answers whether the stability
    signal beats a plain cut of the same size. It does not gate shipping. The option is deleted
    afterwards.
  - **`Threads=4` confirmation (A4):** one lab run before the default flips, tagged
    "(idle-lab candidate)": `threads=4`, `concurrency=1` (the workflow requires it), `games=6660`,
    18 shards. About 3 h, with a wider interval of roughly ±7 Elo. The full 20k-game sample would
    take three such batches, about 9 h of lab.
  - **Second arm, F = 70:** via `candidate_arms` it halves each arm's precision. As a separate
    run it costs another ~3 h. It covers D2 being wrong in the too-aggressive direction. Screen
    winners shrink on confirmation, so a split-arm winner would need a confirming run anyway.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2 schedule, why run ≤ 2 keeps 100%, #886 evidence | source comment on `soft_limit_percent` |
| D1 update rule (completed only) | `IterationPolicy.h` contract comment beside `assess_iteration` |
| D4 clock-only scaling, and why `infinite` must not scale | source comment on `ResolvedLimits::soft_scalable` |
| D5 option, range and default | `SearchTuning.def` entry comment; `Docs/Changelog.md` |
| D6 lab result, time-per-move comparison | `Measurements/ci-per-change.md` row; Changelog headline |
| Allocation diagnostic (retained candidate) | `Measurements/move-quality-tier1.md` profile |
| Signal comparison (score, node growth, depth uninformative) | already on #103; cited from the source comment |
