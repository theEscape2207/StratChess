# #299 / #310 — progress

Status for resuming cold. Design and rationale: `abort-unwind-and-pv-integrity.md`. Delete this file
with that one at the end of PR 2.

## Where we are

**PR 0 (node limit + rename) — merged as #326.** Results and rationale are in `Docs/Changelog.md`
(2026-08-17); nothing durable from it lives only here.

All four assumptions in the design are settled and none changed a decision. The remaining landings are
PR 1 (the abort-unwind guard) and PR 2 (the PV-move ordering revival).

## Next steps

1. **PR 1** — implement the unwind guard (D1) and the root partial-iteration handling (D3), plus the
   `pv_replays_legally` Debug assertion (D5). The deterministic regression test uses #326's node limit.
   Falsify by reverting the guard. Then `search-reviewer`, then bench, then `-Sprt NonRegression`.
2. **PR 2** — the PV-move ordering revival (D7), only after PR 1 merges, so its SPRT baseline is the
   merged state. Delete the design doc and this file with it, once Harvest is discharged.

## Deferred from #326, deliberately

Not lost, just not worth their own PR yet — fold them into PR 1, which touches the same block:

- Extract the duplicated poll in `pvs()`/`quiescence()` into one private helper. Both copies must agree
  on the two counters they read, and PR 1 is about to add abort state to exactly that block.
- Tests the reviewer suggested and I did not add: `fixed_nodes(0)` semantics (it currently arms the
  check and aborts at the first poll rather than meaning "unlimited" — asserted, not tested),
  `infinite` + `nodes` precedence, and a `Threads=4` node-limit smoke test. The last one is the only
  new code path from #326 with no coverage at all.

## Watch out for

- **A node-limited search's TT and PV are deliberately untrustworthy until PR 1 lands.** The seam makes
  the abort fire on demand, and the abort is exactly what poisons them. Do not use it to generate
  reference data in the meantime.
- The `go nodes 20000` reproduction from `position startpos moves e2e4 e7e5 g1f3` (depth 6 returning
  `score cp 0` after depth 5's `-8`) is the PR 1 test case, but it must be re-derived once the guard
  changes what an interrupted iteration returns.
- `StratChessTests/UCITests.cpp` already has a PV-legality replay helper behind the `cmd_go` tests (the
  anonymous-namespace block ending just before "at depth >= 3 the pv carries more than one move and
  replays legally"). Check it before writing `pv_replays_legally` — the test side may already have the
  shape PR 1 needs.
- `Move` equality ignores flags, so the PV replay's membership test must compare `flags()` itself.
  Filed as #325; if that lands first, drop the hand-rolled comparison.
- Both SPRTs are the user's spend decision — report the cost, do not start one unilaterally.
