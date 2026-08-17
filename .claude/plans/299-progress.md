# #299 / #310 — progress

Status for resuming cold. Design and rationale: `abort-unwind-and-pv-integrity.md`. Delete this file
with that one at the end of PR 2.

## Where we are

**PR 0 (node limit + rename) — merged as #326.** **PR 1 (the abort unwind guard) — pushed, awaiting
review.** Results and rationale for both are in `Docs/Changelog.md` (2026-08-17); nothing durable
from either lives only here.

All four assumptions in the design are settled and none changed a decision. PR 1 landed D1, D3 and
D5, and closed the last open assumption: the fastchess warnings **are** this defect — `go nodes 10000`
from `position startpos moves e2e4 e7e5 g1f3` reproduced an illegal `info … pv` line before the guard
and does not after it. PR 2 (the PV-move ordering revival) is the only landing left.

## Next steps

1. **PR 1 is awaiting cross-agent review**, and `-Sprt NonRegression` under time control has not been
   run — it is the user's spend decision. Fixed-depth equivalence does not cover it, because TT
   content under abort is exactly what changes.
2. **PR 2** — the PV-move ordering revival (D7), only after PR 1 merges, so its SPRT baseline is the
   merged state. Delete the design doc and this file with it, once Harvest is discharged.

## Watch out for

- **A node-limited search's TT and PV are trustworthy again as of PR 1.** The warning that stood here
  while PR 1 was open no longer applies.
- The abort-path regression test is `cmd_go: a node-limited search never reports a spliced pv`
  (`UCITests.cpp`). Budget **10,000** is the one that was red before the guard; the other three are
  the same check at other abort points. `Docs/TestDesign.md` says why they are not interchangeable.
- `Move` equality ignores flags, so `pv_replays_legally` compares `flags()` by hand. Filed as #325; if
  that lands, drop `same_move_exactly` in `PVIntegrity.cpp` for `==`.
- PR 2's SPRT is the user's spend decision — report the cost, do not start one unilaterally.
