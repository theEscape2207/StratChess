# Early TT prefetch via KeyAfter — Design

**Issue:** #825

## Goal

`Board::DoMove` prefetches the child's TT bucket just before its legality check, so the miss
overlaps that check and the work leading up to the child's probe. #822 made `InCheck()` cheap, so
less work now hides the miss. After #822, `BuildContext` (the first reader of the child's bucket)
took a larger share of the profile. The #825 spike issued the prefetch earlier, from a key the search
predicts before `DoMove`. The result was **+2.11% [+1.59, +2.63] nps** on an ordered pair, with
`BuildContext` falling from 12.7% to 10.7% and the search node-identical
([result](https://github.com/theEscape2207/StratChess/issues/825#issuecomment-6133852141)). This
change ships that spike as production code.

This is the variant C that #776 measured at −1.43% and abandoned. Two things have changed since
then. #822 removed most of the legality-check work that B's late prefetch was hiding behind. And
#776's post-mortem found that `KeyAfter` was **not inlined**. The spike repeated that result: left
to the inliner, `KeyAfter` cost −0.23% (4.0% self time), and force-inlined it gained +2.11%.

## Review focus

- **D1, the force-inline.** The whole gain depends on it. The guard only catches a lost inline when
  someone measures (Invariants), so check that this is acceptable and that no cheaper guard exists.
- **D4, keeping both prefetch sites.** `pvs` and `quiescence` were measured together, never apart.
  Shipping exactly what was measured is a deliberate choice, made over trimming to qsearch only.
- **Assumption A2:** in-check qsearch nodes currently prefetch every pseudo-legal evasion. About 89%
  of those are rejected (#823). The +2.11% already includes that waste.

## Scope

**This change will:**

- Add `Board::KeyAfter(const Move&)`, which predicts the child's zobrist key, and
  `Board::PrefetchChild(const Move&)`.
- Call `PrefetchChild` in `pvs()`'s and `quiescence()`'s move loops, at the spike's sites.
- Keep `DoMove`'s and `DoNullMove`'s existing prefetches, and update `DoMove`'s comment.
- Add a contract test for `KeyAfter`.

**This change will not:**

- Make `KeyAfter` exact for en-passant captures (D2).
- Change `DoNullMove`. Its prefetch has nothing in front of it to move earlier.
- Special-case in-check nodes. That is #823's job (D6).
- Touch the TT, `PrefetchTarget`, or the binding lifecycle from #776.

## Decisions

### D1: `KeyAfter` lives in `Board.h`, marked `STRAT_FORCEINLINE`

`KeyAfter` reads `Board`'s private state (`mailbox_`, `state_`, `zobrist_hash_`), so it is a
`Board` member, as #776 planned. It is defined in the header so that `AIPerplex.cpp` can inline it.

**Rejected: rely on the inliner.** This was measured twice, in #776 and in this spike's v1. Both
times clang-cl kept `KeyAfter` out of line, and the call cost the whole gain.

**Rejected: a smaller body that the inliner would accept.** The castling block is already behind
`[[unlikely]]` and a table lookup. What remains is the minimum for a correct key. A future clang
heuristic could change the result anyway, so this option guarantees nothing.

`STRAT_FORCEINLINE` already exists in `Compat.h` and has four uses.

### D2: The key is approximate: exact except for en-passant captures

It does not remove the captured pawn on an en-passant capture. On the spike's walks, 99.95% of keys
matched on kiwipete and 100% on the start position, and every miss was an en-passant capture. A
wrong key wastes one prefetch, and `DoMove` still prefetches the right bucket.

**Rejected: an exact key.** It needs one more branch on every call, and en-passant captures are
about 1 in 2,000 moves. #776's analysis says a per-move cost under about 15 cycles is what lets this
variant win, so a branch that nearly never pays its way does not belong. The doc comment states
this limit, so no caller treats the key as exact.

### D3: Promotions use `GetEffectiveMovPiece`, and castling uses a `static constexpr` mask table

These are the spike's v2 forms. Castling rights are updated as
`rights & kept[from] & kept[to]`, which is the same per-square mask that `DoMove` applies, kept as
a table so the hot path does not branch per square. The rook's zobrist pair follows `DoMove`'s
squares: KING_CASTLE moves the rook from `to+1` to `to−1`, QUEEN_CASTLE from `to−2` to `to+1`.

### D4: Prefetch at both measured sites

The two sites are:

- `pvs()`: right after the exclusion `continue`.
- `quiescence()`: after the delta-pruning `continue` and before the SEE test. A delta-pruned move
  never pays for a prefetch. A SEE-pruned capture wastes one, but the SEE test is the work the
  prefetch is meant to overlap.

**Rejected: qsearch only**, even though that is where `BuildContext`'s stall shows. That split was
never measured, and it would be a second experiment. Profile shares point to the qsearch site as
the larger share of the gain, but they cannot show that the `pvs` site costs anything. A follow-up
can trim a site if a measurement shows it is free to drop.

**Rejected in `pvs`: after the frontier guards**, since a frontier-pruned quiet move would then not
waste a prefetch. Moving the prefetch shortens the overlap it exists for, and that placement was
not measured.

### D5: `DoMove`'s prefetch stays

It covers en-passant captures (D2) and the `DoMove` calls outside the two loops: the any-legal-move
probe at `AIPerplex.cpp:1346`, the emergency move, and moves played on game and UCI boards. The root
and singular verification already run through `pvs`'s loop. When the early prefetch was correct, it is about 5 instructions
on a line that is already in flight. The spike kept it, so removing it would be unmeasured. Its
comment changes to say that it is now the fallback, and that the search loops prefetch earlier.

**Rejected: remove it.** That would make the en-passant gap a real miss, and it would need its own
measurement.

### D6: No in-check special case; #823 removes the waste structurally

In check, `material_bounds_hold` is false, so every pseudo-legal evasion reaches `PrefetchChild`.
That is about 30 wasted prefetches per in-check qsearch node, on top of the rejected `DoMove`s. The
+2.11% already includes this cost. #823 (legal-only evasions) removes those moves from the list.
The two changes commute: each touches the other's code only through the move list, so whichever
lands second needs only an equivalence rerun. Skipping the prefetch at in-check nodes before #823
lands would be an unmeasured second variable.

## Assumptions I cannot verify from the code

- **A1: clang-cl honours `STRAT_FORCEINLINE` at both call sites.** Verified for the spike build.
  The profile shows no `KeyAfter` symbol and attributes its cost to the search bodies (+3.8
  points). The production build is checked the same way: `KeyAfter` must be absent from the
  `Measure-CpuProfile` symbol list. GCC (the lab) honours `always_inline` by definition, and the
  macro maps to it.
- **A2: the in-check waste does not cost more than it gains on other hardware.** Not verified. The
  spike measured only this machine. A lab run would show a cache-pollution regression on the lab's
  CPUs. That run is optional (Validation).
- **A3: the gain survives a fresh build from `origin/main`.** The spike branch was measured on
  `cd97a1f`, and production code is rewritten rather than cherry-picked. The ordered-pair rerun in
  Validation settles this.

## Invariants

- The search is node-identical: `Compare-SearchEquivalence` IDENTICAL against the merge base.
- `KeyAfter(m)` equals the key after `DoMove(m)` for every legal move that is not an en-passant
  capture.
- `KeyAfter` is `const` and changes nothing. A prefetch never faults, even on a board whose
  `PrefetchTarget` is the default dummy.
- `KeyAfter` is inlined into `pvs` and `quiescence` in the shipping clang-cl build. Only a
  measurement catches this one (A1), so the source comment on `KeyAfter` says why it is
  force-inlined and what a lost inline costs.

## Validation

Tier: engine. Equivalence, a contract test, and a placement-controlled speed comparison.

- **Contract test** `[board][keyafter]`: walk the positions from `InCheckTests.cpp` (the perft set,
  en-passant pins, castling, promotions) to depth 3. For every move `DoMove` accepts, require
  `KeyAfter == get_zobrist_hash()`, except en-passant captures, which are counted. Require that at
  least one en-passant capture and one move of each castling side were seen, so the test cannot pass
  vacuously. **Falsify** it twice before committing: drop the castling-rights term, then the
  captured-piece term, and watch it fail each time.
- **Equivalence:** `Compare-SearchEquivalence -BaselineRef origin/main` IDENTICAL, depth 12, 6
  positions.
- **Speed:** `New-OrderedBuildPair` against the merge base, then
  `Compare-Bench -Control -Rounds 60 -Affinity 4`. `pvs` and `quiescence` both change size, so the
  pair needs #785's spacers, as it did for the spike. **Gate:** the 95% lower bound is above 0. A
  withheld Speedup caused by a wide A/A interval is not an objection, but rerun once if the A/A
  interval leaves ±1%.
- **Inline check:** one `Measure-CpuProfile -Before -After` run. `KeyAfter` must not appear as a
  symbol (A1), and the `BuildContext` share falls.
- **Elo:** the search is node-identical, so this is a pure speed change, and there is no local
  SPRT. A +2% speedup is worth about +2–4 Elo, which is inside the lab's ±4. The lab run is offered
  in Cost as an optional check of A2, not as a gate.

## Cost

- **Size:** about 60 lines of engine code (`Board.h`, `Board.cpp` comment, `AIPerplex.cpp`), plus a
  test of about 70 lines. 4 files.
- **Blast radius:** the engine tier. No gate, skill or doc changes beyond the changelog.
- **Review:** one code review (search-reviewer).
- **Optional parts:**
  - **CI strength lab against the merge base:** about 3 h, 18 of 20 CI slots. It covers A2 (in-check
    cache pollution on the lab's GCC hardware). It is unlikely to resolve a +2% speedup.
  - **`Compare-BenchLinux`:** about 15 min. A GCC trend that is cheaper than the lab for A2's
    direction.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1: why force-inline, and that v1 and #776 lost the gain out of line | source comment on `KeyAfter` |
| D2: the en-passant limit, and that a wrong key only wastes a prefetch | the same comment |
| D5: `DoMove`'s prefetch is now the fallback | source comment in `DoMove` |
| Why C wins now after losing in #776 (#822, inlining), and the measured result | `Docs/Changelog.md` |
| D6: in-check waste, revisit after #823 | a comment on #823 |
