# Depth-2 frontier futility — Design

**Issue:** #634 (re-screen tracked by #961)

## Goal

Frontier futility skips a hopeless quiet move only at depth 1. #634 proposed extending it to depth 2
(extended futility). That attempt was parked on a wall-clock gate: nodes moved -33% to +55% depending
on the position, which no screen can turn into an Elo forecast. #636's method has since dropped
wall-clock gates in favour of the lab, and the search around it has changed (LMP at index 8 for depths
2 and 3, depth-2 LMP UPPER stores, quiet SEE pruning and IIR). So the question is now only "does it
gain Elo on today's tree?" This change ships the extension behind a runtime option that is off by
default, so the lab can answer that on one SHA.

## Review focus

- **D3, the precedence rule at depth 2.** Wherever the futility guard holds, every LMP and quiet-SEE
  candidate except a D6 push is also a frontier candidate, and frontier is tested first. A
  frontier-only fail-low frame therefore stores the fail-soft UPPER floor `max(best, static_eval + 300)`
  instead of their `alpha`. A mixed frame keeps today's alpha policy. That is a
  stronger claim, and it is made on depth-1 children rather than quiescence.
- **D6, the 7th-rank push exemption.** It is the one guard depth 1 does not need. Without it, depth 2
  hides promotions that quiescence would find. It is narrow: a push threatening a capture is still
  skippable.
- **D2, the 300 cp margin.** It is pre-registered and stays fixed through the lab run.

## Scope

**This change will:**

- Add `frontier_futility_max_depth` (default 1, domain [1, 2], UCI `FrontierFutilityMaxDepth`).
- Make the frontier margin depth-dependent, used by both the guard and the fail-low floor.
- Exempt pawn pushes to the relative 7th rank from frontier futility at depth 2 (D6).
- Rewrite the floor comment, which currently assumes quiescence children.
- Add tests for the depth band, the margin and the floor, each falsified.

**This change will not:**

- Change any default. At the default the search is node-identical to `main`.
- Allow depth 3 (#498 measured 94.75% overlap with LMR there).
- Add a margin-step tuning field, or reorder the frontier, LMP and SEE guards.
- Touch #545 (TT-refined reverse futility), which is the second slice of #961.
- Promote the default. That is a follow-up PR, and only on a lab H1.

## Decisions

### D1: a max-depth field, not a feature flag

`TUNING_FIELD(int, frontier_futility_max_depth, 1, 1, 2, true, "FrontierFutilityMaxDepth", true)`
sits beside `frontier_futility_margin`. `frontier_futility_eligible` then tests
`depth > tuning_.frontier_futility_max_depth` instead of the hard-coded `depth > 1`.

- This mirrors `reverse_futility_max_depth`, as #634 settled.
- **Rejected: a Boolean `FrontierFutilityDepth2`.** Its future end state is the same field, so it
  would only add a rename.
- **Rejected: a domain up to `MAX_PLY`.** The margin formula has no evidence past depth 2, and depth 3
  is out of scope. `Validate()` rejecting 3 is the guard.

### D2: margin = `frontier_futility_margin + 100 * (depth - 1)`

This gives 300 cp at depth 2 and the unchanged 200 at depth 1. It is computed once per node into a
local and read by both the move guard (`AIPerplex.cpp`, `frontier_candidate`) and the floor.

- #634 decided this margin, and #529 showed the margin is low-risk: 150, 200 and 250 cp at depth 1
  agree on the best move in at least 99.7% of positions.
- **Rejected: a UCI-tunable step.** It would add a second axis to a lab run that tests only one.

### D3: guards and precedence unchanged

Depth 2 uses the depth-1 guards unchanged:

- the node is not PV, not in check, not an exclusion frame, and alpha is outside the mate range;
- at least one legal move has been searched;
- the move is a quiet non-promotion that is neither a killer nor the hash move;
- after the move is played, it neither gives check nor draws immediately.

Frontier is still tested before LMP and quiet SEE. Both of those guards' per-move terms are subsets of
frontier's, and the post-move gates are identical. The eval term is constant across a non-PV node.

So at a depth-2 node where the guard holds, frontier takes every move LMP or quiet SEE would skip,
except D6's 7th-rank pushes. Those pushes are exempt from frontier only: LMP (from index 8) or quiet
SEE may still skip them, and frontier skips advance the legal index. That gives two kinds of frame,
and each keeps today's result and store policy unchanged:

| Skips in a completed fail-low frame | Result and store |
|---|---|
| Frontier only | UPPER at `max(best, static_eval + 300)` |
| Frontier, plus LMP or quiet SEE on a D6 push | UPPER at original alpha; with `LateMovePruningDepth2StoreUpper=false` and an LMP skip, return alpha and store nothing |

In a frontier-only frame the fail-soft value is lower than LMP's alpha. That is a tighter claim, made
under the futility premise, and is exactly the depth-1 behaviour. With
`LateMovePruningDepth2StoreUpper=false` (not the default), a frontier-only frame now stores where
LMP's no-store return used to apply. That is accepted, and the option's comment will say so.

- **Rejected: giving LMP precedence, or raising the floor to alpha** whenever a skipped move was past
  LMP's index. That adds a per-move branch to recover a weaker bound, and nothing shows the stronger
  bound misleads.
- **Rejected: a new precedence rule for mixed frames.** The existing floor-then-alpha ordering
  already resolves them, and in the direction of the weaker claim.

### D4: the floor and store mirror depth 1

`best_value = max(best_value, static_eval + margin)` is kept, still at most alpha, so the store stays
UPPER.

- **At depth 1** the floor rarely binds (0.29%, #529), because quiescence fails high at exactly beta.
- **At depth 2 it is expected (unmeasured) to bind on most fail-lows.** A depth-1 child that cuts by searching hands back
  our quiescence value after its refutation, which usually sits below `eval + 300`.
- **The exceptions** are a child cut by reverse futility, which is fail-hard and returns exactly
  alpha, and a child TT cut, which is fail-soft.

The floor only ever raises the value toward alpha, so binding often is conservative. The comment is
rewritten to say this rather than lean on quiescence. The bind rate goes unmeasured: the lab decides,
and no rate would change this decision.

### D6: pushes to the relative 7th rank are exempt at depth 2

At depth 2, the child after a skipped push is a depth-1 opponent node that must move, and our
quiescence beneath it then promotes. That is a gain of about 800 cp, far past the margin. At depth 1
the same push leads straight into the opponent's quiescence, which stands pat, so no promotion is
reachable and nothing is lost by skipping it.

The term applies at depth 2 only, so depth 1 and the default stay node-identical. It is tested last,
because it reads the moving piece.

- **Rejected: no exemption.** That hides a known 800 cp class in pawn races and rook endgames, which
  is where #634's step 4 lost the most. A lab H0 would then not show whether the idea or this hole
  failed.
- **Rejected: exempting every passed-pawn push.** That is wider than the hole the reviewer found, and
  it costs a passed-pawn test per move.
- **Known residue:** depth-2 LMP has the same gap from index 8. It is out of scope here.

### D5: the gate is the lab, with a rule stated before dispatch

The gate is `strength.yml`, Full with `sprt=true` (SPRT [0, 3]), `reference_ref=merge-base`,
`candidate_uci_options=FrontierFutilityMaxDepth=2`, `Threads=1`. It runs only on the owner's go.

- **H1:** a follow-up PR sets the default to 2.
- **H0, or inconclusive at the cap:** a PR deletes the field and restores the hard-coded depth 1.
- **Dropped from #634's plan:** the wall-clock gate (#636) and the decision-change screen, because
  screens have mispredicted the lab here (#664).
- **Knock-on effects the lab also measures:**
  - a null-move child from depth 6 lands at depth 2, so its quiet threat moves become skippable and
    null-move cuts rise;
  - singular verifications from depths 7-8 have depth-2 grandchildren, so how often extensions and
    multi-cut fire changes.

  These belong to the experiment, not to a defect.

## Assumptions I cannot verify from the code

None remain.

- **Lab routing is verified from source.** `.github/scripts/play_shard.sh` builds separate candidate
  and reference option arrays and attaches each to its own engine.
- **Application is acknowledged in source.** `UciHandler::set_tuning_option` answers
  `info string <name> <value>` only when the value is applied.
- **A preflight before the lab spend** shows the option changes the search. A fresh process, at
  `Threads=1` and fixed depth, is compared at max depth 1 versus 2 on a position that exercises the
  guard. It must show the `info string FrontierFutilityMaxDepth 2` acknowledgement and a different
  node count or `frontier skips` line.
- **PGN differences are not used as proof.** Timed searches differ even under identical settings.

## Invariants

- At the default (`FrontierFutilityMaxDepth=1`), search is node-identical to `main` at `Threads=1`.
- An effective depth of 3 or more (read after IIR) is never frontier-pruned, because the domain
  rejects 3.
- At depth 2, a push to the relative 7th rank is never frontier-skipped.
- The first legal move is always searched.
- A completed frontier-skipping fail-low returns at most original alpha and stores UPPER, except where
  the existing depth-2 LMP no-store policy applies. A later searched cutoff keeps its searched score
  and its LOWER store.
- A skip runs no child search and comes before every PV, killer, history and TT write, so the
  aborted-frame contract is untouched.

## Validation

**Tier: Engine.**

- `Compare-SearchEquivalence.ps1` IDENTICAL at the default.
- New tests, each falsified by reverting the line it covers:
  1. depth 2 is eligible at max depth 2 and not at 1;
  2. depth 3 is never eligible, and UCI/JSON reject 3;
  3. the margin boundary at depth 2: eval + 300 equal to alpha prunes, and one centipawn above alpha
     does not;
  4. a depth-2 `pvs()` frame skips moves at max depth 2, and the existing "depth-2 node skips
     nothing" test holds at the default;
  5. a frontier-only depth-2 fail-low, with children well below alpha, returns and stores UPPER at
     exactly `static_eval + 300`, falsified by reverting the floor margin. LMP and quiet SEE are off,
     so no competing skip interferes;
  6. at depth 2 a push to the 7th rank is not frontier-skipped while other quiets are, and a depth-1
     node still skips it. LMP and quiet SEE are off;
  7. a mixed frame at depth 2, with a frontier skip and an LMP skip of a D6 push both observed. With
     the store option on, the node returns exactly alpha, which lies above `eval + 300`, and stores
     UPPER at alpha. With it off, it returns alpha and stores nothing;
  8. the option advertisement line, plus a successful UCI application of 2.
- **Default overhead:** `Compare-Bench.ps1` paired against the merge-base build from the same
  compiler. This is the only nps claim.
- **Flag-on, informational only:** per-position nodes at max depth 2, so an H0 can be read against
  #634's endgame tree growth. They come from a throwaway scratchpad UCI driver that sends the option
  and waits for each fixed-depth search to finish. `Run-Bench.ps1` cannot set tuning options. The same
  driver runs the Assumptions preflight.
- **The Elo gate is D5's lab run, on the owner's go.**

## Cost

- **Size:** 50–200 lines, mostly tests, in `SearchTuning.def`, `AIPerplex.cpp`, the frontier and
  tuning test files, and `Docs/Changelog.md`.
- **Blast radius:** Engine tier, default-off, with no gate, skill or doc change beyond the changelog.
- **Review:** one code review and one `search-reviewer` pass on this document.
- **Lab:** 1–4 h, on the owner's go.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D2's margin formula and its #529 basis | comment on `frontier_futility_max_depth` in `SearchTuning.def` |
| D3's precedence consequence, qualified to frontier-only frames; mixed frames keep alpha | comment at the frontier guard and the floor, and on `LateMovePruningDepth2StoreUpper` |
| D4's depth-2 floor reasoning | the rewritten floor comment in `pvs()` |
| D6's promotion-reach reasoning | comment at the frontier guard's 7th-rank term |
| D5's lab result | `Measurements/` ledger, plus a headline in `Docs/Changelog.md` (pending the owner's lab go) |

**Changed during implementation:** none of the approved decisions changed.
- D2's 100 cp step is the named constant `kFrontierFutilityMarginPerPly` in `AIPerplex.h`.
- Validation 6 searches at an out-of-reach alpha, because a searched b6-b7 promotes and fails high
  at `eval + 400`. That is D6 working.

**Validation results (f61511e1):**
- Equivalence: IDENTICAL at the default.
- Default overhead: `Compare-Bench.ps1`, depth 14, 12 rounds, verdict No slowdown, +0.40%
  [-0.12%, +0.92%].
- Preflight: acknowledged `info string FrontierFutilityMaxDepth 2`.
- Flag-on nodes at depth 14, informational only: -9.5% in total, rook-endgm -16.2%, piece-endgm
  -23.8% (best move changed), tactical-5 -25.9%, startpos to open-mid between -2.9% and +2.6%.
