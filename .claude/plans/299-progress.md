# #299 / #310 — progress

Status for resuming cold. Design and rationale: `abort-unwind-and-pv-integrity.md`. Delete this file
with that one at the end of PR 3.

Branch: `worktree-299-abort-unwind`.

## Where we are

**PR 0 (node limit + rename) — implemented and tested, not yet reviewed or pushed.**

Fast suite is 393 cases / 6694 assertions, which is the previous 384 / 6668 plus exactly the nine new
tests. The two node-limit search tests were falsified by neutering `NodeLimitReached` at its body
rather than at a call site: both then run past 180 s instead of failing, because with no node limit a
depth-20 search has only the 1-hour fallback clock to stop it. That is genuine dependence on the
feature, and it is the only available shape — a depth low enough to finish without the limit would be
what stopped the search, proving nothing.

A related trap for whoever runs these next: `build.ps1 tests` reporting `ninja: no work to do` means
you are measuring the previous binary. A stale artifact reported 374 / 6599 here and looked green.

`go nodes N` works and is deterministic: three runs of `position startpos moves e2e4 e7e5 g1f3` +
`go nodes 20000` produced byte-identical output, stopping at 20,259 nodes (the first poll past the
limit). That run also reproduces the defect on demand — depth 6 returns `score cp 0` after depth 5's
`-8`, which is the contaminated abort score being accepted.

All four assumptions in the design are settled. None changed a decision.

## Next steps

1. PR 0: fixed-depth equivalence (identical nodes and best moves, `Threads=1`) and `Run-Bench.ps1`
   before/after, against a binary built from `origin/main`.
2. PR 0: `search-reviewer`, then `New-PullRequest.ps1`. No Elo match — see Validation in the design.
4. PR 1: implement the unwind guard and the `pv_replays_legally` assertion. The deterministic
   regression test uses PR 0's node limit; pick the limit from a position where the interrupted
   iteration is *accepted*, and falsify by reverting the guard.
5. PR 2: the PV-move ordering revival, only after PR 1 merges.

## Watch out for

- The interrupted-iteration reproduction above is the PR 1 test case, but it must be re-derived
  after PR 1's guard changes what an interrupted iteration returns.
- `Move` equality ignores flags, so the PV replay's membership test must compare `flags()` itself.
  Filed as #325; if that lands first, drop the hand-rolled comparison.
- Both SPRTs are the user's spend decision — report the cost, do not start one unilaterally.
