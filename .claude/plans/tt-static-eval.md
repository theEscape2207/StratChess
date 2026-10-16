# Static evaluation in the TT entry — Design

**Issue:** #832

## Goal

Evaluation is 47.5% of runtime (A/A profile at `0fb63d0`, `StratChessSupport\CpuProfiles\2026-10-16-832`). Nothing remembers an evaluation, so every transposition and every re-search evaluates its position again. #832's spike counted `Evaluate()` calls made at a node whose own TT probe had already found an entry for a position evaluated earlier: **19.1%** of 27.9M calls (pvs 27.4%, quiescence 15.4%) over the 8 bench positions at depth 13. Serving those calls from the entry the node has already loaded skips them at no extra memory access.

The spike counted repeat opportunities, not evals actually served: a declined store, or a node that stores nothing, can lose one. Assuming those calls cost the average eval, they are about 19.1% × 47.5% ≈ **9% of runtime**. That is about +10% nps before new costs. It is an **equal-average-cost estimate**, not a measured ceiling.

The 16-byte entry is full, so the question is where 16 bits come from.

## Review focus

- **D2, the slot check after the key loses 16 bits.** Today a torn pair of words can never decode to a stored key unless both payloads are equal. With the field in word 0, the decoded key is still checked, but only to a probability. Is a multiply by an odd constant strong enough, and is it cheap enough on 4 slot loads per probe?
- **Assumption A1, the served rate under game load.** Bench clears the table per position. In games the table is fuller but also carries entries across searches, so the direction is unknown. The doc gates on wall clock and the lab, not on the estimate.
- **D4, exactness of the dead-draw exclusion.** It is the one input `Evaluate()` reads beyond the Zobrist key.

## Options for the 16 bits

Layout today: key 64 | value 16 | depth 16 | move 16 | metadata 8 (5 bits used) | age 8.

| | Option | Verdict |
|---|---|---|
| **O1** | **48-bit key.** Store key bits 16–63; the bucket index already implies bits 0..k−1 (k = 21 at the default 192 MiB, 24 at the 1536 MiB cap). | **Chosen** (D1) |
| O2 | Narrow the payload and keep a 64-bit key: depth 16→8, age 8→5, eval 14 bits plus an out-of-range sentinel. | Rejected. Depth alone would fit: main depth is 1–255, quiescence budgets lie in [−239, 15], and the stored phase can select the decoding. But the 5-bit age wraps after 32 searches instead of 256, which changes replacement. The 14-bit eval also needs a range argument over every accepted FEN. O1 changes no payload field. |
| O3 | 20-byte entries, 3 per 64-byte bucket. | Rejected. −25% capacity, and the entry no longer fits the two-word lock-free `Slot`. |
| O4 | A parallel `int16_t` array beside the table. | Rejected. It costs a second cache line per probe, which is the separate cache the spike priced at net ≲ +3%. |
| O5 | Stockfish-style 10-byte entries with a 16-bit key. | Rejected. A redesign of the table, out of scope. |

## Scope

**This change will:**

- Store a side-to-move static evaluation in every TT entry written by a node that evaluated, and reuse it at any node whose probe returns an entry carrying one: pvs's `node_eval` and quiescence's stand-pat.
- Repack `PackedEntry` to a 48-bit key plus a 16-bit `static_eval`, keeping 16 bytes and 4 entries per bucket.
- Add `evalserved` / `evalcomputed` counters to the `STRAT_TT_STATS` build and to `Scripts/measure_tt_capacity.py`.

**This change will not:**

- Use the cached eval for any new pruning or as a search input. This change is speed only, and the search stays node-identical.
- Write an eval into a slot whose same-key store is declined. The retained entry keeps whatever it had; D3 says why.
- Add a separate eval cache.
- Touch replacement ranking, bucket size or `Hash` limits.

## Decisions

### D1: 48-bit key in word 0, beside the eval

`PackedEntry` becomes `static_eval` (int16, offset 0) | key48 (offsets 2–7) | value | depth | move | metadata | age. `TTEntry` gains `int16_t static_eval`, and `unpack()` sets it.

`kNoStaticEval = INT16_MIN` marks an entry without one. `Evaluate()` never returns it: static scores stay below `Mate_Threshold`, asserted at the store.

The effect on false matches:
- With k ≥ 16, so `Hash` ≥ 4 MiB, the independent bits a probe checks are 64 − k, the same as today.
- Below 4 MiB, the check drops to 48 bits from 64 − k, at most 50 at `Hash` = 1. At those sizes the D2 torn-pair model is no longer below the collision rate.
- An all-zero key48 reads as empty. A key hits that 2^−48 of the time, against 2^−64 today, and it costs one lost entry.

Rejected: O2–O5 above.

### D2: Word 0 is XORed with the payload word times K = 0x9E3779B97F4A7C15

Today `word0 = key ^ word1`. A torn pair, (A0, B1), decodes to `keyA ^ A1 ^ B1`, so it passes only if A1 == B1, and then nothing is torn.

With the eval in word 0, a plain XOR leaves a hole. If A1 and B1 differ only in `value`, which sits under the eval's 16 bits, the key decodes correctly while B's value is paired with A's key.

**New encoding:** `word0 = (key48 << 16 | eval) ^ (word1 · K) mod 2^64`. A torn pair passes the key check only if the top 48 bits of `A1·K` and `B1·K` agree. That requires `(A1 − B1)·K ≡ j (mod 2^64)` for some `0 < |j| < 2^16`. So the arithmetic difference `A1 − B1` must be one of the 2^17 − 2 values `j·K⁻¹`; that is necessary, not sufficient. The mixing direction (B0, A1) negates the difference, and this set is symmetric.

Oddness alone guarantees nothing. With K = 3, a value-only difference of 1 passes (review B1). So the constant is fixed, and its guarantee is checked.

**Deterministic guarantee, for this K.** None of those 2^17 − 2 differences can be written as per-field differences in two or fewer of word 1's five fields (value 16, depth 16, move 16, metadata 8, age 8 bits, each difference `|d| < 2^width`). So a torn pair whose payloads differ in at most two fields is always rejected, whichever two. That covers value-only, depth-only and the value-plus-depth pairs re-searches produce.

- The check enumerates all 2^17 − 2 differences and finds the fewest fields each needs. It ran during design (`StratChessSupport\CpuProfiles\2026-10-16-832\kcheck.py`, about 15 s in Python): the minimum for this K is 3. For K = 3 it is 1.
- The other common 64-bit mixing constants, `0xD6E8FEB86659FD93` and `0xFF51AFD7ED558CCD`, also give 3.
- The check becomes a unit test, so a changed K or field layout fails it.

**Modelled, not guaranteed: three or more fields.** A pair that differs in three or more fields passes only if its arithmetic difference equals one of the 2^17 − 2 fixed values. Rejection holds on the assumption that real payload differences are not correlated with multiples of K⁻¹. That is the same kind of assumption as the random-Zobrist model the `Slot` comment already rests on.

**What the `Slot` comment will say.** A pair differing in two or fewer payload fields is rejected deterministically. A pair differing in more is rejected except under the model above. The comment will not quote an unconditional probability.

Rejected:
- **Plain XOR.** It has the hole above.
- **`word1 ^ (word1 << 16)`.** Any difference made of four repeated 16-bit chunks cancels.
- **A full 64-bit finalizer.** More dependent operations per slot load, and it gives no deterministic guarantee this check could not give for a single multiply.

### D3: The store carries the eval; a same-key store with none inherits it

`store()` gains an explicit `int16_t static_eval` parameter, with no default, so none of the 8 call sites (3 in pvs, 5 in quiescence) can forget it. Each site passes the frame's eval if the frame computed one, else `kNoStaticEval`.

On the same-key path, an incoming `kNoStaticEval` inherits the stored eval, as `best_move` already does. This is safe because the same key means the same position, which means the same eval (D4).

A declined store writes nothing (scope). Merging the eval alone into a retained entry would be a second kind of write, and the stats counters show what it would buy (A1).

### D4: Dead-draw evaluations are never stored

`Evaluate()` reads one thing beyond the key: `dead_draw_score_`. That is contempt, set per `Search()` from the root colour, and it is returned when `endgame_scale == 0`.

Today a stale one could not be served anyway:
- `Search()` already clears the TT when the root colour or contempt context changes under nonzero contempt;
- `SetTuning()` clears it on a change of contempt magnitude.

The exclusion is a deliberate extra restriction, so the eval field's exactness does not depend on those clears staying.

AIPerplex stores `kNoStaticEval` whenever the eval equals `draw_score(td)`. Every dead-draw result equals it. A genuine eval that happens to equal it only loses a hit, so the rule is exact with no new Evaluator API.

Everything else `Evaluate()` reads is in the key:
- bitboards;
- castling rights;
- side to move, which the eval is relative to.

The halfmove clock is not read. En passant is in the key but not read, so it only splits hits.

Rejected: an Evaluator flag or a changed return type, which is API for one caller.

### D5: Reuse sites

- **pvs:** after a probe hit carrying an eval, set `static_eval` / `static_eval_known` before `node_eval` can run.
- **quiescence:** hoist the probe's entry out of its `if`, and use its eval for `stand_pat` when not in check.
- **Other `Evaluate()` calls:** the ply backstops and the budget-exhausted return run before any probe, so they stay as they are.

## Assumptions I cannot verify from the code

- **A1: game-load served rate is high enough to pay for the multiply.** The bench rate is 19.1% with a near-empty table; under game load, with an aging and full table, it is lower.
  - Verified by `measure_tt_capacity.py` on a lab PGN at the default `Hash` with the new counters: served ÷ (served + computed).
  - Gated in the end by wall clock and the lab, not by this number.
  - Not yet run.
- **A2: the multiply costs well under the gain.** It adds one multiply to every slot load, and `probe()` / `store()` load up to 4.
  - Verified by the ordered-pair `Compare-Bench.ps1 -Control`.
  - Not yet run.

## Invariants

- **The search is node-identical to `main` at `Threads=1`, at default `Hash`:**
  - a served eval equals what `Evaluate()` would return (D4);
  - key matching is unchanged for k ≥ 16 (D1);
  - replacement is untouched.
- **`sizeof(PackedEntry) == 16`, 4 entries per 64-byte bucket.**
- **Every `TTEntry` field is set in `unpack()`, including `static_eval`.**
- **A slot built from two stores whose payloads differ in at most two fields is always rejected (D2).**

## Validation

- **Engine tier.** `Compare-SearchEquivalence.ps1` gives identical nodes and best moves.
- **Unit tests, `TranspositionTable`:**
  - eval round-trip, including negative values;
  - same-key inherit when the incoming store has `kNoStaticEval`;
  - a declined store keeps the old eval;
  - key48 match and mismatch;
  - a hand-built torn pair (A0, B1) whose payloads differ only in `value` is rejected, as is the review's K=3 example pair. This is the D2 hole, and the test must fail with plain XOR or K = 3;
  - an exhaustive check that none of the 2^17 − 2 accepted differences for the chosen K is a difference in two or fewer fields. It covers both mixing directions and must fail for K = 3.
- **Unit test, AIPerplex:** a dead-draw position stores `kNoStaticEval`.
- **Speed:** ordered-pair `Compare-Bench.ps1 -Control` against the merge base. The verdict is the ordered-pair result; the ~10% equal-cost estimate is not a bound to check it against.
- **A1:** `measure_tt_capacity.py`, reporting the served rate.
- **Strength:** speed alone does not ship it. Propose a lab run, the owner's call.

## Cost

- **Size:** 150–250 lines across `TranspositionTable.h`, `AIPerplex.cpp` (8 store sites, 2 reuse sites), `TTStats.h`, `measure_tt_capacity.py`, TT tests and the Changelog.
- **Blast radius:** Engine tier, plus `search-reviewer`; no gates or skills change.
- **Review:** full code review, about 200k tokens.
- **Measurement:**
  - about 10 min `Compare-Bench`;
  - about 20 min `measure_tt_capacity.py`;
  - a lab run of about 3 h on 18 CI slots, if the owner approves.
- **Optional parts:** none.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1 layout, false-match arithmetic, and why the 16 bits came from the key (O2–O5) | comment on `PackedEntry` |
| D2 constant, the ≤2-field deterministic guarantee and the ≥3-field model | `Slot` comment, plus the exhaustive unit test |
| The TT now stores a static eval as well as scores | `Docs/EngineContracts.md` → Search internals (its "scores, not static evals" line goes stale) |
| D3 inherit-on-same-key, and no eval merge on decline | comment in `store()` |
| D4 dead-draw exclusion | comment at the store sites' helper in `AIPerplex.cpp` |
| Served rate, nps and lab result | `Docs/Changelog.md`, PR body, ledger row |
