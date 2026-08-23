# Pawn Hash Table

**Issue**: #131 · **Epic**: #110 Tier 4 · **Depth**: design sketch · **Status**: not started
**Depends on**: #116 (the terms that justify it), #127 (EvalContext)

> Sketch, not an executable plan. Explicitly a throughput optimization — no eval-quality effect.

## Goal

Cache the pawn-structure component of the evaluation in a small hash table keyed on a pawns-only Zobrist
hash, so it is computed once per distinct pawn configuration rather than once per evaluated node.

## Why, and why not yet

Pawn structure is the part of the evaluation that changes least often during a search and, once #116 and
#97 land, costs the most to recompute. Currently doubled and isolated penalties; soon also:

- **#116** — passed-pawn span tests and backwards-pawn detection, per pawn.
- **#97** — pawn-shield scanning around each king.

A typical search subtree shares one pawn configuration across a large number of nodes, which is the
textbook case for a pawn hash.

**But the win here is smaller than in most engines**, and this needs saying plainly so nobody
over-invests: `Evaluate()` is only ever called from `AIPerplex::quiescence` (`AIPerplex.cpp:662,693`),
never at interior PVS nodes. Total eval calls are therefore a smaller fraction of node count than in an
engine that evaluates everywhere. The win is real but bounded.

**Do not build this before the terms that justify it exist.** A pawn hash over today's two cheap pawn
terms would very plausibly be slower than recomputing them.

## Approach

1. **Pawns-only Zobrist key.** `zobrist::piece_keys` (`Board.h`) already covers pawns. Maintain a second
   accumulator alongside `zobrist_hash_` in `Board::add_piece` / `remove_piece` / `move_piece` — cheaper
   than hashing the pawn bitboards on demand, and those three functions are already the single choke point
   for every piece movement.
2. **Small direct-mapped table** of `{key, mg_score, eg_score}` entries. Pawn configurations are far fewer
   than positions; a few thousand entries is plenty. No replacement policy needed beyond overwrite.
3. **Lookup at the top of the pawn term**, store on miss.

## The decision that actually matters: where the table lives

`Eval.h` carries an explicit Lazy SMP sharing contract as a class comment:

> `EvalManager` and its derived classes hold no mutable state of any kind — no data members beyond
> compile-time-constant enums/statics, and `Evaluate()` is `const` … A single `EvalManager` instance is
> therefore safe to share, unsynchronized, across every Lazy SMP helper thread's concurrent `Evaluate()`
> calls — no per-thread clone is needed.

**A pawn hash breaks that contract.** Two options:

- **(a) Own the table per-thread in `ThreadData`** (`StratEngine/ThreadData.h`, which already holds every
  other piece of per-search state: board copy, node counter, PV, killers, history). Passed into the eval
  call or into `EvalContext`. No synchronization, no contract change, no sharing hazard. **Recommended.**
  Cost: each helper thread warms its own table — acceptable, and the same trade Lazy SMP already makes for
  killers/history.
- (b) Share one concurrent table across threads, following `TranspositionTable`'s discipline. Better hit
  rate, but it requires revising the `Eval.h` contract comment, a real concurrency review, and it makes the
  evaluator stateful — which every other item in this epic currently relies on it *not* being.

**Decide this explicitly in the eventual plan.** The failure mode of not deciding is option (b) happening
by accident — a mutable member added to `EvalComplex` because it was the shortest path, silently invalidating
a documented thread-safety argument that #109's review established.

Note that `Evaluate()` is `const`, so option (b) would need `mutable` or a `const_cast` — treat either
appearing in a diff as a signal that this decision was skipped rather than made.

## Measurement

- **Correctness**: scores must be **identical** with and without the cache, over #127's position corpus via
  #129's batch mode. A pawn hash is a pure cache; any score difference is a bug (most likely a key collision
  or a key that fails to capture something the term depends on — e.g. caching a term that reads king
  position, which is *not* in a pawns-only key).
- **Throughput**: nodes/second at fixed depth, single-threaded, before and after. This is the entire point,
  so it is the headline number.
- **Hit rate**: instrument it once during development. A hit rate below ~90% means the key or the table size
  is wrong.
- **ELO**: whatever the extra search depth buys — small, and must not be claimed without measurement. SPRT
  (#130) `NonRegression` is the right form: the claim is "faster and not weaker".

## Files likely touched

`StratEngine/Board.h` / `Board.cpp` (pawn key accumulator), `StratEngine/ThreadData.h` (the table, under
option (a)), `StratEngine/Eval.h` / `Eval.cpp` (lookup/store in the pawn term), plus a new small header for
the table itself. Tests: a `[tt]`-adjacent tag or `[eval]`.

## Test ideas

- The pawn key is identical for two positions with identical pawn placement but different piece placement,
  and different when a pawn moves.
- The pawn key is maintained incrementally: after `DoMove` + `UndoMove`, it matches the original (the same
  invariant the main Zobrist key already needs, and the same way it breaks).
- Cached and uncached evaluation produce identical scores over the corpus.
- Only pawn-derived quantities are cached — a term that reads a non-pawn quantity must not be inside the
  cached block. Worth an explicit test if any term is borderline.
- With `threads > 1`, per-thread tables produce the same scores as single-threaded (option (a) sanity).
