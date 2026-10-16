# Pawn-structure cache for `eval_pawns` and `eval_king_pawn_cover` — Design

**Issue:** #837 (successor to #131; prices from spike #833 and the probe below)

## Goal

`eval_pawns` and `eval_king_pawn_cover` depend almost entirely on the pawn structure, and each
search re-evaluates the same structure many times. On current `main` (`295fa5f`, after #832 put
static eval in the TT), computing both terms twice costs **7.37% [6.50, 8.23]** of nps. That is
above #131's break-even of ~4% removed work. The goal is a cache that recovers most of this cost
without changing any score. It is a speed-only change: node-identical search, no Elo claim beyond
the nps gain.

## Review focus

- **The probe cost is estimated, not measured** (Assumption 1). It decides whether the net is
  +2% or +5%. #131 measured two probes at 2.2-3.3% of runtime; one probe is taken as half of that.
- **D1's key** (pawn-only tag, with king-keyed cover embedded in the same entry) beats the
  alternatives only on the probe below. Check the arithmetic in D1.
- **D4, blockade replay.** This is the one place a cached score is combined with live
  occupancy, so it is where a wrong score could get in.

## Scope

**This change will:**

- add a per-thread, direct-mapped pawn-structure cache, owned by `ThreadData`;
- route the five search `Evaluate()` call sites through a cached overload;
- add tests for cached/uncached equality, blockade replay, king-square changes and a forced
  collision.

**This change will not:**

- add a pawn Zobrist key to `Board`. The two pawn bitboards are the exact tag (#131);
- cache any other term, or `EvalContext::pawn_attacks`;
- change any score, weight or `EvalBreakdown`. The breakdown and the UCI `eval` command stay
  uncached;
- run the strength lab. A node-identical gain of a few percent nps is below what it resolves.

## Probe: hit rates on current `main` (2026-10-17)

The probe was throwaway instrumentation in `Evaluate()` (reverted), counted at `Threads=1` over
`Run-Bench.ps1 -Depth 13`'s 8 positions. It saw 22.6M evaluations, excluding dead draws.
Rates are aggregate; the per-position range is in brackets.

| entries | pawns + both kings | per-colour (2 probes) | pawn-only | cover, embedded in a pawn-only entry (per colour) |
|---:|---:|---:|---:|---:|
| 256 | 65.9% [31-81] | 70.1% [45-84] | 79.7% [65-91] | 66.9% [40-80] |
| 1,024 | 70.6% [34-86] | 75.4% [50-89] | 84.2% [70-95] | 70.1% [43-83] |
| 4,096 | 74.5% [37-90] | 79.6% [54-93] | 87.8% [75-97] | 72.5% [45-86] |
| 16,384 | 77.9% [40-94] | 83.2% [59-96] | 90.8% [80-99] | 74.4% [46-88] |

"Embedded" means the entry has a pawn-only tag and also stores each colour's king square. On a
pawn hit, a colour's cover is reused if its king has not moved since the entry was written.

**Prices, measured on `295fa5f`.** Both terms were computed twice, using #131's method, on a
`New-OrderedBuildPair.ps1` pair (1221/1221 pinned functions plus all hot functions at identical
addresses). `Compare-Bench.ps1 -Control` ran 12 rounds at depth 13, with identical nodes.

- Result: **−7.37% [−8.23, −6.50]**; A/A control +0.36% [−0.15, +0.88].
- Every position cost 6.9-8.7%.
- The split between the terms was not re-measured. Spike #833's ratio (pawns 62.6%, cover 37.4%)
  gives pawns ≈ 4.6% and cover ≈ 2.8%.

## Decisions

### D1: one probe, with a pawn-only tag and king-keyed cover inside the entry

Net at 4,096 entries, taking a probe at 1.1-1.65% (Assumption 1):

| key | removed work | probes | net |
|---|---:|---:|---:|
| **pawn-only tag, embedded cover** | 0.878 × 4.6 + 0.725 × 2.8 ≈ **6.1%** [5.3, 6.8] | 1 | **+3.7% to +5.6%** |
| pawns + both king squares | 0.745 × 7.37 ≈ 5.5% [4.8, 6.1] | 1 | +3.2% to +5.0% |
| per colour (#131's layout) | 0.796 × 7.37 ≈ 5.9% [5.2, 6.6] | 2 | +1.9% to +4.4% |

The embedded key wins on both counts. `eval_pawns` keeps the pawn-only hit rate (88%): a king
move does not evict it. The cover gets nearly the combined key's rate, with no second probe.

At the worst bench position (pawn 75%, cover 45%), the net is still ≈ +3.0%. #131 measured
two probes at 2.2-3.3% before #832 cut the number of evaluations. That price is used here
unscaled, so the probe cost is overstated rather than understated.

Rejected:

- Combined key: same probe count, lower hit rate for the larger term.
- Per-colour key: the second probe costs more than its extra hits recover.
- Separate pawn and cover tables: two probes again.

### D2: entry and table layout

One entry is a single 64-byte cache line (`alignas(64)`), so a probe touches one line:

- the tag: `wp`, `bp` (16 B);
- `passers` (8 B): both colours' passed pawns, separated by intersecting with `wp` or `bp`;
- per colour, three `int16_t` for `eval_pawns` (the phase-neutral score and the *unblocked*
  passer mg/eg sums) (12 B);
- per colour, the king square and the three cover mg values (`int16_t`). Cover eg is always 0
  (14 B).

A slot is chosen by multiply-mixing the two bitboards and keeping the top bits. A full tag
compare means a collision costs a miss, never a wrong score. There is no replacement policy: a
miss overwrites.

`int16_t` holds every value with wide margin. Per colour, |score| ≤ 8 × 35 and the passer sums
are ≤ 8 × 45 × 32/16 = 720. Cover is bounded by the existing `KING_PAWN_COVER_WORST` and
`KING_SAFETY_BEST_BONUS`. These bounds become `static_assert`s next to the entry, not runtime
checks.

The table size is a constructor argument (a power of two). Slice 2 sets the shipped size by
measurement: sweep 1,024 / 4,096 / 16,384 entries (64 KB / 256 KB / 1 MB per thread) and take
the smallest size whose nps is within the A/A interval of the best. Above 4,096, the probe gains
under 1% removed work per ×4.

Rejected: `int32_t` fields (74 B, which straddles two lines).

### D3: ownership — `ThreadData` holds it, and a cached `Evaluate` overload takes it

- `ThreadData` gains `std::unique_ptr<PawnCache> pawn_cache`, following `cont_history`'s
  pattern, so a helper's `ThreadData` stays cheap to move.
- `Evaluator` gains `int Evaluate(const Board&, PawnCache&) const noexcept`.
- The existing `Evaluate(const Board&)` stays uncached. The tests' call sites, `Breakdown()` and
  UCI `eval` keep calling it unchanged.
- Both overloads share one body, through a `PawnCache*` passed to `RawWhitePov`. A null pointer
  computes directly, so the uncached path is exactly today's code.

This keeps the `Eval.h` Lazy SMP contract literally true: `Evaluator` still holds no state a
search mutates. The mutable state belongs to the caller, one table per thread. The contract
comment gains one sentence saying so.

Rejected:

- A pointer in `EvalContext`: it is built inside `Evaluate()`, so the pointer would have to come
  in through the signature anyway.
- `mutable` or `thread_local` state in `Evaluator`: it breaks the contract and hides the
  ownership.

### D4: blockade is replayed per call, exactly

`eval_pawns` scales each passer's bonus by `PASSED_PAWN_BLOCKADED_SCALE / 16` when
`ctx.occupied[enemy]` covers its stop square. This reads non-pawn occupancy, so it cannot be
cached under a pawn tag (#131).

The entry stores the unblocked passer sums plus the passer set. Per call, the cached path does
this:

1. Compute `blocked = passers_c & stop_squares⁻¹(occupied[enemy])`, usually empty.
2. For each blocked passer, add the exact per-passer delta,
   `BONUS·(scale·B/16)/16 − BONUS·scale/16`, for mg and eg.

The current code accumulates per-passer integer divisions, and this replay adds the same
per-passer terms, so the result is bit-identical. It is not an approximation.

Both the miss path and the uncached path use one helper that computes a passer's
(`scale`, blocked-scale) contribution, so the two paths cannot drift apart.

### D5: cover edge cases bypass the cache

`eval_king_pawn_cover` returns `{}` for `phase == 0` or a missing king. Both checks stay ahead of
the probe in the cached path, so the entry only holds values for a real king at a non-zero phase.
A colour whose king square differs from the entry's is recomputed and written back into the
entry, without touching the other colour or the pawn part.

### D6: a cache write is exempt from the aborted-frame rule

The entry is a memo of a pure function of its tag (`wp`, `bp`, king square). Its content can
never differ from a recompute, so a write from a frame that is later aborted changes nothing the
search sees. `EngineContracts.md`'s aborted-frame paragraph lists it beside the node counters.

The cache is never cleared: correctness does not depend on clearing it. A helper's fresh cache
starts cold, which only costs it hits.

## Assumptions I cannot verify from the code

1. **The one-probe cost is ~1.1-1.65% of runtime.** This is half of #131's measured two-probe
   cost; the probe itself is not measured. It is settled by slice 2's bench, which is the gate.
   If it is ≥ 3%, the net falls below +2%, and the gate closes the issue.
2. **The pawns/cover cost split** comes from spike #833 (pre-#832), not a re-measure. Only D1's
   ranking depends on it. The embedded key stays ahead of the combined key for any split, because
   its pawn hit rate is higher and its cover hit rate is within 2 points.
3. **The hit rates are bench-representative.** Bench positions at depth 13 are not games. #131
   saw the same 67-78% band, and the gate measures on the same bench, so this affects
   transfer to games, not the gate.

## Invariants

- `Evaluate(board, cache) == Evaluate(board)` for every position and every prior cache content.
- `Threads=1` search is node-, bestmove-, score- and PV-identical to the merge base.
- `Evaluator` holds no state a search mutates. Each search thread owns its own cache, and no
  table is shared.
- `EvalBreakdown` still sums to `Evaluate(board)` (unchanged path).

## Validation

**Tier: engine.** Validation:

- **Equality corpus (new test).** Walk the positions reachable at depth 3 from each
  `Tests/perft_test_cases.json` root, with one cache shared across the walk, and assert cached ==
  uncached at every node. Sibling moves keep pawns fixed while pieces and kings move, so the walk
  exercises cover reuse, cover rewrite and blockade on/off naturally. Run it at 4,096 entries and
  at **1 entry**, which forces a collision or overwrite on every structure change.
- **Directed tests.** Same pawns, with an enemy piece moved onto and then off a passer's stop
  square: the cached score follows the blocker. Same pawns, with one king moved: only that colour's
  cover changes. Also cover `phase == 0` and a kingless board.
- **Falsify.** Break the replay (drop the blocked delta) and check that the directed and corpus
  tests fail.
- `Compare-SearchEquivalence.ps1 -After <exe>`: node-identical.
- Lazy SMP races: the `tsan-linux` CI job (`Docs/CI.md`) drives multi-threaded search over the
  per-thread cache.
- **Speed gate** (#837): build `New-OrderedBuildPair.ps1` against the merge base, then run
  `Compare-Bench.ps1 -Control`. The verdict must be **Speedup** with the lower bound ≥ +2%.
  Close the issue below +1%. Between +1% and +2% is the owner's call.

**No Elo match.** The search is identical, and a few percent of nps is below the lab's
resolution.

## Cost

- **Size:** 150-250 lines. New `PawnCache.h`, plus `Eval.{h,cpp}`, `ThreadData.h`, five call
  sites in `AIPerplex.cpp`, one test file, and `EngineContracts.md`.
- **Blast radius:** engine tier: eval hot path and the threading contract. No gate or skill
  changes.
- **Review:** one code review, plus an `eval-reviewer` pass.
- **Bench:** the size sweep is 3 ordered-pair runs of ~10 min quiet each.
- **Optional:** none.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 key choice, with the probe table and prices | `Docs/Changelog.md` and the PR body |
| D3 caller-owned cache keeps the Lazy SMP contract | `Eval.h` contract comment |
| D4 exact per-passer replay | source comment on the replay helper |
| D6 a memo write is exempt from the aborted-frame rule | `Docs/EngineContracts.md` |
| Measured nps gain and chosen size | `Docs/Changelog.md`; `Docs/Workflow.md` → Speed and nps (#131 note updated) |
