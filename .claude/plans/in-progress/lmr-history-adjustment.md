# History-adjusted LMR reductions — Design

**Issue:** #730 (child B2 of #636)

## Goal

The late move reduction (`AIPerplex.cpp`, `apply_lmr` branch of `pvs()`) depends only on depth and move
number, and ignores everything quiet ordering has learned about the move. In #636's historical
baseline (`af2f996`, before A1 and A3), LMR re-searches were 11.2% of all nodes. The #730 triage
calculated that the formula is predicted to sit on its `max(1, depth - 2)` cap for most eligible
quiets, which means a depth-1 reduced search. That calculation weights move numbers uniformly; the
real share is pending D4. History and continuation history are now bounded and two-sided (#651,
#664). Letting a move's ordering score shift its reduction should spend depth on quiets history rates
highly, and save it on the rest.

## Review focus

- **D1, applying the adjustment after the cap.** Before the cap, the overshoot of the raw product
  attenuates or swallows the adjustment.
- **A1, the score scale.** The divisor arms rest on how large ordinary quiet scores are at LMR sites,
  which is unmeasured. If the arms are inert, the lab reads "0 Elo" for a change that never acted.
  D4 and D5 are what close it.
- **D2, a stale killer score.** A move scored as a killer can reach LMR after singular verification
  has replaced the killers.

## Scope

**This change will:**

- Adjust R for LMR-reduced quiets by their ordering score, within today's bounds (D1, D2).
- Expose the divisor as a tuning field with UCI name `LmrHistoryDivisor`; 0 disables it (D3).
- Put the reduction in a pure function that the tests drive directly (D6).
- Add profile-build counters for cap and adjustment reach, and teach `Compare-SearchProfile.ps1` to
  parse and report them (D4).

**This change will not:**

- Change the base formula `sqrt(d-1)*sqrt(m-1)` or its cap. That is B1, measured after this change.
- Change LMR eligibility, including behaviour at `lmr_min_depth` 1 or 2.
- Add per-side UCI option input to `Compare-SearchProfile.ps1`. Probe binaries are built with a
  different compiled default instead (D5).
- Add an improving flag (B3), or change history ageing or update rules.

## Decisions

### D1: Applied after the cap, two-sided

```
cap = max(1, d-2)
R0  = min(max(1, int(sqrt(d-1) * sqrt(m-1))), cap)   // unchanged
R   = clamp(R0 - score / divisor, 1, cap)            // only when divisor > 0
```

A positive score reduces less. A negative score reduces more, but only where R0 is below the cap.
Because `cap` is the existing upper bound, behaviour at `lmr_min_depth` < 3 is unchanged: there
`cap` is 1, so R stays 1, as R0 already was.

- **Rejected: adjusting before the cap** (`int(raw) - score/divisor`, then clamp). The raw product
  often overshoots the cap by several plies. At d = 6, m = 20, `int(9.75)` = 9 against a cap of 4, so
  an adjustment of a few plies is swallowed. It is not always a no-op: a large enough score still
  acts. But it wastes the adjustment against the overshoot.
- **Rejected: reduce-less only.** The two-sided form is already inert wherever R0 is at the cap. A
  separate one-sided branch adds code, and it removes the only place where "reduce more" can act:
  high depth with a low move number.

### D2: The signal is the ordering score ScoreMoves already computed

The signal is `scored_idx[si].first`. For an ordinary quiet it is `history + cont_1ply + cont_2ply`,
in [-49,152, +49,152] (3·HISTORY_MAX). Reading it needs no table lookup. It is also the score that
made the move late, so the reduction agrees with the ordering. The snapshot predates any history
update made earlier in this node's loop. That is accepted, like the ordering itself.

- **Rejected: butterfly history alone.** It drops A3's signal and needs a lookup.
- **Accepted mismatch.** Butterfly history halves every iteration and continuation history once per
  search, so one divisor weights the two differently as depth rises. Ordering already accepts the
  same trade-off.
- **The stale-killer exception.** ScoreMoves runs before singular verification. If verification
  replaces this ply's killers, a former killer reaches LMR carrying a score of 800,000 or 900,000.
  It follows the same formula, with no special case. At the arm values in D5 the quotient is large,
  so R clamps to 1: the move is reduced as little as possible, which suits a move that recently
  refuted a sibling. At large divisors (for example 1,000,000) the quotient can be smaller, down to
  0. There is no int overflow risk: |score| <= 1,900,000.

### D3: Divisor as a tuning field; integer division; 0 means off

`TUNING_FIELD(int, lmr_history_divisor, <default>, 0, 1'000'000, true, "LmrHistoryDivisor", true)`

C++ division truncates toward zero, so a score smaller in magnitude than the divisor changes nothing,
and cold or neutral history leaves R alone. When the divisor is 0 the adjustment is skipped, and the
search must be node-identical to `main`.

- **Rejected: a shift.** Power-of-two granularity is too coarse to tune.
- **Default:** the PR is built and checked for equivalence at default 0. After the lab, a one-line
  commit sets the default to the selected arm. Only that field changes, so the commit is
  behaviour-identical to the measured arm. If no arm beats its error bar, the change parks with
  default 0.

### D4: Reach telemetry, required

`LmrStats` gains four counters:

| Counter | Counts |
|---|---|
| `capped` | R0 == cap |
| `adjusted_less` | R < R0, ordinary score |
| `adjusted_more` | R > R0, ordinary score |
| `killer_adjusted` | R != R0 with \|score\| > 3·HISTORY_MAX: the stale-killer exception |

`reduced` is the denominator for all four. They print on a new optional line, after the `lmr` line
and only when `reduced != 0`:
`info string lmrhistory capped C less L more M killer K`. The existing `lmr` line is unchanged, so a
baseline build still parses.

`Compare-SearchProfile.ps1`:

- adds `lmrhistory` to `$ProfileSchema` as optional;
- reports the four rates as a percentage of `reduced`;
- shows a side that lacks the line as **n/a, not 0**. A baseline that never printed the line has
  not shown zero reach. Other optional lines keep their current "absent reads as zero" rule.

`-SelfTest` covers parsing, the n/a rendering and a malformed `lmrhistory` line. The exact-output
fixture in `StratChessTests/SearchTelemetryTests.cpp` and the telemetry contract in
`Docs/Engine-Readme.md` gain the new line.

### D5: Choosing the arms, before the lab

The candidate PR's default is 0, so probe binaries are throwaway profile builds that differ only in
the compiled default of `lmr_history_divisor`. Probing them with `Compare-SearchProfile.ps1 -After
<probe exe> -Depth 12 -Positions Tests/profile-screen.fen` needs no option plumbing. The same holds
for the "before" side, which is the merge base.

- **Starting guesses:** 4096, 8192 and 16384. For ordinary scores, the pre-clamp quotient is at most
  ±12, ±6 and ±3.
- **Arm acceptance:** an arm qualifies when its ordinary reach, `(less + more) / reduced`, is at
  least 2%. Arms must differ from each other by at least a factor of 1.5 in ordinary reach. If an arm
  falls below 2%, halve its divisor and probe again.
- Record each arm's counts and rates in the PR before the lab is dispatched.
- **Outcome (2026-10-05).** The base sits at the cap for 99.0% of reductions, and "more" never acts
  at practical divisors. Late quiets carry small ordering scores, so the starting guesses reached
  0.0005-0.14%. After halving, the selected arms are **512 (2.24% reach), 256 (3.42%) and 64
  (5.81%)**. 128 (4.59%) is not 1.5x from 256. At these divisors the ordinary quotient is no longer
  bounded to a few plies: any score of at least the divisor reduces less, down to R = 1.

### D6: The reduction as a pure function

`lmr_reduction(depth, move_number, score, divisor)` is an inline free function in
`AIPerplex.h` that returns R. It holds both today's formula and D1, and `pvs()` calls it. The tests
drive it directly. A node-count difference on one position could not tell D1 apart from any other
change that alters behaviour.

## Assumptions I cannot verify from the code

- **A1: ordinary score magnitude at LMR sites.** The bound is ±49,152; the typical value depends on
  gravity equilibrium and ageing. Not verified. D5's probe settles it before the lab.
- **A2: the lab applies `LmrHistoryDivisor` per arm.** The transport is verified statically:
  `strength.yml` passes arms to `compare_lab_configs.py`, selects `ARM_OPTIONS`, builds
  `option.Name=Value` and hands that to fastchess. The engine side is an implementation obligation,
  covered by the UCI binding test. After the run, the resolved comparison artifact and the shard
  arm/option logs confirm each arm's value. PGN score comments do not, because the arms play
  disjoint opening ranges.

## Invariants

- For `depth >= 3`: `1 <= R <= depth - 2`, so `depth - 1 - R >= 1`. Below depth 3, R is 1 as today,
  and the existing assert's `lmr_min_depth < 3` exemption stays.
- `LmrHistoryDivisor = 0` is node-identical to the merge base at `Threads=1`.
- The search stays deterministic at `Threads=1`. There is no new persistent write, so the
  aborted-frame rule is untouched.

## Validation

Engine tier, search change.

- **Equivalence:** `Compare-SearchEquivalence.ps1 -After <exe> -BaselineRef 2201b87`, with the default-0
  build.
- **Unit tests of `lmr_reduction`:**
  - divisor 0 equals today's formula across a grid;
  - a positive score at the cap reduces R, which proves D1's placement;
  - a negative score below the cap raises R, but never past the cap;
  - truncation in both directions: score = ±(divisor − 1) gives no change, score = ±divisor gives
    one ply;
  - divisor 1 and divisor 1,000,000;
  - a killer score (900,000) at each D5 arm gives R = 1;
  - depth 1 and depth 2 give R = 1.
- **UCI and `search_tuning`:** the option binds, and an out-of-range value is rejected.
- **`Compare-SearchProfile.ps1 -SelfTest`** passes with the D4 additions.
- **Screens (#636 Method):**
  - the D5 arm probes;
  - then, for the arm the lab selects, a depth-12 screen with `-Seeds 8`;
  - depth-14 wall clock over the 200 positions, in chunks of 10 with the build order rotated per
    chunk, reported as the mean per-position paired time ratio ±2 SE.
- **Strength lab, approved by the owner for #730:** three arms against `main`. Merge when the
  selected arm's Elo minus its error bar is above 0. The selected arm's estimate is labelled
  best-of-three: its error bar is not corrected for selection. Record the screens beside the lab row
  in `Measurements/ci-per-change.md`.

## Cost

- **Size:** about 200 lines, possibly over.
  - Engine (`AIPerplex.cpp/.h`, `SearchTuning.def`, `SearchTelemetry.h`): about 60.
  - Tests: about 80.
  - `Compare-SearchProfile.ps1` and its self-test: about 50.
  - Docs (`Engine-Readme.md`, `Changelog.md`, `Measurements/ci-per-change.md`).
- **Blast radius:** Engine tier. One new UCI option, one new profile-telemetry line.
- **Review:** one code review plus `search-reviewer`.
- **Optional:** none. D4 is required, because it is the only evidence for A1.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1: after the cap, two-sided; adjusting before the cap is swallowed by the overshoot | comment on `lmr_reduction` |
| D2: stale killer follows the formula; R = 1 at the arm values | comment on `lmr_reduction` |
| D3: 0 = off, truncation toward zero | comment on the `SearchTuning.def` entry |
| D4: n/a vs 0 for an absent `lmrhistory` line | `Compare-SearchProfile.ps1` help, `Engine-Readme.md` telemetry contract |
| Arms, reach probe, screens, lab result, chosen default | `Measurements/ci-per-change.md`, `Docs/Changelog.md`, PR body |
| Review dispositions (finding 1's option plumbing rejected, D5) | PR body |
