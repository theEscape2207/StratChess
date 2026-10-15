# Legal-only move lists at in-check nodes — Design

**Issue:** #823 (from spike #739)

## Goal

At an in-check node, `pvs()` and `quiescence()` generate ~35 pseudo-legal moves, of which ~4 are
legal, then score and sort all of them and let `Board::DoMove` reject the rest by make/undo. Spike
#739 measured 32% of all `DoMove` calls as rejected in-check moves and put the waste at a ceiling of
~8.5-11% of runtime. Generating only the legal evasions removes it with no change to the search tree.

**The current ceiling is unmeasured; this is an experiment gated on measured speed.** The #739 shares
predate #822 (PR #824: +1.57% nps on a plain pair, +2.41% on an ordered pair, both "No slowdown"),
which cheapened the legality `InCheck()`, and #825, which added a TT prefetch per rejected move. On
the old profile, the in-check ordering waste (~4.3%) plus rejected make/undo self time (3.3-4.0%)
came to ~7.6-8.3%; how much of that survives is a planning estimate, not a bound — profile shares
drift between sessions, and #824 realised a quarter of its profile forecast. The triage asked for a
fresh A/A profile first; this doc departs from that deliberately. A profile would size the ceiling,
but the decision rests on the ordered-pair `Compare-Bench` Speedup gate, which measures the benefit
directly and parks the change if it does not confirm.

## Review focus

- **D3, identical order.** The node-identity claim rests on the evasion list being exactly the legal
  subsequence of `ComputeLegalMoves`' output, in the same order. Check the argument and that the
  test in Validation would catch a single misplaced move.
- **D4, legality rules.** Exactly legal is what lets the Debug assert stand; the x-ray king retreat,
  en passant and the "pinned pieces have no evasions" proof are where a hand-written generator goes
  wrong.
- **D6, the non-check path.** Mask parameters thread through helpers that the common non-check path
  also runs; a code-layout nps loss there could cancel the gain.

## Scope

**This change will:**

- add `MoveGenerator::ComputeEvasions(const Board&, MoveList&)`, exactly legal, precondition
  `board.InCheck()`;
- call it from `pvs()` and `quiescence()` when in check, and from the perft entry points
  (`perft_recursive`, `perft_detailed_recursive`, `divide`) when in check;
- thread destination and origin masks through the private generator helpers it shares with
  `ComputeLegalMoves` / `ComputeCaptures`;
- add an order-equivalence test and edge-case tests.

**This change will not:**

- skip `DoMove`'s legality check anywhere (D2);
- change out-of-check generation, ordering or pruning — out-of-check rejection overhead is ~0.2-0.3%
  of runtime on the #739 estimate;
- switch `IsCheckmated`, `PVIntegrity`, `UCIHandler`, `HumanPlayer` or the emergency-move fallback;
  they are cold paths and keep the pseudo-legal-plus-`DoMove` oracle the tests compare against;
- add a precomputed between-squares table (D4).

## Decisions

### D1: A dedicated evasion generator, not a filter

`ComputeEvasions` generates only moves that resolve the check. Rejected: a pin- and checker-aware
filter over the pseudo-legal list. It needs the same checker, pin and king-safety machinery, still
pays to generate ~35 moves and test each, and forgoes the generation saving. Also rejected: an
evasion generator written as separate loops — it would have to reproduce `ComputeLegalMoves`'
emission order by hand, which is how D3 breaks silently.

### D2: Exactly legal output; `DoMove` keeps its legality check

The list contains exactly the legal moves. `DoMove` still tests legality and is not given an
unchecked variant. At in-check nodes it then runs on ~4 moves instead of ~35; the remaining check is
one `AttackersTo` per legal move (post-#822), about 5% of all `DoMove` calls (3.6M of 74M on the
#739 bench), so skipping it saves a fraction of an already small share. That does not pay for a second
entry point on the most-called board function, nor for re-examining the prefetch lead time #825's
note asks about. A Debug `assert` at the two search call sites states that `DoMove` never rejects an
in-check node's move.

### D3: Same relative order as `ComputeLegalMoves`, so the tree is identical

The evasions are emitted by the same helpers in the same sequence (pawn captures left then right,
non-capturing promotions, pushes, knight, bishop, rook, queen, king; no castling), each iterating a
masked bitboard by ascending square. Masking a bitboard removes bits but never reorders the rest, so
the output is the legal subsequence of `ComputeLegalMoves`' list. `MoveSorter`'s `score_order` is a
strict total order on (score, generation index), and a move's score does not depend on its
neighbours, so the sorted legal moves come out in the same order as before; illegal moves were never
searched. The profile build's seeded tie-break keys on the move itself, so it holds there too.

Every `pvs()` use of the sorted index was checked: `si` feeds only `OrderRemaining`'s trigger, the
LMR score lookup and the `searched` bitset, all relative to the same list; move numbering is
`legal_moves_searched`, already legal-only. Singular extension is gated `!in_check`. The claim is
proven by test and by `Compare-SearchEquivalence`, not assumed.

### D4: Legality rules

With `k` the king square, `occ` all pieces, `them` the enemy pieces:

- **Checkers** = `AttackersTo(k, occ) & them`. Two checkers: king moves only.
- **King moves** to squares not own-occupied whose `AttackersTo(to, occ ^ bit(k)) & them & ~bit(to)`
  is empty. Removing the king from the occupancy catches a retreat along the checker's line; masking
  `to` lets the king capture an undefended checker. Never castling.
- **Single check, other pieces:** destinations restricted to `checker | between(k, checker)`;
  pushes and promotions to `between` only. `between` is computed as the intersection of the
  checker's slider attacks from both ends (rook rays if they share a rank or file, else bishop), empty
  for a knight or pawn — no new 32 KiB table for one lookup per in-check node.
- **Pinned pieces are excluded outright.** A pinned piece moves only along its pin ray, which meets
  the checking ray only at the king, and the pinner is not the checker (it is blocked), so a pinned
  piece has no evasion. Pinned = own pieces that are the sole blocker in `between(k, s)` for an enemy
  slider `s` seeing `k` through own pieces.
- **En passant** is a candidate when the captured pawn is the checker or the en-passant square is in
  `between`, and is emitted only after an exact test: `k` not attacked with the capturing pawn moved
  and the captured pawn removed. It is at most two moves and catches the rank-pin case no pin set
  describes. `AttackersTo` reads attackers from the unmodified piece bitboards, so the victim is
  masked out of the result as well as the occupancy, with `cap` the victim's square:
  `occAfter = (occ & ~bit(from) & ~bit(cap)) | bit(to)`, legal iff
  `AttackersTo(boards, k, occAfter) & them & ~bit(cap)` is empty.

### D5: Perft uses it too

`perft_recursive`, `perft_detailed_recursive` and `divide` call `ComputeEvasions` in check. That puts
the new generator under `Run-PerftCheck.ps1`'s 142,953-position external oracle; otherwise the
largest validation the repo has would not exercise it. They keep `DoMove`'s rejection, which never
fires on an evasion list.

### D6: Masks as plain parameters; `onlyCaptures` folds into them

`GenerateOfficerMoves` takes a destination mask and an origin mask instead of `bool onlyCaptures`:
`ComputeLegalMoves` passes all squares, `ComputeCaptures` passes the enemy pieces as destinations,
`ComputeEvasions` passes `checker | between` and the unpinned pieces. The pawn helpers take the same
two masks. One code path, so emission order cannot drift between generators. Rejected: a
`template<bool Evasion>` copy of the helpers — two instantiations of the hot generator to save one
`AND` per piece, which `Compare-Bench` can show is not needed. If the bench shows a non-check
regression, this decision reopens.

## Assumptions I cannot verify from the code

- **There is still a worthwhile ceiling after #822/#825.** A planning estimate from the #739 profile,
  not re-measured (Goal). Settled by the `Compare-Bench` Speedup gate, which parks the change if it
  fails.

Verified, not assumptions: `go perft` reaches `Perft::divide` (`UCIHandler.cpp:495`), so perftcheck
exercises D5; the root is `pvs()` at ply 0, so no separate root generator needs the change.

## Invariants

- `ComputeEvasions(b)` equals, as an ordered sequence, `ComputeLegalMoves(b)` filtered by
  `IsLegalMove`, for every position with the side to move in check.
- `ComputeLegalMoves` and `ComputeCaptures` output is unchanged, move for move and in order.
- Search is node-identical at `Threads=1`.
- Quiescence's "empty survivor set in check means checkmate" (`AIPerplex.cpp:1596`) holds: the list
  is every legal evasion, and no pruner touches in-check nodes (`material_bounds_hold` is `!in_check`).
  Its comment changes from "pseudo-legal, `DoMove` rejects" to "legal".

## Validation

Engine tier.

- **Order equivalence (unit, `[movegen]`):** walk the `perft test` suite positions to depth 3 and,
  at every in-check position, compare `ComputeEvasions` with the filtered `ComputeLegalMoves` as
  sequences. Plus named cases: double check, slider check with an x-ray retreat, capture of an
  undefended and of a defended checker, pinned would-be blocker, promotion block, en passant of a
  checking pawn (asserting the move is present, e.g. `e5d6` in `7k/8/8/3pP3/4K3/8/8/8 w - d6 0 1`),
  and the same capture made illegal by a discovered diagonal (a black bishop on c6). Falsified by
  reverting the `occ ^ bit(k)` term and watching it fail.
- **Generators unchanged, independently of the shared helpers:** the evasion oracle shares helpers
  with the code it checks, so an order change in a helper moves both sides. Pin exact, unsorted
  `ComputeLegalMoves` and `ComputeCaptures` sequences (moves with flags) captured at `663e2c5`, over a
  few positions covering both colours, both pawn capture directions, quiet and capture promotions,
  pushes, every officer type and en passant. Falsified by swapping two emitted entries. The existing
  `MoveGenCaptureTests` (which sort) and perft suite pass too.
- **Debug build test run**, so the D2 assert runs across the search tests.
- **`Run-PerftCheck.ps1`** full corpus: no failures outside the rejected-FEN bucket.
- **`Compare-SearchEquivalence.ps1`**: identical nodes and best moves.
- **`Compare-Bench.ps1 -Control`**, ordered pair against the merge base, under `measure-strength`'s
  quiet window: a confirmed positive Speedup, or the change is parked. Forecast, not a bound: low
  single digits.
- **No Elo match:** the tree is identical, so strength follows speed alone. A lab run only if the
  owner wants an Elo reading.

## Cost

- **Size:** 150-250 lines across `MoveGenerator.h/.cpp`, `AIPerplex.cpp`, `Tools/Perft.cpp`; ~200
  lines of tests in a new `StratChessTests/EvasionTests.cpp`, including the pinned generator
  sequences.
- **Blast radius:** engine tier; touches the move-generation contract.
  `Docs/EngineContracts.md`'s moves section gains one line.
- **Review:** one code review (170-270k tokens), plus `search-reviewer` for the `AIPerplex.cpp` part.
- **Optional:** none.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D4 legality rules, pinned-piece proof, en-passant test | comments on `ComputeEvasions` |
| D3 order contract (legal subsequence, same order) | `MoveGenerator.h` declaration, and `Docs/EngineContracts.md` moves section |
| D2 why `DoMove` keeps its check | comment at the Debug assert |
| Bench result | `Docs/Changelog.md`, PR body, #823 |
