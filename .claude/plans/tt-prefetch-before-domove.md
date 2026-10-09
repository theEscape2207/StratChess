# TT prefetch before DoMove — Design

**Issue:** #776 (spike #775, triage [comment](https://github.com/theEscape2207/StratChess/issues/776#issuecomment-6079027167))

## Goal

Every child node starts with a TT probe whose bucket is usually not in cache, and the search stalls
on that miss. Spike #775 showed that issuing the bucket's prefetch earlier removes most of the stall:
variant B (prefetch inside `Board::DoMove`) measured **+4.18% [+3.95%, +4.40%] nps** on clang-cl,
ordered pair, A/A +0.13%, node-identical. B reached the TT through a process-wide static on `Board`,
which is not shippable. This change ships the same speed-up without making `Board` know the TT: the
search predicts the child's key before it makes the move and prefetches that bucket (variant C,
Stockfish's `key_after()` pattern). The search is unchanged, so any gain is pure speed.

## Review focus

- **D5, the measurement plan.** C may push both `pvs()` and `quiescence()` across a 64-byte
  boundary, and `New-OrderedBuildPair.ps1` cannot align two resized functions. D5 tries C, then an
  out-of-line shape C′, and stops if neither pair passes.
- **D2, exact rather than approximate prediction.** Exactness costs a few branches per move on
  every node, and that cost is what decides whether C reaches B's gain (Assumption A1).

## Scope

**This change will:**

- add `Board::KeyAfter(const Move&) const` and `Board::KeyAfterNullMove() const`, each exact (D2, D3);
- add `TranspositionTable::prefetch(uint64_t key) const`, sharing the bucket-index expression with
  `probe()` and `store()` (D4);
- prefetch before the three search make-move sites: `pvs()` null move (`AIPerplex.cpp:807`), `pvs()`
  move loop (`:962`), `quiescence()` move loop (`:1527`) (D1);
- add a test that checks `KeyAfter` against the real post-`DoMove` key over move-tree walks rich in
  castling, en passant and promotion;
- measure the final form under placement control (D5) and accept or park it by D6.

**This change will not:**

- change `DoMove`, `DoNullMove`, the Zobrist scheme or any real hash;
- change TT geometry, replacement, probe or store semantics, telemetry or abort-write guards;
- prefetch anything but the TT bucket, or at any site but the three above (root and emergency paths
  are not hot; `has_no_legal_move` and `PVIntegrity` are not search nodes);
- rewrite or extend the benchmark tools;
- run the strength lab or claim Elo. That is an owner-budgeted follow-up.
- implement B. D7 fixes B's contract so that a later slice can start; it does not authorise it.

## Decisions

### D1: The search predicts the child key; Board stays TT-free (variant C)

At each of the three sites, immediately before the make-move call:

```cpp
tt.prefetch(td.board.KeyAfter(move));
if (td.board.DoMove(move)) {
```

The prediction is used **only** to form a prefetch address. It never feeds a probe, store,
repetition check or any decision. A wrong prediction costs one wasted prefetch and nothing else.

Rejected: **B as the primary** (`Board` holds a TT base and mask). It measured best, but it puts
search-owned storage addresses into `Board` and needs the lifetime contract of D7. Its lead time is
also no better than C's: B prefetches part-way through `DoMove`, and C prefetches before it.
Rejected: **prefetch after `DoMove` returns** (A-pvs/A-qs). That needs no prediction, but its lead
time is shorter, and it measured +2.67% summed against B's +3.95% lower bound.

Moves that `DoMove` rejects as illegal, and moves skipped by the post-move futility and LMP guards,
waste their prefetch. B wasted the same ones and still measured +4.18%.

### D2: `KeyAfter` is exact for every move class

Contract: for any move that `DoMove` accepts, `KeyAfter(m)` evaluated before the call equals
`get_zobrist_hash()` after it. It is `const`, so it cannot mutate the board. Its result for a move
that `DoMove` rejects is unspecified but harmless, because only the prefetch address uses it.

It mirrors `DoMove`'s hash updates (`Board.cpp:304–435`):

| Move class | Key terms |
|---|---|
| all | moving piece off `from`; side key |
| quiet, capture, double push | moving piece onto `to`; captured piece off `to` |
| en passant | captured pawn off the square behind `to` |
| promotion (quiet or capture) | pawn off `from`, promoted piece onto `to`; captured piece off `to` |
| castling | the rook's `from`/`to`, as `DoMove` moves it |
| castling rights | `castling_keys[old] ^ castling_keys[new]`, where `new` applies `DoMove`'s revocation rules (king move; rook leaving a corner; any move onto a corner) |
| en-passant square | old `ep_keys[ep]` out if set; new one in on a double push (`MoveHelper::GetEnPassantSquare`, which sets it unconditionally) |

The castling-rights rules copy `DoMove`'s if-chain rather than using a from/to square mask. A mask
on `from` would revoke rights for a non-rook piece leaving a corner, which `DoMove` does not do.

Rejected: **Stockfish-style approximate** (piece moves, captures and side only). It is a few
instructions cheaper, but:

- it mispredicts **every** child of a node whose position has an en-passant square, i.e. every
  node after a double push;
- it mispredicts every child whose castling rights actually change (a king move while its side
  still has rights, a rook leaving a corner, any move onto a corner), and every promotion, en
  passant and castling move;
- its contract would need a list of exceptions, and a test could not catch the cases drifting. An
  exact contract is one sentence and an exhaustive walk enforces it.

The price of exactness is a duplicate of `DoMove`'s hash logic. The test of D2's contract (see
Validation) is what keeps the two in step.

### D3: `KeyAfterNullMove` removes the en-passant key

`KeyAfterNullMove() == hash ^ side_key ^ (ep != NO_SQUARE ? ep_keys[ep] : 0)`. This matches
`DoNullMove` (`Board.cpp:820–841`), which drops a pending en-passant right before it flips sides.
The issue's `hash ^ side_key` is exact only without an en-passant square. Being exact costs one
branch, so the null move gets the same one-sentence contract as D2.

### D4: `TranspositionTable::prefetch` shares the bucket index

```cpp
void prefetch(std::uint64_t key) const noexcept
{
	_mm_prefetch(reinterpret_cast<const char*>(&table[bucket_index(key)]), _MM_HINT_T0);
}
```

A private `bucket_index(key)` replaces the `static_cast<size_t>(key) & index_mask` expression now
written out in both `probe()` and `store()`, so the prefetched bucket cannot drift from the probed
one. The codegen is unchanged. Buckets are 64 bytes and 64-byte aligned, so one prefetch covers one
bucket.

`_mm_prefetch` comes from `<immintrin.h>`, which `StdAfx.h` already includes, and is available on
all three compilers. The engine is x86-64 only (PEXT magics), so no `#if` is needed. Rejected: the
spike's `__builtin_prefetch` with an MSVC `#else`, which was two code paths for one instruction.

A prefetch has no C++-observable effect and never faults. Under Lazy SMP it is therefore not a data
race, and it cannot be one even when another thread is storing to the same bucket.

### D5: Measurement — one ordered pair, merge base against the final form

Every measurement uses same-toolchain Release clang-cl builds, and every one follows skill
`measure-strength`, including the quiet-window rule. Exactly one long benchmark runs: on the first
shape below whose ordered pair passes. Pairs are relinks and take minutes, so they all happen before
any quiet window.

Every function starts on a 64-byte boundary (`-falign-functions=64`, `CMakeLists.txt:295`), and the
tool's size covers the tail padding. A hot function therefore counts as resized only if its growth
crosses a 64-byte boundary.

1. **Shape C (inline).** The D1 call sites, with `KeyAfter` and `prefetch` free to inline. Build it
   and the merge base, then run `New-OrderedBuildPair.ps1`. If the pair passes, run one
   `Compare-Bench.ps1 -Control -Rounds 60 -Affinity 4` (about 40 min quiet). That result is the
   verdict.
2. **Shape C′ (out-of-line)**, only if C's pair fails. Prediction and prefetch move into two
   `STRAT_NOINLINE` free functions in `AIPerplex.cpp`: `prefetch_child(tt, board, move)` and
   `prefetch_null_child(tt, board)`. Each site's growth shrinks to argument setup plus one call. This
   does not guarantee a pass, so re-run the pair. If it passes, run the same single benchmark on C′.
   C′ is the form that then ships, and its extra call per node is inside what is measured.
3. **Neither pair passes.** Stop before any quiet window and report the two maps' hot-function sizes.
   The owner then chooses between a bounded `New-OrderedBuildPair.ps1` issue and parking. This
   outcome resolves nothing and does not trigger B.

Rejected: **a chain of two ordered pairs** (base → pvs sites, then → quiescence site). Each pair
orders the intermediate build differently, so the intermediate image differs between the two
comparisons, and the product of the two ratios carries an unmeasured layout ratio between them.
Rejected: **an uncontrolled comparison.** Placement alone moved a node-identical build by 3.9%
(#555).

An optional GCC trend (`Compare-BenchLinux.ps1 -Rounds 24 -Affinity 4`, about 20 min) is reported
as a trend only and decides nothing.

### D6: Acceptance and park rule

- **Accept C** (or C′) if its single valid run has a 95% lower bound of **≥ +3.5%** and its whole
  A/A interval lies inside ±0.5%. +3.5% is a practical
  floor that the owner chose next to B's +3.95% lower bound. It is not a claim of equivalence to B.
- **A valid miss** (A/A clean, pair passed, lower bound < +3.5%) ends C. B (D7) becomes a separate
  slice, with its own approval and the same measurement. If B also misses, park the issue. The
  spike's historical number is never shipped.
- **An invalid run** (A/A outside the band, a pair failure, a time loss or crash) resolves nothing.
  Re-run in a quieter window; it does not trigger B.
- The PR reports per-position results for every run. Every variant tried is retained, in the PR
  body and under `StratChessSupport/` beside the #775 artifacts.

### D7: B's contract, if it is ever needed

The hook is instance-owned. Each `Board` has a `{const void* base; uint64_t mask}` pair, defaulting
to a static 64-byte-aligned dummy bucket with mask 0, so `DoMove` stays branch-free. `AIPerplex`
sets the pair on the boards it owns (`td_.board` in `init_search`, and each helper's `htd.board`
after its copy from `root`) and resets them to the dummy when the search ends. On every exit,
exceptional ones included, the order is: stop, join the helpers, reset the hooks, finish the launch.
In `Search()`'s RAII order (`launch_guard`, helper vector, `stop_guard`, `AIPerplex.cpp:335–340`)
the reset guard is declared after `launch_guard` and before the helper vector, so reverse
destruction runs it after the joins; it skips helpers that were never created. The rest follows from
that:

- **Board copies.** A copy carries the hook with it. A copy taken from the caller's root board
  carries the dummy, and the search's own boards are set after they are copied.
- **The TT's lifetime.** `SetHash` replaces the TT only while no search runs
  (`AIPerplex.cpp:223`), and the hook is reset at the end of the search, so a hook never names a
  table that is not live.
- **Non-search callers** (game loop, UCI replay, perft) keep the dummy and pay one prefetch per
  move to the same cached 64 bytes.

Next to C, B adds two fields on a hot object, a set/reset pair that every search exit path must
honour, and a rule about which boards own a hook. C adds none of that. B is a fallback only.

## Assumptions I cannot verify from the code

- **A1: C's gain is at least B's minus `KeyAfter`'s cost, and that cost is small.** C's lead time is
  at least B's, but `KeyAfter` reloads the moving and captured pieces and branches on the move type.
  Not verified. D5's measurement settles it, and D6 handles a miss.
- **A2: C's or C′'s ordered pair passes**, which needs at most one hot function's growth to cross a
  64-byte boundary. Not verified. Building the pairs settles it in minutes, before any quiet window,
  and D5 step 3 covers a double failure.
- **A3: `_mm_prefetch(_MM_HINT_T0)` emits `prefetcht0` on clang-cl, MSVC and GCC**, the instruction
  the spike's `__builtin_prefetch` produced. This is documented compiler behaviour. Not checked here;
  a disassembly of the bench binary would settle it.

## Invariants

- The search is node-identical. `Compare-SearchEquivalence.ps1` against the merge base reports
  IDENTICAL per-iteration scores, nodes, PVs, telemetry and best moves at `Threads=1`.
- Real hashes, `DoMove`/`UndoMove` state, move legality, TT semantics and abort-write guards are
  unchanged. The diff touches none of them.
- A predicted key only ever forms a prefetch address. This is source-commented at `KeyAfter`, and
  checked in review.
- `KeyAfter` and `KeyAfterNullMove` meet D2's and D3's contracts for every move `DoMove` accepts,
  and they do not mutate the board.

## Validation

**Engine tier** (`Validate-PrePR.ps1` scopes it). No Elo match is needed to accept the change: it
does not change a single node, so the effect is pure nps, and D6 gates on nps. A lab run is the
owner's call afterwards.

- **Contract test (D2, D3).** A recursive walk to depth 3 over Kiwipete and the CPW positions that
  are rich in en passant, promotion and castling. For every pseudo-legal move, record `KeyAfter(m)`
  and the board's hash. If `DoMove(m)` succeeds, require the new hash to equal the prediction, then
  undo and require the hash to equal the one recorded before. Do the same for `KeyAfterNullMove()`
  around `DoNullMove()` at every node that is not in check, so a position with an en-passant square
  is covered. The test also requires that each special class (en passant, each castle, quiet and
  capture promotion, a rights change, a null move with an en-passant square) was checked at least
  once, so a corpus change cannot silently drop one. Falsify the test: remove the old-ep term and the castling-rights term in turn, and
  watch it fail.
- **Equivalence.** `Compare-SearchEquivalence.ps1 -After .\build\windows-clang-cl\StratChessEvolved.exe
  -BaselineRef <merge base>` reports IDENTICAL. Fixed-depth equivalence says nothing about lifetimes or aborts, and C adds no
  lifetime.
- **Speed.** D5 and D6.
- Debug + sanitizer CI runs the new test and every search test through the prefetch path.

## Cost

- **Size:** 50–200 lines over 5 files: `Board.h`, `Board.cpp`, `TranspositionTable.h`,
  `AIPerplex.cpp` and a test file, plus the changelog. Exactness is about 30 of those lines.
- **Blast radius:** Engine tier. No gate, skill or doc changes beyond the changelog.
- **Review:** one code review (170–270k tokens, 3–5 min).
- **Quiet machine time (owner approval needed):** about 40 min, once. Builds and pair relinks for C
  and possibly C′ are extra, but they are not quiet time.
- **Optional:** the GCC trend, about 20 min quiet. It covers the lab toolchain's direction only. A B
  slice, if D6 triggers it, is another 40 min.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| D1, prediction is a hint only; D2/D3 exact contract and why not approximate | source comment on `KeyAfter` / `KeyAfterNullMove` in `Board.h` |
| D4 shared bucket index; prefetch is not a data race | source comment on `TranspositionTable::prefetch` |
| D5 shape shipped (C or C′) and why C′'s out-of-line call exists, if used | source comment on `prefetch_child`; PR body |
| Measured result, per-position, all variants tried | PR body, `Docs/Changelog.md`, `StratChessSupport/` |
| D7 B contract, if C misses | the B slice's issue |
