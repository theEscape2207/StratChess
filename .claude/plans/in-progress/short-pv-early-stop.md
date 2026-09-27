# Short-PV early stop — Design

**Issue:** #652

## Goal

After each completed iteration, `should_stop_early()` (`AIPerplex.cpp:1834`) ends iterative deepening
when the root PV is shorter than `depth - depth/2`. The rule treats that as proof of a forced line, but
a PV also ends early at any terminal node: an in-search twofold repetition (`pvs()` clears the row and
returns the draw), a fifty-move draw, or stalemate. None of these is a forced line; a repetition only
shows that the best line found so far *can* repeat. The rule fires both under `go depth N` and in timed
games, because it runs after the soft-limit gate.

Evidence on `f58f052`:
- **Fixed depth.** Take the perpetual-check position `6k1/6p1/8/8/4Q3/2q5/r4PPP/6K1 w - - 0 1`.
  `go depth 16` stops at depth 11: the PV `e4e8 g8h7 e8h5 h7g8 h5e8` has 5 plies, which is below
  11 − 5 = 6. The `go depth N` contract is broken.
- **Games.** The latest three strength-lab runs cover 2.1–2.3M moves. Moves scored 0.00 that used under
  a quarter of the side's median time make up 1.0–2.7% of all moves, about half of every 0.00 move.
  Their median depth is 9, against 12 for the rest.
  - About 30% of them come straight after a non-zero score. One case: −0.29 at depth 13, then 0.00 at
    depth 9 in 0.046 s, then +0.20 at depth 12 on the next move.
  - This is a proxy. The PGN records score, depth and time but not PV length. It shows the rule fires
    routinely, not that it costs Elo.

## Scope

**This change will:**

- Delete the PV-length arm of `should_stop_early()`. The mate-score stop and the soft and hard time
  limits stay.
- Replace the unit test that pins the short-PV stop. Add a search-level regression on the
  perpetual-check FEN.
- Update the `Docs/Engine-Readme.md` flow line and the `Docs/TestDesign.md` entry that describe the
  rule.

**This change will not:**

- Touch `assess_iteration_quality()`'s short-PV rejection. It judges *interrupted* iterations, has
  different inputs, and needs its own evidence.
- Change repetition adjudication, PV construction or time allocation.

## Decisions

### D1: Delete the arm, do not replace it

- **Chosen:** delete it.
- **Rejected: exclude draw scores.** Contempt shifts draw scores away from 0, and a 0 score is not
  proof of a repetition. The guard would be incomplete by construction.
- **Rejected: propagate a terminal reason with the PV, or replay the PV to classify its end.** This
  adds machinery to keep a heuristic whose premise is unproven. A PV that ends in a real forced
  sequence (mate) is already covered by the mate-score stop.
- **What deletion costs:** time in genuinely dead positions, such as the perpetual above, which will
  now search to the soft limit. The lab run (Validation) prices that against the premature stops.

### D2: Keep/park threshold, agreed before the lab run

The fix restores a stated contract (`go depth N` reaches N), so it does not have to show a gain.

- **Keep:** unless the lab result is a significant loss, meaning the 95% interval lies wholly below 0.
- **If it is a significant loss:** park, and report which moves lost time. The replacement would then
  be a terminal-aware rule, designed from that data.

Confirmed by the owner 2026-09-27.

## Assumptions I cannot verify from the code

- **The proxy counts this rule, not something else.**
  - A fast 0.00 move could also come from a single legal move, or from a TT-resident repetition line
    that resolves a depth instantly.
  - The count is only used to show the rule fires in games; the decision does not rest on it.
  - Verification: the lab before/after. Its PGNs show whether fast 0.00 moves disappear.
- **The two #652 FENs no longer apply.** They reproduced only under the candidate's move ordering at
  `669e7ba` (#651). On `main` they already reach full depth, so they cannot fail before the fix. The
  perpetual-check FEN is the pre-fix reproducer instead. Verified by probe on `f58f052`.

## Invariants

- `go depth N` on a non-mate position completes depth N, unless it is stopped externally or hits a
  limit.
- A mate score still ends deepening. The comment at `AIPerplex.cpp:1454` relies on this.
- Soft and hard time limits are unchanged.

## Validation

- **Unit tests:** `should_stop_early()` loses its `pv_length` parameter, since it is unused under
  `/W4 /WX`. A short-PV unit case would then pass by construction, so it becomes a boundary case
  instead: `Mate_Threshold - 1` of either sign does not stop. The mate cases stay.
- **Search test:**
  - `Search()` on the perpetual FEN with `SearchLimits::fixed_depth(12)` must report
    `depth_completed == 12`.
  - Falsify it: on the unchanged code it stops at 11.
- **Engine tier:** `Validate-PrePR.ps1`, which runs the build, extended tests, the tactical suite and
  self-play. Then `search-reviewer`, and the cross-agent review.
- **Equivalence:** fixed-depth node equivalence is expected to differ and is not a gate.
- **Strength:** the CI strength lab against `f58f052`, about 3 h and 18 of 20 CI slots. It runs only
  on the owner's go, and is judged by D2.

## Cost

- **Size:** under 50 lines across `AIPerplex.cpp`, `SearchIterationTests.cpp` and two docs.
- **Blast radius:** Engine tier; search time use in both fixed-depth and timed play.
- **Review:** one code review and `search-reviewer`. The PR goes to the cross-agent round.
- **Optional, priced alone:** none. The lab run is required by D2; only its timing is the owner's call.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the PV-length stop was removed, with the perpetual repro | `Docs/Changelog.md`, PR body |
| Lab result and the D2 verdict | `Docs/Changelog.md`, PR body |
| Deepening stops only on a mate score or a limit | `Docs/Engine-Readme.md` flow line |
