# History-adjusted LMR reductions — Design

**Issue:** #730 (child B2 of #636)

## Goal

The late move reduction (`AIPerplex.cpp`, `apply_lmr` branch of `pvs()`) depends only on depth and move
number, and ignores everything quiet-move ordering has learned about the move. LMR re-searches are 11.2%
of all nodes (#636 baseline). The #730 triage found that the formula sits on its `max(1, depth - 2)`
cap for almost every eligible quiet move: at the cap, the reduced search runs at depth 1. History and
continuation history are now bounded and two-sided (#651, #664). Letting the move's ordering score
shift the reduction should spend depth on quiet moves that history rates highly, and save it on the
rest.

## Review focus

- **D1, applying the adjustment after the cap.** This is the decision the change depends on. Before
  the cap, the adjustment is swallowed and the change is a silent no-op.
- **Assumption A1, the score scale.** The divisor arms rest on a magnitude of the quiet score at LMR
  sites that nobody has measured yet. If the arms are wrong, the lab reads "0 Elo" for a change that
  never acted.
- **D2, a stale killer score.** A move scored as a killer can reach LMR after singular verification
  replaces the killers. Check that the stated outcome is acceptable.

## Scope

**This change will:**

- Adjust R for LMR-reduced quiet moves by their ordering score, bounded by today's limits.
- Expose the divisor as a tuning field with UCI name `LmrHistoryDivisor`. 0 disables the adjustment.
- Add profile-build counters for how often R hits the cap and how often the adjustment changes R.

**This change will not:**

- Change the base formula `sqrt(d-1)*sqrt(m-1)` or its cap. That is B1, measured separately after this
  change.
- Change LMR eligibility: captures, promotions, killers, checks, PV nodes and in-check nodes stay
  unreduced.
- Add an improving flag (B3), or change history ageing or update rules.

## Decisions

### D1: The adjustment is applied after the cap, and is two-sided

```
R0 = min(max(1, int(sqrt(d-1) * sqrt(m-1))), max(1, d-2))   // unchanged
R  = clamp(R0 - score / divisor, 1, max(1, d-2))             // divisor > 0 only
```

A positive score reduces less. A negative score reduces more, but never past the existing cap, so
`d - 1 - R >= 1` and `R >= 1` still hold.

- **Rejected: before the cap** (`int(raw) - score/divisor`, then clamp). The raw product overshoots
  the cap by several plies at most sites (d = 6, m = 20: 9.7 against 4), so the adjustment would
  vanish.
- **Rejected: reduce-less only.** The two-sided form is already inert where R0 sits at the cap. A
  separate one-sided branch adds code and removes the only place where "reduce more" can act: high
  depth, low move number.

### D2: The signal is the ordering score ScoreMoves already computed

The signal is `scored_idx[si].first`: `history + cont_1ply + cont_2ply` for a quiet move, in
[-3·HISTORY_MAX, +3·HISTORY_MAX]. Reading it needs no table lookup. It is also the score that made the
move late in the first place, so the reduction agrees with the ordering.

- **Rejected: butterfly history alone.** It ignores A3's signal, and it needs a lookup.
- **Accepted mismatch.** Butterfly history halves every iteration and continuation history once per
  search, so one divisor weights the two differently as depth rises. This is the same trade-off
  ordering already accepts.
- **Stale killer score.** ScoreMoves runs before singular verification. If a verification search
  replaces this ply's killers, a former killer reaches LMR carrying a score of 800,000 or 900,000.
  Then `score / divisor` is large and R clamps to 1: the move is reduced as little as possible. That
  is the right treatment for a move that recently refuted a sibling, so it gets no special case. No
  int overflow: |score| <= 1,900,000.

### D3: Divisor as a tuning field; integer division; 0 means off

`TUNING_FIELD(int, lmr_history_divisor, <default>, 0, 1'000'000, true, "LmrHistoryDivisor", true)`

C++ division truncates toward zero, so a score smaller in magnitude than the divisor changes nothing.
That keeps cold or neutral history from moving R.

- **0 is the kill switch.** The branch is skipped, and the search must be node-identical to `main`.
- **Rejected: a shift.** Power-of-two granularity is too coarse to tune.
- **Lab arms.** The candidate binary runs three divisor values as `candidate_arms` against `main`, as
  #502 and #398 did. The values are chosen from A1's probe, so that each arm adjusts a materially
  different share of reductions. The starting guess is 4096 / 8192 / 16384, a change of at most
  ±12 / ±6 / ±3 plies.
- **Default.** The PR is built and checked for equivalence with default 0. After the lab, a one-line
  commit sets the default to the winning arm. That commit is behaviour-identical to the arm that
  was measured. If no arm beats its error bar, the change parks with default 0.

### D4: Profile-build counters

`LmrStats` gains `capped` (R0 == cap), `adjusted_less` and `adjusted_more` (R != R0, by direction). The
counters are compiled out of shipping builds, like the existing LMR counters, and `append_info` prints
them. They measure D1's reach, and they are A1's probe.

## Assumptions I cannot verify from the code

- **A1: score magnitude at LMR sites.** The bound is ±49,152, but the typical value depends on
  gravity equilibrium and ageing. Not verified. It is settled by the D4 counters: a depth-12
  `Compare-SearchProfile.ps1` run per candidate divisor, before the lab. Each arm must adjust a
  non-trivial share of reductions, and the arms must differ from each other.
- **A2: the lab applies `LmrHistoryDivisor` per arm.** Not verified. After the run it is settled from
  the PGN score comments: the arms' games must differ from each other.

## Invariants

- `R >= 1` and `depth - 1 - R >= 1` at every LMR site (the existing assert stays).
- `LmrHistoryDivisor = 0` is node-identical to the merge base at `Threads=1`.
- The search stays deterministic at `Threads=1`. No new persistent write, so the aborted-frame rule is
  untouched.

## Validation

Engine tier, search change.

- **Equivalence:** `Compare-SearchEquivalence.ps1 -After <exe>` with the default-0 build against the merge base.
- **Unit tests:**
  - the option binds over UCI and `search_tuning`, and an out-of-range value is rejected;
  - a nonzero divisor changes the node count on a fixed position, which proves the branch is
    reachable.
- **Screens (#636 Method):**
  - A1's per-arm reach probe;
  - a depth-12 profile screen of the chosen arm against the base: LMR re-search share, node share, reach;
  - depth-14 wall clock over the 200 positions of `Tests/profile-screen.fen`.
- **Strength lab, approved by the owner for #730:** three arms against `main`. Merge when the best
  arm's Elo minus its error bar is above 0. Record the screens beside the lab row in
  `Measurements/ci-per-change.md`.

## Cost

- **Size:** 50–200 lines. Files: `AIPerplex.cpp`, `SearchTuning.def`, `SearchTelemetry.h`, tests,
  `Measurements/ci-per-change.md`, `Docs/Changelog.md`.
- **Blast radius:** Engine tier. One new UCI option, which needs no doc change beyond the changelog.
- **Review:** one code review plus `search-reviewer`.
- **Optional:** the D4 counters, about 15 lines. Dropping them leaves A1 unverifiable before the lab,
  and the risk is a silent no-op.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1: after the cap, two-sided, and why "before" is a no-op | source comment at the R computation |
| D2: stale killer score clamps to R = 1 on purpose | source comment at the R computation |
| D3: 0 = off, truncation toward zero | comment on the `SearchTuning.def` entry |
| Arms, screens, lab result, chosen default | `Measurements/ci-per-change.md`, `Docs/Changelog.md` |
