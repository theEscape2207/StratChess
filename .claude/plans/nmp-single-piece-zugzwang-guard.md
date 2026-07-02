# NMP Single-Piece Zugzwang Guard (Issue #66, QFORK-001)

## Goal

Fix issue #66: the tactical suite fails QFORK-001 (`8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1`,
expected `Qa4+`/`Qb3`, engine plays passive `Qe2+`) because null-move pruning
(PR #55) mis-prunes the winning line. Restore 8/8 on the exe tactical suite,
and add verification layers so a tactical regression of this class is caught by
the fast test tier / pre-PR gate instead of by a human running the exe suite.

Scope limits: no verification-search implementation (noted as future work), no
NMP re-tuning (R, min-depth unchanged), no new tactical positions beyond the
regression case.

## Root Cause (evidence)

- Reproduced at merge of PR #67/#68: `tactical test` → 7/8, QFORK-001 plays `d1e2`.
- UCI probe with NMP on: depth 4 → `d1e2` (cp 352), depth 6/8 → `d1g4` (cp ≈362).
- Post-fix probe (NMP inert in this position — both sides < 2 pieces, so this
  equals NMP-off behavior): depth 4 → `d1b3` ✔; depth 6 still `d1g4` at bare
  material. So the *regression* is specifically the depth-4 move choice; the
  engine not scoring the full rook win at deeper depths is a pre-existing
  eval/horizon characteristic independent of NMP, out of scope here.
- Issue author verified: `null_move_enabled = false` → `d1b3`, suite 8/8.
- Mechanism: KQ vs KR wins by **domination/zugzwang** — Black loses only
  because he must move (any rook move loses the rook, any king move abandons
  it). At Black-to-move nodes inside the winning lines, NMP lets Black "pass";
  the null search then reports Black holds, failing high and cutting off every
  winning White line. The PR #55 zugzwang guard only refuses the null when the
  side to move has **zero** non-pawn material (king+pawns), so Black's lone
  rook passes the guard and the mis-prune goes undetected.

## Design Decisions

1. **Tighten the guard: require ≥ 2 non-pawn pieces for the side to move**
   (`std::popcount(non_pawn_material) >= 2` — `<bit>` is already in the PCH;
   `Board.h` already uses `std::countr_zero`, so `std::popcount` matches idiom).
   - Single-piece endgames (K+R, K+Q, K+minor ± pawns) are exactly the
     zugzwang-prone class; NMP's node savings there are small because those
     subtrees are cheap anyway.
   - Alternative considered — **verification search** (re-search at reduced
     depth when the null fails high): more general, but a larger change to
     `pvs()` and unnecessary for the observed failure class. Deferred; noted
     in Roadmap as a future NMP enhancement.
   - Alternative considered — static-eval ≥ beta gate: standard, but not
     guaranteed to fire here and couples the guard to eval; rejected for now.
2. **Regression test at the fast tier**: QFORK-001 becomes a Catch2
   `[tactical]` test (dedicated TEST_CASE, since the position legitimately has
   two accepted moves and `kFastCases` requires a unique best move). Runs in
   pre-commit and CI.
3. **Close the verification gap**: `Validate-PrePR.ps1` gains a 4th check that
   runs the exe tactical suite (`StratChessEvolved.exe tactical test` from
   `Tests/`, ~1 s) so the suite's 90% threshold is enforced before every PR.
4. **Unit tests for the new guard branch** in `SearchTests.cpp` (`[search]`),
   following the existing one-test-per-guard-branch pattern.

## Files Changed

- `StratEngine/AIPerplex.cpp` — `should_try_null_move()` guard tightened
- `StratChessTests/SearchTests.cpp` — new guard-branch unit tests
- `StratChessTests/TacticalTests.cpp` — QFORK-001 regression TEST_CASE
- `StratChessEvolved/Scripts/Validate-PrePR.ps1` — add exe tactical-suite check
- `Docs/TestDesign.md` — tactical suite status + new verification layer
- `Docs/Roadmap.md` — issue #66 status; verification-search follow-up note
- `.claude/plans/nmp-single-piece-zugzwang-guard.md` — this plan

## Step-by-Step Changes

1. (TDD) Add failing tests:
   - `[search]`: `should_try_null_move` returns **false** on
     `8/8/8/3r4/4k3/8/8/3QK3 b - - 0 1` (Black: lone rook) and on
     `... w - - 0 1` (White: lone queen); returns **true** with two non-pawn
     pieces (e.g. add a knight).
   - `[tactical]`: QFORK-001 at depth 4 must play `d1a4` or `d1b3`.
   Build tests, confirm exactly these fail.
2. Change `should_try_null_move()` final clause from
   `return non_pawn_material != 0;` to
   `return std::popcount(non_pawn_material) >= 2;` (with updated comment).
3. Rebuild; confirm new tests pass; run exe tactical suite → 8/8; run full
   Catch2 fast + extended tiers.
4. Add tactical-suite check to `Validate-PrePR.ps1` (runs from `Tests/`,
   checks exit code).
5. Update `Docs/TestDesign.md` + `Docs/Roadmap.md`.
6. Full `Validate-PrePR.ps1`, dispatch `search-reviewer`, PR.

## Validation Plan

- `.\build.ps1 all` — clean under /W4 /WX
- `StratChessTests.exe [search]`, `[tactical]`, then `.\build.ps1 extended-tests`
- `cd Tests && ../x64/Release/StratChessEvolved.exe tactical test` → 8/8
- UCI probe QFORK-001 at depth 4: bestmove ∈ {d1a4, d1b3} (deeper depths pick
  other moves at bare-material score — pre-existing, NMP-independent; see Root
  Cause)
- Node-count sanity: `[tactical_full]` timings must not blow up (guard change
  only affects ≤1-piece endgame nodes)
- Self-play via `Validate-PrePR.ps1` (also exercises middlegame NMP, which is
  untouched)

## Key Correctness Properties

- NMP never fires at a node whose side to move has fewer than 2 non-pawn
  pieces (zugzwang-prone class), regardless of pawns present.
- Middlegame behaviour is bit-identical: any position where the side to move
  has ≥ 2 non-pawn pieces takes the same NMP decisions as before.
- The K+P no-op test (`[tactical_full]`) still holds: 0 pieces < 2.
- Exe tactical suite and Catch2 tiers can no longer drift silently: the exe
  suite runs in pre-PR, and QFORK-001 runs in the fast tier (pre-commit + CI).
