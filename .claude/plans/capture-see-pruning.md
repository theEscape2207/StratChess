# Capture SEE pruning in the main search — Design

**Issue:** #944

## Goal

`pvs()` never prunes a capture. Frontier futility, LMP and quiet SEE pruning all exclude captures, and LMR never reduces one. Captures that SEE says lose heavily are therefore searched at full depth at every shallow non-PV node. A count-only probe on current `main` puts 4.64% of the depth-12 tree under such captures at depth 1 alone (T = -100), with a beat-alpha rate in the same class as the shipped quiet guard once T <= -200. This change adds a default-off guard that skips them, so the strength lab can measure whether that tree is worth searching. No Elo is forecast.

## Review focus

- **D1, the threshold and arm order.** Margin 100 (A) prunes 3.4x the tree of margin 200 (B) at depth 1, but its pruned moves beat alpha 5.2x as often, and depth 1 is where a prune saves least (about 2 nodes, against the `DoMove`/`InCheck`/`UndoMove` the guard still pays). A has the larger node share, though each depth-1 prune is individually cheap (2.06 nodes against 1.73 at depth 2), and the depth-1 gap drives the arm comparison. Whether that is more Elo is open: B's harm rate matches the shipped quiet guard's, and A's does not. Both arms run (see Validation).
- **D4, the fail-low store.** This reuses the quiet guard's selective alpha floor and common UPPER store, including where that UPPER is later reused without a matching skip. The capture store rates are unmeasured.
- **Assumption 1:** the losing-tier score read is a sound pre-filter for `see_ge(move, -margin * depth)`.

## Scope

**This change will:**

- add a capture SEE guard to `pvs()`'s move loop, behind `CaptureSeePruning` (default off), with `CaptureSeePruningMaxDepth` and `CaptureSeePruningMargin`;
- add a `capture_see` skip counter to `SearchTelemetry`, beside `quiet_see`;
- add tests for each gate, the store and flag-off equivalence, and update `Docs/EngineContracts.md` and `Docs/Changelog.md`.

**This change will not:**

- change quiescence SEE pruning or its margins (#398);
- make SEE pin-aware, reduce captures with LMR, or change capture ordering or the `ScoreMoves` tiers;
- prune at depths above 4 in the first run. The probe counted depths 5-6, and they stay unmeasured in the lab;
- flip the default. That is a follow-up PR, after the lab rules.

## Decisions

### D1: Threshold `-margin * depth`, depths 1-4, margin 100 then 200

A capture is skipped when `!See::see_ge(board, move, -margin * depth)`. The quiet guard's `-margin * depth^2` was rejected for two reasons. SEE values fall on a lattice of 100 cp steps, so a quiet-scale margin such as 20 would read as T = 0 at depths 1-2. And a linear threshold keeps the quiet guard's property that a move skipped at one depth is also SEE-losing at every shallower eligible depth, which D4's store relies on.

`CaptureSeePruningMaxDepth` defaults to 4 (min depth fixed at 1). Beyond depth 4 a skip saves more per prune, but the harm is unscreened, and the first run should not mix in a second variable. `CaptureSeePruningMargin` defaults to 100, for arm A.

Probe stand-alone shares per arm (D2's gate set, depths 1-4): A, margin 100: 4.64 / 0.60 / 0.53 / 0.23%; B, margin 200: 1.38 / 0.52 / 0.32 / <=0.18%.

### D2: Gates

The guard reuses the quiet guard's node-level predicate in shape, as a new `capture_see_pruning_eligible()` beside `quiet_see_pruning_eligible()`:

- the flag is on, `1 <= depth <= CaptureSeePruningMaxDepth`;
- not a PV node, not in check, not an exclusion frame (a verification's fail-low grants an extension, so it must come from a search);
- `zugzwang_safe`, and `|alpha| < Mate_Threshold`.

Per move, before `DoMove`:

- a capture, not a promotion, not the hash move (defence in depth: it sorts first);
- `legal_moves_searched >= 1` and `best_value > -Mate_Threshold`, the quiet guard's terms for keeping the first legal move and the only rescue in a mated line;
- **not a recapture**: eligible when `cont_key[ply] == kNoContinuation || cont_key[ply] % 64 != move.to()`. A frame below a null move has no previous move, so it stays eligible, as in the probe. Recaptures beat alpha more often than other pruned captures: at arm A's thresholds 1.5 / 2.3 / 2.1 / 3.4x (depths 1-4), and at arm B's 2.3 / 2.2 / 2.3x (depths 1-3);
- **sparse-material gate**: `min(men(white), men(black)) >= MATERIAL_PRUNING_MIN_PIECES`, the quiescence pruners' gate, for the same reason: SEE discards the sacrifices whose value is an endgame-scale change. This is insurance. The probe found it inert (17 of 6.09M depth-1 prunes), because `zugzwang_safe` already requires two non-pawn pieces for the side to move. It is computed once per node, in `capture_see_pruning_eligible()`, not once per move;
- the score in `scored_idx[si].first` is a losing capture, read through a new named helper `MoveSorter::IsLosingCaptureScore(score)`. It is true for a score in `[kLosingCaptureTier, kKiller1Score)`: the two killer scores lie between that tier and `kGoodCaptureTier`, so the upper bound must be `kKiller1Score`. The caller keeps its `IsCapture` test. Then SEE last: `!see_ge(board, move, -margin * depth)`.

After `DoMove`: no checking move. In the probe, losing captures that give check beat alpha 1.7x as often as non-checking ones (triage 2026-11-10). The other guards' immediate-draw term (`check_draws(ply + 1)`) is left out, with a one-line comment: a capture resets the halfmove clock, so the child can be neither a repetition nor a fifty-move draw.

The guard runs only where the other three guards' candidates are false, which holds by construction since they exclude captures. Each term is cheap, and SEE runs only on losing-tier captures. Its call volume is unmeasured: the probe's 10.5M (depths 1-4, 247.5M-node tree) counts SEE-losing captures *after* `DoMove`, the legality check and the check filter, while the guard calls SEE before `DoMove`. The quiet guard's 114.2M was measured at its call site, on a different 326M-node tree. A per-prune cost read comes from the bench (Validation).

### D3: The skip runs no child and changes no numbering

The skip mirrors quiet SEE's: `UndoMove`, increment `telemetry.capture_see.skips`, set `capture_see_skipped`, `continue`. It sits after `legal_moves_searched++`, so a skipped capture still counts as a legal move and the LMR and LMP indexes of later quiets are unchanged. It sits before every persistent write: `cont_key`, the node counter, PV, killer, history and TT. A skip runs no child search, so it needs no abort read of its own.

### D4: Fail-low: alpha floor and the common UPPER store, as quiet SEE

A skipped capture was judged to lose material, which bounds no score. A frame with a skip therefore claims alpha, `best_value = max(best_value, original_alpha)`, and stores UPPER through the common path, exactly as `quiet_see_skipped` does. This is one more term in that existing condition.

**A mixed depth-2 frame keeps the existing no-store precedence.** With `LateMovePruningDepth2StoreUpper=false`, a depth-2 frame that also skipped a quiet through LMP and failed low still takes the existing no-store return (alpha, no TT write), whatever capture skips it also made. At the default (`true`) that return is inert.

**Where the selective UPPER is reused.** The depth-monotonic argument covers SEE for the same move on the same board. The stored UPPER is also served where no matching skip happens: on a later visit the same capture may arrive as a recapture (a different incoming move at the same key) or become the hash move, and quiescence reuses a MAIN UPPER of depth >= 1 with no capture guard of its own. This is the same exposure the quiet guard's store already carries (`Docs/EngineContracts.md`), and it is part of the policy the lab tests, not something the guard proves. The rejected alternative is to return alpha without a store. For the quiet guard, withholding the store would have cost 97% of completed non-PV depth-3 fail-low entries and the best move IIR relies on (#399's probe, #940, #972). This guard also fires from the second legal move. Its own store rates are unmeasured. D1's linear threshold keeps the depth-monotonic argument the existing comment states. A searched cutoff is unaffected: it stores LOWER from a real search.

### D5: Options and telemetry

`SearchTuning.def`:

- `TUNING_FLAG(capture_see_pruning_enabled, false, …, "CaptureSeePruning")`
- `TUNING_FIELD(int, capture_see_pruning_max_depth, 4, 1, MAX_PLY, …, "CaptureSeePruningMaxDepth")`
- `TUNING_FIELD(int, capture_see_pruning_margin, 100, 0, 1000, …, "CaptureSeePruningMargin")`

All three are UCI-visible, so the lab arms run on one binary through `candidate_uci_options`. `SearchTelemetry` gets a `CaptureSeeStats` with `skips`, printed in its own `info string` line, like `quiet_see`.

## Assumptions I cannot verify from the code

1. **The losing-tier read selects exactly the SEE < 0 captures for the guard's frames.** *Verified by search-reviewer, 2026-11-24; see below.* `ScoreMoves` puts a capture in the losing tier iff `!see_ge(m, 0)` (`Docs/EngineContracts.md`). A capture with SEE >= 0 also passes `see_ge(T)` for every T < 0, so skipping the SEE call for it changes nothing: the tier read is a filter, never a decision. Still to confirm in code: no other scoring path (staged generation's capture stage, the hash move, a promotion-capture) gives a SEE-losing capture a score outside the tier, or a SEE >= 0 capture one inside it. **Verified** in the code: `ScoreUnordered` (`Sort.cpp`) is the only scoring path. A capture takes its tier from `see_ge(mv, 0)` on the same board the loop reads. Staged generation scores only the quiets it appends, and `OrderRemaining` never rescores. The hash move scores above every tier and is exempt anyway. What remains is a risk from future change (e.g. capture history, #871), so the contract goes in `Docs/EngineContracts.md` beside the `ScoreMoves` tier bullet, and the tier test lives in the named helper.
2. **The probe's shares predict neither wall clock nor Elo.** Node-share screens have not done so before (#634, #664). Pruning reshapes the tree through the TT, ordering and LMR, so the lab decides. Not verifiable before the run.
3. **The lab's Elo sign (GCC, Linux, 10+0.1) carries over to the shipped clang-cl build.** This is a working assumption: no record establishes it in general for search-shape changes. The clang-cl bench supplies local cost evidence only.

## Invariants

- `CaptureSeePruning=false` is node-identical to the merge base at `Threads=1`.
- The first legal move of a frame is never skipped, and a frame with a skip never reports a mate or stalemate it did not prove.
- A skipped capture leaves no persistent write: no `cont_key`, PV, killer, history or TT write, and no node count.
- A frame with a capture SEE skip and no searched move above alpha stores UPPER at alpha, never below. The one exception is a depth-2 frame that also has an LMP skip while `LateMovePruningDepth2StoreUpper=false`: it returns alpha and stores nothing, as today.
- With the flag off, the `capture_see` counter's `info string` line is suppressed at zero, as quiet SEE's is, so flag-off transcripts are identical.
- With the flag on, a skipped capture is SEE-losing at `-margin*d` for every eligible `d` at or below the frame's depth.

## Validation

Engine tier. With the flag off, `Compare-SearchEquivalence.ps1` against the merge base must report IDENTICAL.

Tests (`[search]`, Catch2, via `AIPerlexTestFixture`), each falsified by removing its gate:

- the guard skips a losing capture at depth 1 (counter > 0 on a fixture position with a hanging-to-pawn capture), and the off flag counts zero;
- **threshold shape**: at effective depth 2 with margin 100, a capture with SEE = -300 is skipped (linear: T = -200) where a quadratic threshold (-400) would keep it, and a capture with SEE exactly at the threshold is kept (`see_ge` is inclusive);
- a frame below a null move (`cont_key[ply] == kNoContinuation`) still prunes;
- **direct eligibility tests** for exclusion frame, mate-window alpha and `zugzwang_safe`, following `SearchQuietSeePruningTests.cpp`'s pattern;
- `IsLosingCaptureScore` on real sorted scores: a losing capture, a good capture, both killers and the hash move;
- each exemption is honoured, each by a case that reaches that gate alone: recapture, checking move, PV node, in check, sparse material (below `MATERIAL_PRUNING_MIN_PIECES`), depth above max. The first-legal-move, hash-move and capture-promotion exemptions are redundant behind the mate sentinel, hash ordering and `see_ge(0)` respectively. They are defence in depth and need no isolating test;
- a frame whose searched moves fail **strictly below** alpha and which skipped a capture stores UPPER at exactly alpha, with the searched best move (removing the floor fails it). A searched cutoff after a skip still stores LOWER;
- a mixed depth-2 frame (an LMP skip plus a capture skip, `LateMovePruningDepth2StoreUpper=false`) returns alpha and stores nothing;
- the counter is appended after the existing ones, reset and summed across threads, and its `info string` line is absent when it is zero.

A paired flag-on/flag-off fixed-depth series on one binary is recorded in the PR (a cost read, not a gate). `Run-Bench.ps1` cannot set a UCI option, so use a small option-capable UCI driver, as #399 did. Verify the option is acknowledged and the skip counter is non-zero, and report elapsed time together with node totals. This is separate from the flag-off-against-merge-base throughput read. A node-count change is expected with the flag on. A flag-on depth-12 run of the probe's 200 positions checks the wiring: the `capture_see` skip count should be the same order as the probe's matgate prunable counts at margin 100, depths 1-4 (6.92M).

**Elo: the strength lab decides**, run on the PR's head commit (owner-approved 2026-11-24):

- **Same commit on both sides.** `reference_ref` is the PR head's full SHA, with default options (flag off), so the run measures the option alone and not code layout or throughput differences against the merge base. Flag-off equivalence against the merge base is the separate gate above. B reuses the same candidate and reference SHA even if it runs later. The run record must show the resolved SHAs and the applied options.
- **Arm A:** `candidate_uci_options: CaptureSeePruning=true CaptureSeePruningMargin=100`, the fixed Normal batch (`games=20088`, `shards=18`, `sprt=false`), 10+0.1, `Threads=1`. About 3-3.5 h.
- **Arm B:** the same with `CaptureSeePruningMargin=200`, run whatever A's result, either straight after A or later as an idle-lab run (the owner's call).
- **Gate exception (owner, 2026-11-24).** The standing ship gate is the 26,568-game SPRT. The owner chose fixed Normal batches for both arms instead ("no benefit in running this as a waved SPRT"), so these batches are the gate for this experiment. The owner may still require the standing gate before the default flips; that is decided in the morning, from the results.
- **Rules, stated before dispatch.** Use each run's reported error bar (planning figure about ±3.5 Elo).
  - An arm **passes** if Elo - err > 0.
  - An arm with 0 < Elo <= err is **inconclusive**, not a failure. Whether to run a confirmation is the owner's call.
  - An arm with Elo <= 0 **fails**.
  - If both pass, the higher point estimate picks the arm. That estimate is a selection result, biased upward. The owner may overrule the pick for B's lower harm rate. If one passes, the default flips to that arm.
  - One fail plus one inconclusive leaves the default off, pending the owner's decision.
  - If both fail, delete the guard, its options and its tests, and record that depths 5-6 were counted but never lab-tested.
  - Every run is recorded in `Measurements/ci-per-change.md`, failures included.

## Cost

- **Size:** 50-200 lines plus tests across about 7 files: `AIPerplex.{h,cpp}`, `SearchTuning.def`, `SearchTelemetry.h`, a test file, `Docs/EngineContracts.md`, `Docs/Changelog.md`.
- **Blast radius:** engine tier, with new UCI options. Search is unchanged with the flag off.
- **Review:** `search-reviewer` on this doc and the diff, a Codex review of this doc, and one code review (170-270k tokens, 3-5 min per run).
- **Optional part:** the recapture exemption, under 5 lines (approved, kept). It covers the 2x harm class.
- **Paid measurement:** one lab run per arm, about 3-3.5 h each (approved).

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 linear threshold and its monotonicity, D4 store | source comment at the guard and at the alpha-floor condition in `pvs()` |
| D2 gates and the recapture and checking-move rates | source comment at the guard |
| Losing-tier pre-filter contract (Assumption 1), `IsLosingCaptureScore` | `Docs/EngineContracts.md`, beside the `ScoreMoves` tier bullet |
| Probe tables and the lab result | issue #944 comment; `Measurements/ci-per-change.md` row |
| Options | `Docs/Changelog.md`; the UCI option table if one lists tuning options |
