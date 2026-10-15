# Early TT prefetch via KeyAfter — Design

**Issue:** #825 · **Review:** `StratChessSupport\DesignRecords\825 - early-tt-prefetch-keyafter.review.md`
(2 Blocking, 2 Add, 2 Clarify; all dispositioned)

## Goal

`Board::DoMove` prefetches the child's TT bucket just before its legality check, so the miss
overlaps that check and the work leading up to the child's probe. #822 made `InCheck()` cheap, so
less work now hides the miss. After #822, `BuildContext` took a larger share of the profile. It does
not read the TT; it is where the profiler charges the stall of the child's preceding probe
(`AIPerplex.cpp:1434`, before stand-pat at `:1482`). The #825 spike issued the prefetch earlier,
from a key the search predicts before `DoMove`. On an ordered pair the candidate beat both copies of
the baseline binary: **+1.88% [+1.32, +2.44]** against baseline and **+2.35% [+1.75, +2.95]**
against control, with A/A −0.46% [−0.94, +0.03]. Compare-Bench's verdict was **No slowdown**,
because the A/A interval left ±0.5% (that rule is #827). `BuildContext` fell from 12.7% to 10.7%
and the search stayed node-identical
([result](https://github.com/theEscape2207/StratChess/issues/825#issuecomment-6133852141)). This
change ships that spike as production code.

This is the variant C that #776 measured at −1.43% and abandoned. Two things have changed since
then. #822 removed most of the legality-check work that B's late prefetch was hiding behind. And
#776's post-mortem found that `KeyAfter` was **not inlined**. The spike's v1, left to the inliner,
showed no resolved gain (−0.23% [−0.68, +0.21], `KeyAfter` at 4.0% self time). v2 force-inlined
both helpers **and** moved the castling masks into a table; the two were not measured apart.

## Review focus

- **D1, the force-inline.** The whole gain depends on it. A one-time disassembly check guards the
  shipped build; nothing guards a later compiler change except the next measurement.
- **D4, keeping both prefetch sites.** `pvs` and `quiescence` were measured together, never apart.
  Shipping exactly what was measured is a deliberate choice, made over trimming to qsearch only.
- **Assumption A2:** every in-check move the loop reaches pays for an early prefetch; about 89% of
  generated in-check entries were illegal in #739's corpus. The spike's gain already includes that
  waste.

## Scope

**This change will:**

- Add `Board::KeyAfter(const Move&)`, which predicts the child's zobrist key, and
  `Board::PrefetchChild(const Move&)`, both force-inlined.
- Call `PrefetchChild` in `pvs()`'s and `quiescence()`'s move loops, at the spike's sites.
- Keep `DoMove`'s and `DoNullMove`'s existing prefetches, and update `DoMove`'s comment.
- Add a contract test for `KeyAfter`.

**This change will not:**

- Make `KeyAfter` exact for en-passant captures (D2).
- Change `DoNullMove`. Its prefetch has nothing in front of it to move earlier.
- Special-case in-check nodes. That is #823's job (D6).
- Touch the TT, `PrefetchTarget`, or the binding lifecycle from #776.

## Decisions

### D1: `KeyAfter` and `PrefetchChild` live in `Board.h`, both `STRAT_FORCEINLINE`

`KeyAfter` reads `Board`'s private state (`mailbox_`, `state_`, `zobrist_hash_`), so it is a
`Board` member, as #776 planned. Both helpers are defined in the header so that `AIPerplex.cpp`
can inline them, and both carry `STRAT_FORCEINLINE`, as in the spike's v2: a `KeyAfter` inlined
into an out-of-line `PrefetchChild` would still pay a call.

**Rejected: rely on the inliner.** In #776 and in the spike's v1, clang-cl kept `KeyAfter` out of
line, and neither showed a gain.

**Rejected: a smaller body that the inliner would accept.** The castling block is already behind
`[[unlikely]]` and a table lookup. What remains is the minimum for a correct key. A future clang
heuristic could change the result anyway, so this option guarantees nothing.

`STRAT_FORCEINLINE` already exists in `Compat.h`.

### D2: The key is approximate: exact except for en-passant captures

It does not remove the captured pawn on an en-passant capture. On the spike's walks, 99.95% of keys
matched on kiwipete and 100% on the start position, and every miss was an en-passant capture. A
wrong key wastes one prefetch, and `DoMove` still prefetches the right bucket.

**Rejected: an exact key.** It needs one more branch on every call, for a move that is rare (about
1 in 2,000 in the kiwipete walk). #776's estimate is that this variant wins only while its per-move
cost stays under about 15 cycles, so a branch that nearly never pays its way does not belong. The
doc comment states this limit, so no caller treats the key as exact.

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
and singular verification already run through `pvs`'s loop. When the early prefetch was correct, it
is about 5 instructions on a line that is already in flight. The spike kept it, so removing it would
be unmeasured. Its comment changes to say that it is now the fallback, and that the search loops
prefetch earlier.

**Rejected: remove it.** That would make the en-passant gap a real miss, and it would need its own
measurement.

### D6: No in-check special case; #823 removes the waste structurally

In check, `material_bounds_hold` is false, so every in-check move the loop reaches pays for a
prefetch, on top of the rejected `DoMove`s. The spike's gain already includes this cost. #823
(legal-only evasions) removes those moves from the list.

The two changes commute **semantically**: each touches the other's code only through the move
list. They do not commute in **speed**: #823 changes the work between this prefetch and the probe,
which is exactly what #822 did. So whichever lands second reruns equivalence **and** a paired bench
against the then-current merge base, and inspects the lead time if #823 skips `DoMove`'s legality
check. #825 lands first, so that cost falls on #823. Skipping the prefetch at in-check nodes
before #823 lands would be an unmeasured second variable.

## Assumptions I cannot verify from the code

- **A1: clang-cl honours `STRAT_FORCEINLINE` at both call sites.** Checked once on the shipping
  build by disassembly (Validation). GCC (the lab) honours `always_inline` by definition, and the
  macro maps to it.
- **A2: the in-check waste does not cost more than it gains on other hardware.** Not verified. The
  spike measured only this machine. A lab run could detect a sufficiently large cache-pollution
  regression on the lab's CPUs. That run is optional (Cost).
- **A3: the gain survives a fresh build from `origin/main`.** The spike branch was measured on
  `cd97a1f`, and production code is rewritten rather than cherry-picked. The ordered-pair series
  in Validation settles this.

## Invariants

- The search is node-identical: `Compare-SearchEquivalence` IDENTICAL against the merge base.
- `KeyAfter(m)` equals the key after `DoMove(m)` for every legal move that is not an en-passant
  capture.
- `KeyAfter` is `const` and changes nothing. A prefetch never faults, even on a board whose
  `PrefetchTarget` is the default dummy.
- `KeyAfter` and `PrefetchChild` are inlined into `pvs` and `quiescence` in the shipping clang-cl
  build. The source comment on `KeyAfter` says why both are force-inlined and what a lost inline
  costs.

## Validation

Tier: engine. Equivalence, a contract test, an inline check, and a placement-controlled speed
comparison. Order: everything machine-free first, the quiet bench last.

- **Contract test** `[board][keyafter]`: walk the positions from `InCheckTests.cpp` (the perft set,
  en-passant pins, castling, promotions) to depth 3. For every move `DoMove` accepts, require
  `KeyAfter == get_zobrist_hash()`, except en-passant captures, which are counted. Require that at
  least one en-passant capture and one move of each castling side were seen, so the test cannot pass
  vacuously. **Falsify** it twice before committing: drop the castling-rights term, then the
  captured-piece term, and watch it fail each time.
- **Equivalence:** `Compare-SearchEquivalence -BaselineRef origin/main` IDENTICAL, depth 12, 6
  positions.
- **Inline check:** disassemble the shipping Release clang-cl exe once, before the quiet window.
  `pvs` and `quiescence` must contain the predicted-key calculation and the prefetch, with no call
  to either helper. Keep the excerpts and the exe's hash for the PR.
- **Speed:** `New-OrderedBuildPair` against the merge base (with #785's spacers, since `pvs` and
  `quiescence` both change size), then one `Compare-Bench -Control` series of 12 rounds.
  **Gate:** the candidate's 95% lower bound is above 0 against baseline **and** against control.
  Compare-Bench's own label is not the gate: it withholds Speedup on any A/A interval outside ±0.5%,
  however large the margin (#827). If the A/A offset exceeds the candidate's smaller margin, run one
  fresh 12-round series, declared before it starts; if that fails too, the speed question is
  unresolved and goes back to the owner.
- **Profile (optional):** one `Measure-CpuProfile -Before -After` run corroborates the stall
  hypothesis (`BuildContext` share falls). It proves nothing about inlining.
- **Elo:** the search is node-identical, so this is a speed change, and there is no local SPRT. Elo
  and behaviour on other hardware remain unmeasured. The generic nps-to-Elo heuristic does not bound
  a TT-latency change: #776 read about +3.2% bench nps but +16.6 ±3.1 Elo, because table pressure
  grows in games. The lab is optional (Cost).

## Cost

- **Size:** about 60 lines of engine code (`Board.h`, `Board.cpp` comment, `AIPerplex.cpp`), plus a
  test of about 70 lines. 4 files, plus the changelog.
- **Blast radius:** the engine tier. No gate, skill or doc changes beyond the changelog.
- **Review:** one code review (search-reviewer).
- **Quiet time:** about 8 min for the bench series, 16 with the rerun.
- **Optional parts:**
  - **CI strength lab against the merge base:** about 3 h, 18 of 20 CI slots, no local quiet. It
    adds the evidence this slice defers: Elo, and A2 on the lab's GCC hardware.
  - **`Compare-BenchLinux`:** about 15 min. A GCC compiler check on this machine; it says nothing
    about cache behaviour on the lab's CPUs.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1: why both helpers are force-inlined, and that v1 and #776 showed no gain out of line | source comment on `KeyAfter` |
| D2: the en-passant limit, and that a wrong key only wastes a prefetch | the same comment |
| D5: `DoMove`'s prefetch is now the fallback | source comment in `DoMove` |
| Why C wins now after losing in #776 (#822, inlining), and the measured result | `Docs/Changelog.md` |
| D6: in-check waste; #823 reruns equivalence **and** a paired bench | a comment on #823 |
| Rejected half of review B1 (label gate), with the owner's reason | PR body |
