# Issue #237 — search instability: execution status

Living status document for the #237 work sequence. The issue body is the authoritative
specification; this file records **where the work stands, what was decided beyond the issue, and
what the next session should pick up**. Delete it when #237 closes.

Last updated: 2026-08-15.

---

## Standing decisions

These were settled during re-review of the issue against the code at `d5f3520` and are not to be
relitigated without new evidence.

1. **Stages ship as separate PRs, in order, and merge before the next one starts.** Each stage
   changes TT contents, node counts and PVs. Bundling destroys causal attribution for `WAC-287`,
   which is the only thing this issue is actually trying to explain.
2. **Instrumentation is stage 0 and is search-semantics-preserving.** Its correctness gate is
   fixed-depth identity (same best move, score and node count at `Threads=1`), not runtime
   equivalence — per-depth output costs time in clocked searches.
3. **No monotonicity test.** Best move legitimately changes with depth. The instrument exposes the
   transition; a human decides whether a given transition is valid.
4. **`Threads=1` for every diagnostic run.** Lazy SMP makes TT contents nondeterministic, which is
   the opposite of what these measurements need.
5. **Test-data changes are a separate, non-blocking track** (stage 4). They must not be folded into
   a search PR.

## Findings still live

Verified against the checkout; the issue does not mention these. (The `send()` race, the two accept
branches and defect B's arithmetic were here too — all three are now fixed and harvested.)

- **Cumulative node counts diverge from the final line in two ways, not one.** `iterative_deepening()`
  sees only `td.nodes_searched` (main thread), while the final `info`/`bestmove` line reports the
  cross-thread total assembled at `AIPerplex.cpp:256-259` — that is the Lazy SMP divergence. The
  second one bites even at `Threads=1`: a clocked search typically starts depth N+1, is interrupted,
  and is rejected by `assess_iteration_quality()`, so `REJECT_AND_STOP` emits nothing while those
  nodes are already counted. The final line is therefore strictly **greater** than the last
  per-iteration line on essentially every timed search. Compare with `<=`, never `==`.
- **The absolute-ply bound stage 2 asks for is genuinely absent, and the exposure is smaller than it
  looks.** `pvs()` indexes `td.killers[ply]` and `td.last_move_was_null[ply+1]` unguarded, and both
  are `[MAX_PLY]` (256, `defines.h:101`). `quiescence()` touches neither, and `PVTable` guards every
  ply access (`PVTable.h`). So removing the `MAX_QSEARCH_DEPTH` cap for in-check nodes risks stack
  exhaustion on a perpetual-check line rather than an out-of-bounds write. Bound it anyway.

## Tactical test semantics — decided

A tactical test asserts **any objectively equivalent decisive continuation**, not the puzzle key
move. That is a change from the current suite, which asserts the key move.

Consequences for stage 4:

- Accepted-move lists are widened from **external** analysis only — exact tablebase results where
  the position is within reach, a strong external engine otherwise. Never from what the current
  engine happens to score equally; that would make the suite assert the engine's own eval.
- The widening is evidence-backed per position, recorded with the source, and lands as its own PR.
- `WAC-043`'s alternatives and `QFORK-001`'s five tablebase-winning moves are the first candidates.
  Note the suite already had a two-best-options precedent in the `QFORK-001` handling.

**The evidence source is the bottleneck, and it showed up immediately.** Applied to the first two
positions, the rule widened nothing: `WAC-043`'s `h2h4` was disproven, and `WAC-065`'s `h6f8` was
unverifiable because Lichess cloud eval only answers for positions someone has already analysed —
neither the position itself at `multiPv=5` nor the position after the move is cached. A full-suite
audit under these semantics therefore needs a local strong engine as the evidence source. That is a
tooling decision, and it should be settled before the audit issue is opened rather than discovered
34 positions in.

**Deferred, not rejected:** tagging positions by intent (key-move tests vs. don't-blunder tests) and
asserting differently per tag is the more accurate model, but it is a test-framework change on top
of the data change. Raise it as its own issue rather than folding it into stage 4.

## Correction to the issue body: `WAC-043`'s historical failure is not overstated

The issue says WAC-043's alternatives are "winning, although by different margins", and treats its
historical depth-8 failure as an artefact of an over-narrow accepted list. External measurement
(Lichess cloud eval, depth 20) says otherwise:

| move | cp | in `best_moves` |
|---|---:|---|
| `d5a8` | +3715 | yes |
| `a3e7` | +3567 | yes |
| `a3b2` | +2358 | no |
| `h2h4` | +793 | no — this is the depth-8 competitor |
| `h2h3` | +683 | no |

`h2h4` is winning in the sense that any engine converts +793, but preferring it over `d5a8` throws
away roughly 2900 cp. Under the decided semantics that is a genuine search error, not a second
solution. So `WAC-043` correctly stays out of the causal reproduction set — it does not currently
regress — but its historical failure should not be described as a test-data artefact. Update the
issue body accordingly.

## Split out: #299, abort returns a valid score

Found while reviewing stage 0, filed separately because it is **not** reachable from #237's
reproduction: `WAC-287` is swept at fixed depth with no clock, so no abort ever fires.

`pvs()` and `quiescence()` return `GameValues::Draw` on abort — the same value `check_draws()`
returns for a genuine repetition/50-move draw — and neither has an abort check between its move loop
and its `tt.store()`. Contaminated values therefore reach the shared table at full nominal depth and
outlive the search. Under time control only, which is why no fixed-depth test catches it.

Keep it out of stages 1-3. Those are deliberately one-defect-at-a-time so that each one's effect on
`WAC-287` is attributable; #299 changes TT contents under time control and would destroy that.

## Also split out: the `stop`-before-`start` race

Surfaced when stage 0's concurrency test hung deterministically. `TimeManager::start()` stores
`should_stop_ = false` (`TimeManager.h:29`); `ApplyLimits()` calls it from inside `GetMove()` on the
**search thread** (`AIPerplex.cpp:203`); `stop()` is callable from any thread. A `stop` arriving
between the search thread being spawned and its reaching `ApplyLimits()` is therefore silently
erased, and with `go infinite` (`limits.depth = 50`) the search never ends — `stop_and_join()` blocks
in `join()` forever. `TimeManager.h:15-16` documents the ordering requirement without enforcing it.

Stage 0's test closes the window with a 200 ms sleep and says so in its comment. **That sleep is a
marker, not a fix** — when the race is fixed, delete it and let the test exercise the real window.

## Stage 2 measurement — decided: bench + SPRT

Approved 2026-08-15. Concrete parameters, so the run does not have to be re-litigated when stage 2 is
ready:

**Bench.** `Run-Bench.ps1 -Exe <path>` before and after. The "before" binary must be built from the
merge base *before* the working tree is touched — `run-tests` does not rebuild the exe, so a
forgotten rebuild silently benchmarks the new binary against itself. Compare **nps**, never node
counts: stage 2 changes what the search visits by design, so node counts are expected to move and
carry no information here.

**SPRT.** `-Sprt NonRegression` (elo0 = -5, elo1 = 0 — "prove it did not make things worse") against
`-ReferenceExe <merge-base build>`. Not against the tag anchor: `Run-EloMatch.ps1` refuses `-Sprt`
there on purpose, because an anchor tests the cumulative sum rather than this change.

**Opening book — already solved, but not by default.** `C:\Users\thees\source\repos\EngineTesting\`
holds `8moves_v3.pgn`: **34 700 openings = 69 400 distinct games**. The auto-resolver only picks up
files matching `openings-large.*`, so as it stands a run falls back to the committed 250-opening
smoke book and starts replaying openings after game 500 — which narrows the error bar without adding
information. Either copy it to `EngineTesting\openings-large.pgn` once, or pass
`-Book "C:\Users\thees\source\repos\EngineTesting\8moves_v3.pgn"` explicitly. At 69 400 distinct
games the book stops being a constraint at any N this project would run.

**`-Games` must be raised well above the 500 default.** Under SPRT it is only a give-up point, and
raising it is statistically free — it costs wall-clock only in runs that would otherwise return
inconclusive. Leaving it at 500 buys nothing and risks an avoidable "inconclusive".

**Operational ceiling.** ≈40 min per 500 games at `-Concurrency 6`. A *background-launched* match
tops out around 700-750 games before hitting the execution tooling's duration cap, so a
multi-thousand-game SPRT must run in the foreground or use resume. Do not raise `-Concurrency` to
buy throughput — it is pinned to physical cores, and oversubscription injects time losses, which
discard the batch.

**If it comes back expensive, widen the bounds before buying games.** Expected sample size scales
with the inverse square of the indifference region's width, so `-Sprt Custom -Elo0 -10 -Elo1 0` costs
roughly **4x fewer games** than `NonRegression`'s `[-5, 0]`. Ask the loosest question that still
settles the decision.

**Expect a possible small negative.** Stage 2 adds nodes by design — legal in-check qsearch does
strictly more work per node. A measured slowdown is acceptable only with a stated benefit that
outweighs it; the benefit claimed here is correctness (qsearch currently evaluates positions as
though the side to move may decline to leave check). If the SPRT accepts H0, that is a real result to
record, not a failure to re-run until it passes.

## Open questions for the user

None outstanding.

---

## Stage board

| # | Stage | Kind | Status | Branch / PR |
|---|---|---|---|---|
| 0 | Per-iteration UCI `info` + `send()` serialisation | semantics-preserving | **merged, PR #300** | — |
| 0b | `WAC-287` baseline sweep, depths 4-12 | evidence | **done, recorded below** | `worktree-237-wac287-baseline` |
| 1 | Terminal-node TT storage (defect B) | search change | **merged, PR #304** | — |
| 2 | Legal qsearch while in check (defect A) | search change | design pinned, implementing | `worktree-237-stage2-legal-qsearch-in-check` |
| 3 | Qsearch TT depth → remaining budget (defect C) | search change | not started | — |
| 4 | Tactical test-data semantics (`WAC-043`, `WAC-065`) | test data | **merged, PR #298** | — |

Stage 4 is scoped to the two positions #237 names. Adopting the new semantics implies the other 34
positions were written under the old one and may also carry narrow lists, but a full-suite audit is
its own issue — widening a corpus piecemeal inside a search-debugging issue is how test data drifts.
Stage 4 also corrects `Docs/TestDesign.md`, which still lists `WAC-287` in the suite (count of 6)
after its removal from `Tests/tactical_test_cases.json`.

### Stage 0 — instrumentation (merged; design harvested into the code)

One decision survives because later stages have to work around it:

- **The observer ships enabled for every UCI `go`, including rated play.** `Run-EloMatch.ps1` drives
  UCI, so a candidate built from this repo pays the per-iteration emit cost inside its search budget
  while the pinned `elo-reference-v2` does not. Kept always-on because per-depth `info` is standard
  UCI, and the bias runs *against* the candidate — it understates gains rather than inventing them.
  Sizing is order 0.2%, below the noise floor of any batch we can afford. Revisit only if a stage
  needs byte-identical timing against the reference.

### Stage 0b — baseline

Two instrument quirks to know before reading any of this as evidence:

- **A `mate +/-9999` reading is not a mate score.** `format_uci_score()` treats any
  `|cp| >= Mate_Threshold` as mate, and `Search_Init` (50000) exceeds it, yielding
  `plies = 30000 - 50000 = -20000` and `mate -9999`. The root score should never actually be
  `+/-Search_Init` — `adjustScoreForGameState()` converts terminal nodes before returning — so this
  is latent rather than live, and it was left alone as out of scope for a semantics-preserving
  stage. If it ever appears, it is a sentinel leak, not a mate.
- **An interrupted-but-accepted iteration can carry a contaminated score.** `pvs()` returns
  `GameValues::Draw` on abort, and `assess_iteration_quality()` only catches the resulting bogus 0
  when `|state.best_score| > score_draw_threshold`. This is pre-existing and already reached the
  final `info` line; the instrument merely makes it visible one line earlier. A clocked search
  showing a plausible score at depth N and a suspicious 0 at depth N+1 is displaying this, not a new
  bug. Use fixed depth for evidence runs and it cannot arise.

#### The baseline

Recorded 2026-08-15 on `b72f361` (`origin/main`, stage 0 merged). Release clang-cl,
`Threads=1`, `Hash=192`, position
`rn3k1r/pp2bBpp/2p2n2/q5N1/3P4/1P6/P1P3PP/R1BQ1RK1 w - - 0 1`. **This is the comparison basis for
stages 1-3.**

| depth | move | score | nodes | PV |
|---:|---|---:|---:|---|
| 1 | `g5h7` | +266 | 43 | `g5h7` |
| 2 | `h2h4` | +102 | 305 | `h2h4 a5a2` |
| 3 | `c1d2` | +290 | 3 053 | `c1d2 a5b5 g5h7` |
| 4 | `d1h5` | +215 | 16 303 | `d1h5 e7d6 g5e6 f8e7` |
| 5 | `d1h5` | **+518** | 23 624 | `d1h5 e7c5 g5e6 f8e7 h5c5` |
| 6 | `d1h5` | +196 | 87 912 | `d1h5 e7b4 g5e6 f8e7 h5a5 b4a5` |
| 7 | `d1h5` | **+495** | 115 875 | `d1h5 e7c5 g5e6 f8e7 h5c5 a5c5 e6c5` |
| 8 | `f7e6` | +189 | 937 049 | `f7e6 a5c3 a1b1 b8a6 g5e4 c3a5 e4f6 e7f6` |
| 9 | `d1h5` | **+438** | 1 300 926 | `d1h5 e7c5 g5e6 f8e7 c1g5 c5d4 e6d4 h7h6 a1e1` |
| 10 | `d1h5` | +245 | 1 702 695 | `d1h5 e7c5 g5e6 f8e7 h5c5 a5c5 e6c5 e7f7 c5b7 b8d7` |
| 11 | `d1h5` | **+502** | 2 232 309 | `d1h5 a5g5 h5g5 f8f7 g1h1 g7g6 f1f6 e7f6 g5f4 b8d7 f4f6` |
| 12 | `d1h5` | +268 | 3 592 464 | `d1h5 e7c5 g5e6 f8e7 h5c5 a5c5 e6c5 e7f7 c5b7 b8d7 c1d2 h8e8` |

#### What the baseline changes about the diagnosis

**The depth-8 move is a symptom; the parity swing is the phenomenon.** Scores separate almost
perfectly by depth parity:

- odd (5, 7, 9, 11): +518, +495, +438, +502 — mean ≈ **+488**
- even (4, 6, 8, 10, 12): +215, +196, +189, +245, +268 — mean ≈ **+223**

A ≈265 cp oscillation, and it is **not converging**: the gap at depth 11→12 (+502 → +268) is as wide
as at 5→6. `d1h5` is chosen at every depth from 4 to 12 except 8, and the move that displaces it
scores +189 — squarely inside the even-depth band, not an outlier. At depth 8 the compressed
even-depth scores simply reordered.

So "deeper search returns a worse move at depth 8" is the visible tip of a systematic even/odd
horizon effect. **Stages 1-3 should be judged on whether the parity swing narrows, not on whether
depth 8 flips back.** The swing is a continuous, far more sensitive metric; the depth-8 flip is one
noisy bit.

**The disputed line runs through a check.** The recurring PV fragment `g5e6 f8e7` is `Ne6+` followed
by a king evasion — a knight on e6 attacks f8. This is the first *direct* link between `WAC-287` and
defect A: quiescence permits stand-pat while in check and generates only captures, so it cannot see a
quiet king evasion. The main line of this position repeatedly puts the side to move in check near the
horizon, which is exactly where that defect bites. Still not proof, but far stronger than the
plausibility argument in the issue body.

#### Method note: fresh-process-per-depth is unnecessary

Per-iteration output at depths 1..N is **byte-identical** whether produced by a fresh `go depth N` or
read out of a single deeper run — same scores, same PVs, same node counts. Verified across the
separate depth-4..10 runs and the depth-12 run.

That is expected (a fresh process at depth N still runs iterations 1..N, building the same TT the
same way), and it confirms the search is fully deterministic at `Threads=1`. Practical consequence:
**one `go depth 12` reproduces this entire table.** Stages 1-3 can capture their post-change sweep in
a single run instead of nine, and any divergence in the 1..12 sequence is a real effect rather than
run-to-run noise.

### Stage 1 — terminal-node TT storage: done, and `WAC-287` did not move

Branch `worktree-237-stage1-terminal-tt`, commit `79473d1`. `pvs()` now resolves the
checkmate/stalemate score before it classifies and stores the node, and writes it as EXACT with an
empty move; the `-Search_Init` sentinel can no longer be narrowed into the table.

Post-change sweep, same position and settings as the baseline above: **identical selected move,
score and PV at every depth 1-12.** Only node counts moved, and only downward — 0 at depths 1-5,
then −40 (d6), −40 (d7), −148 (d8), −427 (d9), −713 (d10), −1 795 (d11), −3 502 (d12: 3 592 464 →
3 588 962, −0.1%). The parity swing is unchanged at ≈265 cp and depth 8 still returns `f7e6`.

Bench, depth 12, two runs each: before 2 956 513 / 2 970 266 nps, after 2 987 626 / 2 975 507 nps —
+0.6%, inside the run-to-run spread. Total bench nodes 34 478 850 → 34 477 205 (−0.005%).

**No SPRT was run, and one is not worth buying.** Nothing the search decides changed on either
corpus, and a 0.1% node effect is two orders of magnitude below what any affordable batch resolves.
The stage stands on the regression tests plus the invariance of the sweep. This is the outcome the
document predicted; defect B was worth fixing because it is wrong, not because it explains
`WAC-287`.

**Consequence for stages 2-3: the baseline table above is still the comparison basis.** Stage 1 left
it untouched, so a stage-2 or stage-3 sweep can be read straight against it.

From the `search-reviewer` pass (LGTM, no blocking findings):

- **The old bound was inert except in mate-range windows, and that is where it did damage.** +15536
  as an UPPER bound only collapses a window whose `alpha` already exceeds it — which is exactly the
  aspiration window `search_with_aspiration()` opens after a mate score is seeded. There the probe
  returned +155 pawns for a mated or stalemated position. That regime, not general search, is where
  any Elo lives; a general game batch cannot resolve it, a mate-heavy suite could. This is now
  pinned by the probe-side test.
- **The new terminal store is the one `tt.store()` in `pvs()` that is provably abort-immune**, so
  #299's discard-on-abort guard must leave it exempt rather than wrap it. Written up on #299 itself
  (comment), so it survives this file.
- **The ply-100 mate-normalisation cliff is now #305.** Pre-existing, latent, and no longer this
  document's to carry.

### Stage 2 — legal qsearch while in check: design, pinned before implementation

All of it lives in `AIPerplex::quiescence()`. Read against `origin/main` @ `97340a5`.

**The shape.** `in_check` moves to the top of the function, above the `MAX_QSEARCH_DEPTH` cutoff.
In check the node becomes a small full-width search: no stand-pat, all legal evasions, no delta
pruning. Out of check nothing changes at all.

| | out of check | in check |
|---|---|---|
| stand-pat baseline / `>= beta` cutoff | as today | **suppressed** — illegal to decline a move |
| `Eval->Evaluate()` | as today | **not called** — its only consumers are stand-pat and delta pruning |
| move generation | `ComputeCaptures` | `ComputeLegalMoves` (pseudo-legal; `DoMove` filters) |
| delta pruning | as today | already disabled today, unchanged |
| `!moveFound` | "no captures" — *not* stalemate, as today | **checkmate**: return `-Mate + ply`, store EXACT + empty move |
| `MAX_QSEARCH_DEPTH` cutoff | as today | bypassed; bounded by absolute ply instead |

**Decisions that could have gone the other way:**

1. **The absolute bound is a pure safety net at `MAX_PLY`, not an in-check extension budget.** The
   issue asks for "an explicit absolute-ply safety bound"; a *tuned* limit on consecutive check
   extensions is a strength decision and would confound this stage's measurement. At the bound the
   node returns the static evaluation even in check — that is the one place the issue's "never
   return a static eval while in check" has to yield, and it is why the bound must be somewhere a
   real search cannot reach.
2. **The recursion is bounded by material, not by the ply cap.** Worth stating because it looks
   unbounded: a side that is *not* in check still generates captures only, so the chain can only
   continue through **capturing checks**, which run out. Quiet perpetual check cannot occur inside
   qsearch. This is what makes bypassing `MAX_QSEARCH_DEPTH` affordable, and it is the assumption to
   re-check if the node cost comes back worse than expected.
3. **Evasions keep `qsearch_depth + 1`.** Not counting them against the budget is the other common
   choice and risks explosion; counting them is the smaller change.
4. **Evasion ordering is left as `SortMovesByValue` (MVV-LVA).** On a mixed list quiets score
   `-piece/16`, so king moves — often the only evasion — sort *last*. That is poor ordering and
   costs nodes, but a new evasion heuristic in this PR would confound the node/Elo attribution the
   whole issue exists to protect. **Follow-up candidate, not this stage.**
5. **TT phase and depth are untouched** (`QUIESCENCE`, `qsearch_depth`). Their semantics are stage
   3's; fixing both at once would destroy attribution.

**Already true, so not part of this change:** delta pruning is gated on `!in_check` today, and
castling-out-of-check is already excluded because `AddCastleMoves` includes the king's origin square
in its attack mask.

**Watch during implementation:** `const GameInfo& info` at the top of `quiescence()` is a *reference*
into `td.info_seq`, which `add_move_to_seq()` can reallocate. It is safe today only because `info` is
read before the move loop and never after. Do not introduce a use of it after the loop.

### Stage 3

Specified in the issue body. Needs: a focused regression test for the exact invariant, the fix alone,
the post-change sweep against the baseline table, the full suite, a `search-reviewer` dispatch, and
bench plus SPRT (parameters pinned above). It is a rename-and-invert with a narrow blast radius.

Stage 2 is the one with a mechanism for `WAC-287`'s behaviour — supported by the baseline's `Ne6+`
finding, not just by argument — and the one that costs nodes.

Judge each stage on the **parity swing** (odd-depth mean minus even-depth mean, ≈265 cp at baseline),
not on whether depth 8 flips back to `d1h5`. The swing is continuous and sensitive; the depth-8 move
is one bit of a compressed ordering and can flip for reasons unrelated to the fix under test.

---

## Resuming in a later session

1. Read this file and the #237 issue body.
2. `Get-Worktrees.ps1` for drift and PR state.
3. Pick up the first stage on the board that is not merged; the stages are strictly ordered except
   stage 4.
4. Re-check assumptions against `origin/main` before executing — this document ages.
