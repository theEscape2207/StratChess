# The PV-move ordering hint is redundant with the transposition table — Design

**Issue:** #299, #310 (this supersedes D7 of `abort-unwind-and-pv-integrity.md`)

## Goal

`MoveSorter::ScoreMoves` has a top scoring tier — 2,000,000, above `hash_move`'s 1,900,000 — for a
"previous iteration's PV move" that is never supplied. `pvs()` reads it from `td.pv_table` at a ply
whose row the same call cleared on entry, so `get_pv_move(ply)` returns an empty move at every node,
and an empty `Move` is `h1 → h1`, which no generated move can equal. The tier has therefore never
fired anywhere, and ordering has been hash-move-first in practice for as long as the code has
existed.

D7 of `abort-unwind-and-pv-integrity.md` planned to revive it: snapshot the accepted iteration's root
line and offer `snapshot[ply]` while the search path is still a prefix of that line. That was
implemented and measured, and the measurement says the hint has almost nothing to supply. This
document records what was measured and proposes deleting the tier instead.

## What was measured

The revival was implemented in full (`ThreadData::pv_hint`, path tracking through the move loop and
the null-move path, snapshot on an accepted iteration) and instrumented at the point of use. Counts
are per **entire** depth-12 search at `Threads=1` — each of these searches visits millions of nodes.

| | startpos | kiwipete | open-mid | closed-mid |
|---|---|---|---|---|
| hint offered at all | 55 | 56 | 55 | 55 |
| …agrees with the hash move | 48 | 50 | 47 | 52 |
| …**differs** from the hash move | 0 | 0 | **1** | 0 |
| …TT held **no** move for the node | 7 | 6 | 7 | 3 |
| TT entry depth vs node's remaining depth (deeper/equal/shallower) | 0/0/48 | 0/1/49 | 0/0/47 | 0/0/52 |

Three things follow, and none of them was visible from the code.

**The hint can only fire ~55 times per search.** It is gated on `is_pv_node && ply > 0` and on the
path still following the snapshot, so at most one node per ply per iteration qualifies. 55 is the sum
of the PV lengths over the iterations, not a fraction of the tree.

**Where it fires, it names the move the TT already names.** It differed once across four complete
searches. This is not luck: the entry at a PV node is that node's own store from the previous
iteration, which is where the snapshot came from too. The depth row confirms the mechanism — the
entry is nearly always *shallower* than the node's remaining depth, i.e. it is last iteration's, not
some deeper search's.

**So the whole content of the revival is the 3–7 nodes per search where the TT lost the entry** to
replacement and the hint supplies one.

Fixed-depth cost, `Run-Bench.ps1` depth 12, `Threads=1`, revival vs `origin/main`: total nodes
+3.8%, wall clock +4.5%, per-position nps flat (2.89M → 2.91M — the hint adds no measurable per-node
cost). Per position the node count swings from −29% (`closed-mid`) to +48% (`open-mid`, which also
changed its best move at depth 12). Those swings are ~5 high-leverage PV nodes getting a different
first move; the sign is per-position luck, and eight positions cannot resolve it either way.

## Scope

**This change will:**

- Remove the `pv_move` tier from `MoveSorter::ScoreMoves` and the `pv_move` parameter from its
  signature, along with the dead local in `pvs()` that feeds it.
- Keep `PVTable::get_pv_move`, which has a live caller in `iterative_deepening()`
  (`metrics.current_move`).

**This change will not:**

- Land the D7 revival. Its implementation and tests are on `worktree-299-pv-move-ordering` and are
  abandoned by this document, not merged.
- Touch any other ordering tier — hash move, killers, MVV-LVA, history are all unchanged.
- Change the transposition table's replacement policy. "The TT sometimes loses a PV node's entry" is
  a real observation here (3–7 per search) and might be worth its own issue, but a replacement-policy
  change is a different change with a different risk profile.

## Decisions

### D1: Delete the tier rather than land the revival

Rejected: **land the revival as implemented.** It is correct — the tests cover the path-prefix rule,
the flags comparison and the null-move break — and it is what D7 approved. But it buys a different
first move at 3–7 nodes per search, its fixed-depth effect is indistinguishable from noise at eight
positions, and the only instrument that could settle it is an SPRT whose prior is ~0 Elo. Keeping
code that survives on "an SPRT could not prove it harmful" is how a codebase accumulates.

Rejected: **score the hint below the hash move, or use it only when the TT has no move.** These read
as the cautious middle and are not: the two moves differ at ~0–1 nodes per search, so every variant
collapses into the same change. There was no variant to test.

Chosen: **delete.** The tier is dead today, so removing it is provably behaviour-preserving, and it
takes one comparison per move out of the sorter's innermost loop plus one dead concept out of the
search's mental model.

### D2: The deletion is proved by fixed-depth equivalence, not argued

`mv == pv_move` is always false today because `pv_move` is always the empty move and `Move` equality
compares from/to only, so the empty move reads as `h1 → h1`, which move generation never produces.
That argument is sound but it is an argument. `Scripts\Compare-SearchEquivalence.ps1` (#330) turns it
into evidence: identical per-iteration `info` lines and `bestmove` across six positions at depth 12,
`Threads=1`, against a baseline built from `origin/main`.

### D3: The TT losing PV-node entries is recorded, not fixed

The instrumentation found the TT holding no move at 3–7 PV nodes per search. That is the only place
the hint had anything to add, and it is a fact about replacement policy, not about move ordering.
Filed as an issue rather than addressed here, because a replacement-policy change needs its own
measurement and would be invisible inside this diff.

## Assumptions I cannot verify from the code

None outstanding. The one assumption this rests on — that the hint and the hash move name the same
move in practice — was the thing measured rather than assumed, and the counts above are from the
shipping toolchain at `Threads=1`, where they are deterministic.

The counts come from a **single-threaded** search. Under Lazy SMP each helper keeps its own PV table
and its own snapshot while sharing the TT, so a helper could see a TT miss where thread 0 does not.
That would change the 3–7 figure, not the conclusion: the deleted tier is dead on every thread,
because `get_pv_move(ply)` is empty on every thread for the same reason.

## Invariants

- Move ordering is unchanged at every node: the removed tier never selected a move.
- Identical node counts and best moves at `Threads=1`, before and after.
- `PVTable::get_pv_move` keeps its live caller in `iterative_deepening()`.

## Validation

Engine tier.

- **Fixed-depth equivalence** via `Compare-SearchEquivalence.ps1 -BaselineRef origin/main`: identical
  per-iteration `info` lines and `bestmove`, six positions, depth 12, `Threads=1`. This is the whole
  correctness argument, and it is a gate rather than a spot check.
- `Run-Bench.ps1` before/after. The expectation is no measurable change — one integer comparison per
  move is below the instrument's resolution — and the run exists to confirm it, not to claim a win.
- **No Elo match.** Behaviour is bit-identical by the check above, so there is no strength question
  to answer. This is exactly the case where the equivalence check replaces an SPRT rather than
  supplementing it.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| That the hint was dead, and that the TT already names the same move | source comment where the tier was, in `Sort.cpp` |
| The measured counts and the equivalence result | `Docs/Changelog.md`, and the PR body |
| D1 superseding D7 of `abort-unwind-and-pv-integrity.md` | PR body, and a closing comment on #299 |
| The TT losing PV-node entries at 3–7 nodes per search | a new issue (D3) |

**Changed against an approved decision:** D7 of `abort-unwind-and-pv-integrity.md` approved reviving
the hint; this reverses that on evidence gathered by implementing it. `abort-unwind-and-pv-integrity.md`
and `299-progress.md` are deleted with this change, since PR 2 was the last landing they were waiting
on and their remaining Harvest rows are discharged here.

Delete this file once the table above is discharged.
