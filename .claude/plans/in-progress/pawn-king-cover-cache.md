# Pawn / King-Cover Cache — Design

**Issue:** #131 · **Epic:** #110 Tier 4 · Replaces the sketch in `not-started/pawn-hash-table.md`

## Goal

The king-safety series (#97) bought +32.81 ± 3.79 Elo and paid roughly 8% of nps for it — about
5.5% for the shelter/storm/openness scan in `eval_king_pawn_cover` and 2.5% for the attack term.
Two rounds of micro-optimisation bounded what shaving can recover; what is left is the scan itself,
three files for both colours at every evaluated leaf. But that scan is a **pure function of
`{own pawns, enemy pawns, king square, colour}`**, and a search subtree changes none of those for
long stretches. Caching its result per thread removes the work rather than shaving it. At the
project's calibration (1% nps ≈ 1.7 Elo) the ~5.5% at stake is worth ~9 Elo, which is larger than
the swing PR 4 of #97 is being run to resolve — which is why this lands first.

## Scope

**This change will:**

- Add `StratEngine/PawnCache.h`: a small direct-mapped table whose entries are tagged by the exact
  `{white_pawns, black_pawns, king_square, colour}` they were computed from.
- Own one table per `ThreadData`, passed explicitly into evaluation by search.
- Return `eval_king_pawn_cover`'s three contributions from the table on a hit (**PR 1**).
- Extend the same table to a pawn-only subtotal of `eval_pawns`, which requires splitting that term
  into a cacheable part and a dynamic blockade adjustment (**PR 2**, gated separately and dropped if
  it does not pay).
- Sweep table size and report hit rate and bytes per thread rather than asserting a size.

**This change will not:**

- Change any score, node count, best move or PV. This is a cache; any observable difference is a
  defect, not a trade-off, and no Elo match is run to "confirm" one that cannot exist.
- Add a pawn-Zobrist accumulator to `Board` (D2), or any `DoMove`/`UndoMove` bookkeeping.
- Make `EvalManager` or `EvalComplex` stateful. The Lazy SMP sharing contract in `Eval.h` survives
  unmodified, including its wording.
- Share one table across threads. Per-thread ownership is the whole reason the contract survives.
- Cache the attack term (`eval_king_attack`). It reads piece placement, which the key does not
  cover, and its 2.5% is not addressable this way.
- Cache anything read by the `eval` command, the term-level test fixture, or `Breakdown()`. Those
  keep the uncached path, which is also what makes cached-vs-uncached comparison possible at all.
- Retune anything. #117 owns weights; this change must not move a single score.

## Decisions

### D1: King cover is cached first, in its own PR; the pawn subtotal follows in a second

`eval_king_pawn_cover` is cacheable as it stands — it reads `ctx.pawns[both]`, `ctx.king_sq[colour]`
and `ctx.phase`, nothing else — so the term with the largest measured price needs no restructuring
to cache. `eval_pawns` does: its passer bonus consults `ctx.occupied[enemy]` on each passer's stop
square (`Eval.cpp:148`), so the complete pawn score is **not** a function of pawn placement alone.
Caching it means splitting it into a pawn-only subtotal plus a blockade adjustment reapplied after a
hit, which needs the passer set carried in the payload.

Two PRs, each with its own gate, rather than one:

| PR | Content | Gate |
|---|---|---|
| 1 | `PawnCache`, per-thread ownership, `eval_king_pawn_cover` lookup | Exact equivalence, then ≥ +2% nps |
| 2 | `eval_pawns` split; pawn-only subtotal cached | Exact equivalence, then ≥ +2% nps **incremental over PR 1** |

The split is not bureaucracy: PR 1 is a pure addition that changes no term's logic, while PR 2
restructures a scoring function, and bundling them would leave a failed equivalence check
unattributable between a cache bug and a bad split. PR 2 is also the one that may not be worth
doing — #248's evidence for the #116 pawn terms is a −1.5% to −7.6% band contaminated by a changed
search tree, so its recoverable share is genuinely unknown, and it must clear the threshold on its
own or be dropped with the measurement recorded.

Rejected: caching the pawn subtotal alone, which is what the issue text proposes. It does not touch
the cost this step exists to recover.

### D2: Entries are tagged by exact bitboards, not by a pawn Zobrist key

The two pawn bitboards are already an exact 128-bit identity of the pawn structure. Mixing them (with
the king square) selects the slot; the full tag is then **compared**, so an index collision is a miss
and an overwrite, never a wrong score. A 64-bit pawn Zobrist key compared alone would make a
collision a silently wrong evaluation, and maintaining one incrementally would add a second
accumulator invariant to `Board::add_piece` / `remove_piece` / `move_piece` — a new way for
`DoMove`/`UndoMove` to be subtly wrong, bought for nothing.

This reverses the sketch in `not-started/pawn-hash-table.md`, which proposed the accumulator. The
sketch's premise — that hashing on demand is too expensive — does not apply when the bitboards are
the tag and are already in the context.

Rejected: a Zobrist key plus a verification field (the cost of both with the benefit of neither);
and a key over full occupancy, which would make every ordinary piece move destroy the reuse this
exists to capture.

### D3: One entry per colour, keyed `{white_pawns, black_pawns, king_sq, colour}`

Two probes per evaluated node, one per colour, matching how the term is already computed. The
alternative — one entry holding both colours, keyed on both king squares — halves the probes but
evicts both colours' work whenever either king moves, and the two colours' results are otherwise
independent. Colour is part of the key because the tables are defender-relative and the `ahead` mask
depends on it: the same pawn structure scores differently for White and Black.

Colour packs into the same tag byte as the square (6 bits of square, 1 of colour), so one table
serves both and there is one allocation per thread.

### D4: The cache is a parameter, not an `EvalContext` field

`EvalContext` is documented as a pure per-call description of the position, and PR 1 of #97 measured
a single *unused* 8-byte field in it at ~0.6% nps — the struct is close enough to a cache-line budget
that adding a pointer to it is not free. The cache therefore travels as an explicit
`PawnCache*` parameter through `RawWhitePov` into `eval_king_pawn_cover`, `nullptr` meaning "compute
it". `Breakdown()` and the term-level fixture pass `nullptr` and are unaffected.

The search-facing entry point is a **new name**, `EvalComplex::EvaluateCached(const Board&,
PawnCache&)`, not a second `Evaluate` overload: overriding one overload of `Evaluate` in a derived
class hides the other in the base, and the resulting call would resolve by static type without a
diagnostic. `EvalSimple` gets no cached path — it has no term that would use one.

`AIPerplex::quiescence`'s three `Evaluate` call sites (`AIPerplex.cpp:843,869,924`) become
`EvaluateCached(td.board, td.pawn_cache)`. That is the only production call site: static evaluation
is confined to quiescence, which also bounds the size of the prize.

### D5: The probe sits after the term's existing guards, and phase is not in the key

`eval_king_pawn_cover` returns a zeroed `KingPawnCover` for a kingless board or at `phase == 0`
before reading anything. Both guards stay in front of the probe: they are exact, they cost one
comparison, and caching their result would put phase into a key that otherwise needs only pawns and
a square. Everything below the guards is phase-independent — the three contributions are `{x, 0}`
pairs and `BlendPhase` is applied by the caller — so the payload is three `int16_t` magnitudes and
the key stays 17 bytes.

`BuildContext` already zeroes the piece aggregates at `endgame_scale == 0` and `Evaluate()` returns
`Draw` there without calling any term, so no probe happens on that path either.

### D6: Direct-mapped, overwrite on mismatch, size chosen by sweep

No replacement policy, no ageing, no generation counter: on a tag mismatch the entry is recomputed
and overwritten. Every entry is equally valid whenever its tag matches, so the only thing a policy
could buy is a better hit rate under thrash, and the access pattern here — long runs sharing one
pawn structure — does not thrash.

Size is **measured, not asserted**: 256, 1,024 and 4,096 entries, recording aggregate nps, hit rate
and bytes per thread for each, and the smallest size at the plateau ships. At ~24 bytes per entry
that is 6 KB / 24 KB / 96 KB per thread, and Lazy SMP allocates up to 32 copies. A larger table with
a worse cache footprint can easily be slower than a smaller one, which is exactly why the sweep
exists rather than "a few thousand is plenty".

### D7: Hit-rate instrumentation is compile-time gated and off in every measurement

Probe/hit counters live behind a `constexpr bool` defaulting to false, so the counted build is a
development tool and the shipping build has no branch. They are read once during the size sweep and
never used to justify the merge — nps is what decides that. `#ifdef` in the hot path is avoided:
a `if constexpr` on a compile-time-false flag leaves the surrounding code readable and generates
nothing.

## Assumptions I cannot verify from the code

- **A search subtree reuses one pawn structure and king square often enough for a direct-mapped
  table to hit.** Universally true of engines, not measured here. Verified by the hit-rate counters
  in D7 during the sweep; a hit rate below ~90% means the key or the size is wrong, and is a reason
  to stop rather than to grow the table.
- **The ~5.5% `eval_king_pawn_cover` costs is mostly the scan, so removing the scan recovers most of
  it.** The probe, tag compare and store are not free, and evaluation happens only in quiescence, so
  the recoverable share is bounded below 5.5% by an unknown amount. This is precisely what the +2%
  gate tests; it is not assumed.
- **The 1.7 Elo per 1% nps conversion.** The project's existing calibration, reused rather than
  re-derived. Nothing in this change depends on it beyond framing the prize — the gate is stated in
  nps, not in Elo.
- **Two probes per node is cheaper than one scan.** Not verifiable without measuring, and if it is
  false the whole change fails its gate and is closed with the data, which is a successful outcome
  for this issue rather than a failure.

## Invariants

- **No searched score changes.** Cached and uncached `Evaluate()` are identical over the evaluation
  corpus, and at `Threads=1` node counts, best moves, scores and PVs are identical.
- A tag mismatch never returns a stored value. Index collisions are possible by construction; wrong
  answers are not.
- `EvalManager` and `EvalComplex` remain stateless and safely shared unsynchronized across Lazy SMP
  threads — the contract comment in `Eval.h` is still true, word for word, after this change.
- Every table is reachable from exactly one thread. No synchronization exists because none is needed,
  and that is only sound while ownership is per-`ThreadData`.
- The uncached path stays live and is what the tests compare against; it is not a fallback that
  could rot.
- A kingless board and `phase == 0` still score zero from all three contributions, without a probe.

## Validation

Tooling tier is Engine. Equivalence is proved **before** any speed number is read, because a fast
wrong answer is not a result:

- New `[eval]` cases, with matching `Docs/TestDesign.md` entries: identical tags for identical pawn
  placement with different piece placement; distinct tags after a pawn move, capture and promotion;
  a forced index collision returning the recomputed value rather than the resident one; cached and
  uncached agreement across the corpus including mirrors and the kingless board; and a king moving
  with the pawn structure unchanged, which must miss.
- For PR 2 additionally: a non-pawn blocker moving onto and off a passer's stop square while the pawn
  tag is constant — the cached score must follow the blocker. This is the case the split exists to
  keep correct, so it is written first and falsified against the unsplit code.
- `Compare-SearchEquivalence.ps1 -After <exe>`: identical nodes, best move, score and PV at
  `Threads=1`.
- `Threads>1` under the Linux Debug + sanitizer gate, to show ownership really is per-thread.
- Every new test is falsified by disabling the cache and watching it fail.

Then speed, on the shipping clang-cl toolchain, at a depth where every bench position clears the
200 ms timing warning, with alternating order-balanced runs and the spread reported:

- **Merge at ≥ +2% aggregate nps**, repeatable, with no material per-position regression. Node counts
  and best moves must be identical first, which is what makes aggregate nps a valid comparison here.
- **Close at < +1%**, or anywhere inside run-to-run spread, recording the sweep and the memory cost.
  1–2% is a judgement band; report the spread and decide rather than rounding up.

**No SPRT and no strength match**, for either PR. Behaviour is identical by construction, so a match
would be measuring the harness. If behaviour is *not* identical, that is a cache bug and measurement
stops.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Exact bitboard tags mean an index collision is a miss, never a wrong score (D2) | comment on the `PawnCache` entry type |
| Why no pawn-Zobrist accumulator was added to `Board` (D2) | same comment, plus issue #131's closing note |
| Per-colour entries and why colour is part of the key (D3) | comment on the key/tag helper |
| The cache is a parameter, not an `EvalContext` field, and the ~0.6% that decided it (D4) | comment where the parameter enters `RawWhitePov` |
| `EvaluateCached` is a distinct name because an `Evaluate` overload would be hidden in the derived class (D4) | comment on the declaration in `Eval.h` |
| Per-thread ownership is what preserves the Lazy SMP contract (D4) | comment on the `ThreadData` member, pointing at the contract comment in `Eval.h` |
| The probe sits below the kingless/phase guards, so phase is out of the key (D5) | comment at the probe site |
| Chosen table size, hit rate, bytes per thread, and the sweep behind them | PR body and `Measurements/local.md` |
| Measured nps delta and the merge-or-close decision against the thresholds | `Docs/Changelog.md`, PR body, issue #131 |
| Whether PR 2 (pawn subtotal) paid, or was dropped and why | issue #131 — a recorded negative is the point of its gate |
| New `[eval]` coverage | `Docs/TestDesign.md` |
| #97's PR 2/PR 3 nps overrun is settled or confirmed structural | `.claude/plans/in-progress/king-safety-evaluation.md`, before PR 4's ablation runs |

Delete this file once both PRs have landed or been closed with their measurements, and the table
above is discharged. Nothing durable should survive only here.
