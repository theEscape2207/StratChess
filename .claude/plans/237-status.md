# Issue #237 — search instability: execution status

Living status document for the #237 work sequence. The issue body is the authoritative
specification; this file records **where the work stands, what was decided beyond the issue, and
what the next session should pick up**.

Last updated: 2026-08-16.

---

## Read this first: #306 answered the issue, and #312 invalidated the numbers

**#237 is closed** (COMPLETED, 2026-08-15 — auto-closed by stage 1's PR #304). Its headline question
is genuinely answered, but by **#306** rather than by any of the three defects it named:
`MoveGenerator::GenerateOfficerMoves()` masked the capture set with the mover's *own* pieces, so
`ComputeCaptures()` never produced an officer capture and quiescence had only ever resolved pawn
captures and promotions. The fix merged as **PR #316**; the evidence is the last comment on #237.

Three consequences, and they are why this file still exists:

1. **Every stage is now merged**, stage 3 last, as PR #318. It shipped as an ordinary correctness fix
   judged on the invariant it repairs plus bench, because the parity swing was spent as an acceptance
   criterion — #306 took it from ≈265 cp to ≈4 cp on its own.
2. **Every sweep recorded below is historical.** The stage 0b baseline and the stage 2 sweep both
   predate #306, so neither is a valid comparison basis for anything. Stage 3 measured against a
   fresh baseline taken on `d8f0f54`.
3. **Every node count below is pre-#312.** `nodes_searched` excluded quiescence nodes until PR #313,
   so those figures are not comparable with anything measured today, in either direction.

One task is left — the cumulative strength run — and then this file goes. See
[What is left, and when this file can go](#what-is-left-and-when-this-file-can-go).

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
  exhaustion on a perpetual-check line rather than an out-of-bounds write. *Addressed by stage 2
  (PR #311), which added the `MAX_PLY` backstop plus draw detection on the in-check path.*

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

## Also split out: the `stop`-before-`start` race — now #301

Surfaced when stage 0's concurrency test hung deterministically. `TimeManager::start()` stores
`should_stop_ = false` (`TimeManager.h:29`); `ApplyLimits()` calls it from inside `GetMove()` on the
**search thread** (`AIPerplex.cpp:203`); `stop()` is callable from any thread. A `stop` arriving
between the search thread being spawned and its reaching `ApplyLimits()` is therefore silently
erased, and with `go infinite` (`limits.depth = 50`) the search never ends — `stop_and_join()` blocks
in `join()` forever. `TimeManager.h:15-16` documents the ordering requirement without enforcing it.

Stage 0's test closes the window with a 200 ms sleep and says so in its comment. **That sleep is a
marker, not a fix** — when the race is fixed, delete it and let the test exercise the real window.

## Measurement parameters — decided: bench + SPRT

Approved 2026-08-15 for stage 2, and they carry over unchanged to stage 3:

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
| 0b | `WAC-287` baseline sweep, depths 4-12 | evidence | **done, PR #303 — expired by #306** | — |
| 1 | Terminal-node TT storage (defect B) | search change | **merged, PR #304** | — |
| 2 | Legal qsearch while in check (defect A) | search change | **merged, PR #311** | — |
| 3 | Qsearch TT depth → remaining budget (defect C) | search change | **merged, PR #318** — plus the TT replacement-policy fixes it forced | — |
| 4 | Tactical test-data semantics (`WAC-043`, `WAC-065`) | test data | **merged, PR #298** | — |
| — | Officer captures in `ComputeCaptures()` (#306) | search change | **merged, PR #316** — this is what explained `WAC-287` | — |
| — | Quiescence nodes counted in `nodes_searched` (#312) | instrumentation | **merged, PR #313** | — |

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

#### The baseline — expired, kept for the reasoning it produced

Recorded 2026-08-15 on `b72f361` (`origin/main`, stage 0 merged). Release clang-cl,
`Threads=1`, `Hash=192`, position
`rn3k1r/pp2bBpp/2p2n2/q5N1/3P4/1P6/P1P3PP/R1BQ1RK1 w - - 0 1`.

**Do not compare anything against this table.** It was the comparison basis for stages 1-3 until
#306 merged; the position now scores flat across depths 4-12 and returns `d1h5` throughout. Node
counts here are also pre-#312 (main-search only). What survives is the *method* — the parity
decomposition, and the `Ne6+` observation that first tied this position to horizon checks.

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

**Stage 2 ships with two prerequisites, and the branch is named for them.** Legal in-check
quiescence makes quiet evasions reachable, and that exposed two defects that had been unreachable:
LMR was reducing checking moves, and a board set up from a FEN carried an empty repetition history
(`Board::setup_from_fen_impl` never seeded it, and `is_repetition` excluded the root position).
Stage 2 alone scores 35/36 on the tactical suite; all four changes together score 36/36 — so the
intermediate states are configurations that will never ship, and splitting them into separate PRs
would have measured engines we do not intend to build. Per-defect attribution was established by
tactical-suite bisection instead of by Elo.

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
2. **~~The recursion is bounded by material, not by the ply cap.~~ That claim was wrong.** It ran:
   a side that is *not* in check generates captures only, so the chain can only continue through
   capturing checks, which run out. The hole is that an in-check node generates **quiet** evasions
   too, and a quiet evasion may itself give check. Two sides can therefore alternate checks with no
   capture between them — material never falls, the position is free to repeat, and the line runs to
   the absolute backstop, where it is settled by a static evaluation of a position still in check.
   That is precisely the defect this stage removes, reintroduced at the ply cap.
   **Resolved by checking repetition and fifty-move draws on the in-check path**, which is the only
   path that can reach them; a capture-only chain is irreversible and cannot repeat. Caught in
   review — the bad invariant is recorded here rather than deleted, because it is the kind of
   argument that sounds airtight and licenses removing a bound.
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

#### Stage 2 — post-change sweep: the parity swing narrowed by a third

**Superseded.** This sweep was taken with the #306 defect still present, i.e. against a quiescence
that could not resolve officer captures, and its node counts are pre-#312. Stage 2 merged as PR #311
on its own correctness grounds; the swing reduction below is no longer evidence for anything.

Recorded 2026-08-16 on `223870f`, same position, binary and settings as the baseline
(`Threads=1`, `Hash=192`, one `go depth 12`).

| depth | move | score | nodes | baseline move | baseline score |
|---:|---|---:|---:|---|---:|
| 1 | `c1f4` | +206 | 43 | `g5h7` | +266 |
| 2 | `h2h4` | +102 | 306 | `h2h4` | +102 |
| 3 | `d1h5` | +287 | 3 521 | `c1d2` | +290 |
| 4 | `d1h5` | +215 | 5 954 | `d1h5` | +215 |
| 5 | `d1h5` | **+499** | 12 191 | `d1h5` | +518 |
| 6 | `d1h5` | +202 | 68 596 | `d1h5` | +196 |
| 7 | `d1h5` | **+484** | 101 300 | `d1h5` | +495 |
| 8 | `f7c4` | +204 | 1 127 795 | `f7e6` | +189 |
| 9 | `f7c4` | **+296** | 1 255 416 | `d1h5` | +438 |
| 10 | `d1h5` | +245 | 2 576 173 | `d1h5` | +245 |
| 11 | `d1h5` | **+306** | 3 267 349 | `d1h5` | +502 |
| 12 | `d1h5` | +268 | 5 090 641 | `d1h5` | +268 |

Odd-depth mean **+396** (was +488), even-depth mean **+227** (was +223): the swing falls from
**≈266 cp to ≈169 cp, a 36% reduction**, and it comes entirely from the odd-depth band collapsing
toward the even one — which is the direction the diagnosis predicts, since the odd band is the one
inflated by quiescence nodes that stood pat while in check. Depths 9 and 11 carry almost all of it.

Read against the criterion this document set, that is the stage working. Two things it is not:
the swing is reduced, not removed, so defect A is one contributor rather than the whole
phenomenon; and depth 8 still does not return `d1h5` — it now prefers `f7c4`, and depth 9 follows
it. That was expected to be a noisy bit and it behaved like one.

Node counts on **this** position rise 3 592 464 → 5 090 641 at depth 12 (+42%), against −30% across
the bench suite. Both are real: legal in-check qsearch does strictly more work per in-check node,
and `WAC-287` is a position whose main line runs through checks repeatedly, which is why it was
selected. The suite-wide direction is what the nps and Elo numbers are built on; this position is
the diagnostic, not the sample.

### Stage 3 — qsearch TT depth: merged, PR #318, and it grew two companions

Shipped as a correctness fix on its own terms: the issue that motivated it was already answered, so
it was judged on the invariant it repairs plus bench, not on `WAC-287`.

**What landed, in three parts.** Only the first was planned.

1. **The unit fix.** `quiescence()`'s parameter is remaining budget: `AIPerplex::QSEARCH_BUDGET = 15`
   from `pvs()`, `qsearch_budget - 1` on recursion, cutoff at `< 0`, and all four `tt.store()` calls
   write the remainder. The cutoff is exactly equivalent — `consumed > 15 ⟺ budget < 0` — so the
   node set the budget admits is unchanged; the reuse relation is what flips.
2. **A phase penalty in `replacementScore()`.** Correcting the unit moved a quiescence *root* — one
   per main-search leaf, so the most numerous entry there is — from `adjusted_depth` 0 to 7, i.e.
   above every main entry of depth 1-6. `pvs()` mines main entries for a hash move even when they are
   too shallow to cut off, so that cost interior move ordering. The −2560 penalty puts the whole
   quiescence band below main-search depth 0, PV bonus included.
3. **Empty slots are filled before anything is evicted.** `store()` scored empty slots like occupied
   ones; an empty slot scores `-512 * age_diff`, so penalized quiescence entries sank *below* empty
   slots and were evicted while three slots in the bucket sat unused.

**Measured, `Run-Bench` position set at fixed depth (node counts are deterministic; wall clock was
single-sampled and is not quoted as a measurement):**

| variant | 16 MB nodes | 192 MB nodes |
|---|---:|---:|
| pre-change reference | 29.80 M | 33.44 M |
| unit fix alone | 40.25 M (+35.1%) | 28.13 M (−15.9%) |
| + phase penalty | 32.33 M (+8.5%) | 27.74 M (−17.0%) |
| + empty-slot fix (shipped) | **28.98 M (−2.7%)** | **28.41 M (−15.0%)** |

`WAC-287` returns identical move, score and PV at every depth 1-12 against a fresh pre-change
baseline on `d8f0f54`.

**SPRT: inconclusive, and deliberately not in `Docs/EloLog.md`.** `-Sprt NonRegression` against the
pre-change build, 2000 games at 10+0.1, 192 MB: `+9.90 +/- 11.88 Elo`, LOS 94.9%, LLR 1.69 of 2.94.
Roughly 3000-3500 games would likely have crossed the accept bound. The batch also raised
illegal-PV warnings (#310) from *both* engines, which makes it discardable by this project's own
rule, and it measured only the first of the three changes above. Recorded here rather than in the
ledger for both reasons.

#### Harvest: two decisions this stage reversed

- **"TT phase and depth are untouched by stage 3"** (the stage 2 design section above) is **wrong as
  shipped**. Depth semantics changed, and the replacement calibration had to change with them. Left
  in place above rather than edited, because the reasoning that produced it is the useful part.
- **The +8.5% residual at 16 MB was not "the price of correctness".** It was characterised that way
  before the empty-slot defect was found. Both reviews' instinct that replacement policy was still
  wrong beat the argument that the cost was unavoidable — worth remembering the next time a residual
  gets explained rather than investigated.

Durable content is in the source: the budget unit on `AIPerplex::QSEARCH_BUDGET` and
`quiescence()`'s declaration, the penalty's sizing argument in `replacementScore()`, and the
empty-slot rule in `store()`. Nothing from this section needs to survive the file.

### Spin-offs from this work — where each one lives now

| # | What | Status |
|---|---|---|
| #299 | Aborted searches return `Draw` and poison the TT at full depth | open; stage 1's terminal store is exempt from its guard (noted on the issue) |
| #301 | `stop` erased by `TimeManager::start()`, `go infinite` unstoppable | open; stage 0's 200 ms sleep in the concurrency test is the marker to delete when it lands |
| #305 | TT mate-score normalisation stops working beyond ply 100 | open |
| #306 | Officer captures never generated | **closed, PR #316** |
| #307 | Legacy `PlayerAiBase::Quiescent` has no depth cap, live after #306 | open |
| #308 | `quiescence()` holds a `GameInfo` reference into a vector it can reallocate | open — this is the "watch during implementation" note above, now filed |
| #310 | Reported PV contains illegal moves | open |
| #312 | `nodes_searched` excluded quiescence | **closed, PR #313** |
| #314, #315 | Node-counter gaps (null-move/re-search edges; `qnodes` above `Threads=1`) | open |
| #319 | TT same-key store overwrites unconditionally, bypassing phase and depth | open; the one route still ignoring the policy stage 3 established. Current behaviour is pinned by a test |
| #320 | Quiescence evasion ordering sorts king moves last | open; deferred through stages 2-3 to protect attribution, that reason has expired |
| #321 | In-check quiescence entries store negative budgets and are never reused | open |

Both items that had no home now have one: evasion ordering is #320, and the `WAC-043` correction was
applied to the #237 issue body on 2026-08-16.

---

## What is left, and when this file can go

**Outstanding: the cumulative strength measurement.** Everything in #237 is merged; what has never
been measured is the arc as a whole. Agreed parameters, so the dispatch does not have to be
re-derived:

```
gh workflow run strength.yml --ref main \
  -f reference_ref=bda2cc48e6c948322afa7f11a49331f80a17862a \
  -f games=19980 -f shards=18 -f concurrency=3
```

`bda2cc4` is the merge of #298 — the last commit before any #237 *engine* change, so the run spans
#300, #304, #311, #316, #313 and #318. Expect #306 to dominate the result. It is ~3 h and takes 18 of
the 20 concurrent job slots, so it queues every other merge while it runs. The number lands in
`Docs/EloLog.md`'s **Linux/GCC ledger** and must not be compared with the local clang-cl rows,
including the SPRT quoted above.

**Then delete this file.** The three lifecycle conditions (`Docs/Workflow.md` → Design document
lifecycle) are otherwise already met:

- *No inbound references* — nothing in the tree cites it.
- *No spec role* — the strength run above is the last unstarted item, and it is one command.
- *Durable content harvested* — tactical-test semantics are in `Docs/TestDesign.md`, SPRT bounds,
  book resolution and the meaning of "inconclusive" are in `Docs/EloMeasurement.md`, the per-stage
  reasoning is in source comments, and every loose end is an issue in the table above. Record the
  strength-run result in `Docs/EloLog.md`, not here.

---

## Resuming in a later session

1. Read this file's "Read this first" section, then the #237 issue body **and its last comment** —
   the comment is where the conclusion lives.
2. `Get-Worktrees.ps1` for drift and PR state.
3. All five stages are merged. The only work left is the strength run above.
4. Re-check assumptions against `origin/main` before executing — this document ages, and it has now
   been overtaken twice.
