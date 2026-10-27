# TT mate-band width — Design

**Issue:** #305

## Goal

`GameValues::Mate_Threshold` (29900) classifies a score as a mate, but a mate found `d` plies from
the root scores `Mate - d`, and `MAX_PLY` is 256. Beyond `d = 100` the score falls below the threshold,
so `TranspositionTable::store()` and `probe()` take the ordinary-score branch: the stored value is
not rebased to the node and a later probe at another ply returns the wrong distance (reproduced
by the triage: store at ply 101, probe at ply 7 returns 29899, not 29993). Widening the band to the
whole supported ply range removes the silent branch; an assertion alone would leave the wrong
answer in Release.

## Review focus

- **A1/D2**: static eval stays below the widened band by code-derived bounds, not by typical positions.
- **D3**: every other consumer of the threshold keeps its policy (notably the qsearch refusal) when
  deep mates join the band.
- **A2**: a TT hit can compose `probe_ply + mate_distance_from_node` past `MAX_PLY`; that residual
  is accepted, not fixed.

## Scope

**This change will:**

- Set `Mate_Threshold = Mate - MAX_PLY` (29744) and static_assert its relation to `MAX_PLY`, `Mate`
  and the int16 TT field.
- Correct stale comments (qsearch's "MAX_PLY keeps that out of reach", the TT's storage comment).
- Add the regression tests that fail on current main.

**This change will not:** raise `MAX_PLY`; change mate-distance arithmetic, TT packing or layout,
`Mate` or `Search_Init`; alter evaluation weights; change qsearch's cutoff policy; or touch the
out-of-domain `Search_Init` UCI formatting (no upper guard; unchanged, see Invariants).

## The three score domains

1. **Finite centipawn**: static eval and the scores built from it. `|s| < Mate_Threshold`. Stored
   verbatim.
2. **Root-relative mate**: `±(Mate - d)` with `d` plies from the root (`pvs()` at ply `d` returns
   `-Mate + d` when mated). Exists for `1 <= d <= MAX_PLY - 1` . pvs() and quiescence() return
   Evaluate() from ply 255, so terminals occur at `d <= 254` and `|s| >= Mate - 254 = 29746`.
3. **Normalised storage**: `±(Mate - k)` with `k = d - store_ply` plies from the storing node,
   `0 <= k <= 254`, so `|s| <= Mate = 30000 < INT16_MAX` and `>= 29746`. Store adds `ply` to a
   domain-2 value, probe subtracts the probe ply.

The classification must be one predicate with one boundary, so domain 2 and 3 are both entirely at
or above the threshold and domain 1 entirely below. `Mate - MAX_PLY` (29744) has a two-ply margin
over the deepest reachable mate (29746).

## Decisions

### D1: `Mate_Threshold = Mate - MAX_PLY`, derived in the enum

Keeps one shared band (the audit shows twelve consumers (eleven predicates plus the `store()` static-eval assertion) use the same predicate) and the capacity
meaning is stated: the band holds every mate distance the ply arrays can address. Rejected: a TT-only
wider threshold (two predicates would disagree on a score 29744..29899 for pruning versus storage),
and raising the threshold's cost by making the TT store a separate mate flag (changes packing,
a non-goal).

### D2: static-eval disjointness from code bounds

`Evaluate()` is not clamped, so the argument is bounded term by term. A static score is
`(mat_w + pos_w) - (mat_b + pos_b)`, side-signed.
- Material: `g_iPieceValues` Q900 R500 B300 N300 P100 K10000. The king value is in
  `material_score_` but cancels with both kings on board. Largest non-king material per side:
  16 men, 8 pawns can promote, so at most 9Q+2R+2B+2N = 8100+1000+600+600 = 10300. Without the
  cancelling king (kingless boards exist only in tests) the largest imbalance is 10300+10000 = 20300.
- Positional, per colour: pawns, rooks, bishops, outposts, castling, mobility, PST, mop-up, king
  safety. Bounded in `Eval.h` from the live constants over at most 16 men: max PST entry 60 x 16,
  mobility 15 pieces x 27 squares x weight 4, pawns, ten rooks and their pairs, outposts, and
  `KING_SAFETY_MAX_PENALTY`: about 5450 per colour.
- Total with both kings: about 10300 + 2 x 5450 = 21200, against 29744. The endgame scale only
  shrinks a score; the dead-draw score is the contempt option, capped at 100.

The bound is a `static_assert` in `Eval.h` summing those constants, so a retune that outgrows it
fails to compile; it covers positions with both kings (a kingless board never reaches the search).
The debug assertion in `store()` stays as the runtime check on the value actually handed in.

### D3: consumers keep their policy

All consumers compare `abs(x) >= Mate_Threshold`; widening moves 145 points of range from "ordinary"
to "mate" and changes no policy:

| Consumer | Effect |
|---|---|
| `TranspositionTable` store/probe | fixes the defect |
| singular eligibility (`AIPerplex.cpp`) | mate-range hash value now refused at distances 101-254; was margin arithmetic against a mate value |
| qsearch mate-cutoff refusal | now refuses deep mates too; the stale "unreachable" comment is rewritten. Policy unchanged |
| null move / reverse futility / frontier futility / LMP guards | refuse when the window is in the mate band, now to 254 plies |
| `singular_multicut_eligible` | same guard |
| emergency handling (`handle_empty_move_emergency`) | deep mate scores no longer mistaken for a no-mate case |
| `IterationPolicy` MATE stop | stops on a deep mate (previously kept iterating). A real behaviour change, accepted: such scores arise only from TT chains and match the policy already applied to d <= 100 |
| `Game::PrintStateMessage`, `format_uci_score` | report `mate N` for a distance up to 128 moves |

`Search_Init` (50000) is above `Mate` in magnitude; the lower bound moving adds no new sentinel
exposure, and the formatter's missing upper guard is unchanged.

## Assumptions I cannot verify from the code

- **A1**: positional terms stay under the bound in D2 for every legal position. Verified by the
  compile-time sum and the existing eval-term corpus bound (`EvalTermTests`); a position that
  broke it would need ~19000 cp of positional score beyond 16 men.
- **A2**: a TT entry stored at ply `s` with distance `k` can be probed at ply `p` with `p + k > 254`
  (a transposition into a longer path). The probed value is then below the threshold and reads as
  ordinary. Cannot arise from a single search line (the ply backstop is 255); accepted, since closing it
  (clamping the denormalised value) changes mate arithmetic, a non-goal. The window is about 150 plies
  narrower than before. Not measured.

## Invariants

- `|Evaluate()| < Mate_Threshold`; `Mate_Threshold == Mate - MAX_PLY`; `Mate + MAX_PLY` fits int16.
- Store then probe at any ply pair returns `sign * (Mate - k')` for every mate within the supported
  range; centipawn scores keep their value.
- Qsearch never takes a cutoff on a mate-band TT value.

## Validation

Engine tier. Run on unmodified main (Release), the new tests failed: 8 test cases, 48 assertions,
covering the cross-ply TT probes, the terminal pvs storage, the qsearch deep-mate refusal, the
literal band and the frontier and iteration-policy boundaries. After the change they pass in Release
and Debug, and `Compare-SearchEquivalence.ps1` against the merge base is IDENTICAL (6 positions,
depth 12, 120 lines). Regression tests (literal expected values) written first and shown failing on
unmodified main: both signs at distance 99/100/101 and 255, cross-ply both directions, centipawn
scores at 29743 / -29743, terminal pvs stored at ply 101 and 254 probed at 7, qsearch refusal of a
deep-mate entry. `[tt]`, `[search]`, `[qsearch]`, `[limits]`, `[uci]`, `[eval]` in Release and Debug,
Run-Lint, `Compare-SearchEquivalence.ps1` against the merge base (differences must be explained by
corrected mate classification), `Validate-PrePR.ps1`. No Elo match: arithmetic correction, no
per-node cost.

## Cost

Size under 200 lines across about 8 files. Engine tier. One code review and one search-review.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| three score domains and the band's capacity meaning | comment on `Mate_Threshold` in `defines.h` |
| static-eval bound | static_assert comment in `Eval.h` |
| qsearch refusal covers deep mates | `quiescence()` comment |
