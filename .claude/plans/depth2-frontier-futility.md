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
  candidate is also a frontier candidate, and frontier is tested first. So neither of them fires at
  that node: the node stores the fail-soft UPPER floor `max(best, static_eval + 300)` instead of their
  `alpha`. That is a
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

So at a depth-2 node where the guard holds, frontier takes every move LMP or quiet SEE would skip
(except D6's pushes), and neither of their alpha claims is made there. The node instead stores
`max(best, static_eval + 300)` as UPPER. Both values are at most alpha, so the bound type is the same.
Only the fail-soft value is lower, a tighter claim made under the futility premise, which is exactly
the depth-1 behaviour.

- **Rejected: giving LMP precedence, or raising the floor to alpha** whenever a skipped move was past
  LMP's index. That adds a per-move branch to recover a weaker bound, and nothing shows the stronger
  bound misleads.
- With `LateMovePruningDepth2StoreUpper=false` (not the default), such a node now stores instead of
  taking LMP's no-store return. That is accepted, and the option's comment will say so.

### D4: the floor and store mirror depth 1

`best_value = max(best_value, static_eval + margin)` is kept, still at most alpha, so the store stays
UPPER.

- **At depth 1** the floor rarely binds (0.29%, #529), because quiescence fails high at exactly beta.
- **At depth 2 it should bind on most fail-lows.** A depth-1 child that cuts by searching hands back
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

- **The lab applies `candidate_uci_options` to the candidate only.** `strength.yml` checks the name
  against the advertised option table before any shard starts, but a misspelt or ignored value would
  read as a ~0 Elo result. *Verified* by checking that the run's PGN node and score comments differ
  from the reference side's on the same opening; done when the run reports.

## Invariants

- At the default (`FrontierFutilityMaxDepth=1`), search is node-identical to `main` at `Threads=1`.
- An effective depth of 3 or more (read after IIR) is never frontier-pruned, because the domain
  rejects 3.
- At depth 2, a push to the relative 7th rank is never frontier-skipped.
- The first legal move is always searched.
- Every node that skips a move stores a value at most alpha, as UPPER.
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
  5. a depth-2 fail-low with children well below alpha returns and stores UPPER at exactly
     `static_eval + 300`, falsified by reverting the floor margin;
  6. at depth 2 a push to the 7th rank is searched while other quiets are skipped, and a depth-1
     node still skips it;
  7. the option advertisement line.
- One `Run-Bench.ps1` pass at the default, which should match `main`'s nps within noise.
- One flag-on pass, with per-position nodes reported for information only, so that an H0 can be read
  against #634's endgame tree growth.
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
| D3's precedence consequence | comment at the frontier guard, and on `LateMovePruningDepth2StoreUpper` |
| D4's depth-2 floor reasoning | the rewritten floor comment in `pvs()` |
| D6's promotion-reach reasoning | comment at the frontier guard's 7th-rank term |
| D5's lab result | `Measurements/` ledger, plus a headline in `Docs/Changelog.md` |
