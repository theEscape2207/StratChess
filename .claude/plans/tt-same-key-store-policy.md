# TT same-key store policy — Design

**Issue:** #319 (measurement record: #335)

## Goal

`TranspositionTable::store()` short-circuits on `entry.key == key` and overwrites the slot with no
regard for phase, depth or bound. The replacement policy that the rest of `store()` applies — main
content outranks the entire quiescence band, PV entries carry a bonus, deeper wins — is skipped on
exactly the path where the two entries describe the *same position*, so the comparison is most
meaningful. Measured on `090e3f8` at `Threads=1`, `go depth 12` over four positions, this costs the
engine its hash move at **21 of 197** principal-variation nodes that lay on the previous iteration's
accepted PV: 18 because a quiescence store replaced the main entry, 3 because a `MAIN` store carrying
`Move::EmptyMove()` erased the move. Capacity eviction accounted for none of the 197.

## Scope

**This change will:**

- Make the key-match path in `store()` consult `replacementScore()` instead of overwriting blindly.
- Preserve the stored `best_move` when an overwriting store for the same key carries an empty one.
- Re-point the test that pinned the old behaviour, and add tests for the new one.

**This change will not:**

- Change `replacementScore()`, the phase penalty, the quiescence discount, or the eviction path for
  *different* keys. Those were settled in #318 and this change deliberately reuses them unaltered.
- Let `quiescence()` mine `MAIN` entries for cutoffs. A declined quiescence store means the qsearch
  cache is now missing at those keys, and letting qsearch use the main entry there is the obvious
  answer — but it is a second behaviour change with its own measurement, so it belongs in its own
  issue.
- Refresh the age of an entry whose overwrite was declined. That would keep a retained entry alive
  longer against *other* keys, which is a change to the eviction path this document excludes.

## Decisions

### D1: Reuse `replacementScore()` for the key-match decision rather than writing a phase/depth rule

The issue lists three candidate fixes, ascending in ambition: preserve `best_move` only; keep main
against incoming quiescence unless deeper; depth-preferred replacement generally. The third,
expressed as "the incoming entry must score at least as high as the one it replaces", subsumes the
second and needs no new arithmetic: `replacementScore()` already encodes phase, depth, node type and
age, and its phase penalty already places the whole quiescence band below main-search depth 0. A
bespoke `if (existing.phase == MAIN && incoming.phase == QUIESCENCE)` rule would be a second policy
to keep in step with the first — which is how this hole appeared in the first place.

Rejected: leaving the key-match path with its own hand-written rule. Also rejected: candidate 1
alone, which closes 3 of the 21 measured cases and leaves the dominant 18 open.

Ties overwrite (`>=`). The incoming entry always carries the current age, so a tie means equal depth,
phase and node type at the same age — the PVS re-search case, where the fresher bound is the one
worth keeping.

The price of reusing one ranking is that the PV bonus, which on the eviction path means "this entry
is more useful to keep", now also acts on a comparison between two claims about the *same* position,
where depth is what makes a claim stronger. At 512 against a ply's 256, a non-PV store has to be two
plies deeper to displace a PV entry, so a transposition that reaches the position one ply further
from the root and searches it deeper is declined. That costs a cutoff, never soundness — the retained
entry is still a sound claim — and it is bounded to one iteration, since a generation of age is
-512 and cancels the bonus exactly. Accepted rather than special-cased: a same-key exception to the
PV bonus would be the second policy this decision exists to avoid.

### D2: Preserve `best_move` on the overwrite path when the incoming move is empty

Needed *in addition to* D1, because the 3 empty-move cases are `MAIN` stores from `pvs()` itself —
the null-move cutoff and the terminal mate/stalemate store — which can legitimately outscore the
entry they land on and so pass D1's test. Keeping the old move is safe: `hash_move` reaches nothing
but `MoveSorter::ScoreMoves()`, where it is matched against the freshly generated legal moves, and it
was produced for this same key. A stale hint costs ordering quality at worst, never legality.

### D3: A declined store is silently dropped, not merged

No attempt is made to fold the incoming value into the retained entry, or to refresh its age. The TT
is a cache: dropping a store loses a cutoff, never correctness.

## Assumptions I cannot verify from the code

- **That the 21 measured misses are the whole of the damage.** Taken from #335's instrumented run on
  `090e3f8`, not re-derived here. Verified by re-running the same instrumentation before and after
  (see Validation) rather than trusted.
- **That declining quiescence stores at keys already holding a main entry does not cost more in
  re-searched qsearch nodes than the improved ordering saves.** Not knowable from the code; that is
  what `Run-Bench` and the SPRT are for.

## Invariants

- A probe for a key that has ever been stored and not evicted returns an entry whose content came
  from one real store — no field is ever a mix of two stores, except `best_move`, which may be
  carried forward from an earlier store for the same key (D2).
- `entry_count` and `pv_count` are untouched by a declined store.
- The eviction path for different keys is bit-identical to `origin/main`.
- Quiescence content still never outranks main content on depth alone, in either path.

## Validation

Engine tier. This changes what the table holds under normal search, so it is **not** a
fixed-depth-equivalence change: `Compare-SearchEquivalence.ps1` is expected to differ and is not run
as a gate.

- **Unit** — `[tt]` tests for: a quiescence store declined against a main entry for the same key; a
  shallower main store declined against a deeper one; an equal-depth store accepted; `best_move`
  preserved when the incoming store carries an empty one; counters unmoved by a declined store.
- **Mechanism** — #335's instrumentation re-created as a temporary patch (not committed) and run on
  the same four positions at `Threads=1`, `go depth 12`. Success criterion, stated on #319:
  **197 examined, 197 usable, 0 misses**. Met, as **221 examined / 221 usable / 0 misses** — the
  before run reproduced #335's 197/176/0/18/3 exactly, and the population grows because the PV
  itself lengthens once the ordering holds.
- **Speed** — `Run-Bench.ps1`, both binaries clang-cl Release. Better ordering should cut nodes; the
  declined quiescence stores push the other way, so the sign is not predictable in advance.
- **Strength** — SPRT against `elo-reference-v2`. Required before merge; not started unattended.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the key-match path scores instead of overwriting (D1) | source comment in `store()` |
| Why an empty incoming move keeps the old one (D2) | source comment in `store()` |
| "An exact key match always overwrites in place" is no longer true | `Docs/Engine-Readme.md` |
| Before/after PV-node hash-move availability, bench, SPRT | `Docs/Changelog.md` + PR body |
| qsearch could mine `MAIN` entries — deliberately not done here | follow-up issue |

No approved decision changed during implementation. D1 and D2 landed as written; the measurement
confirmed the "before" figures this document takes from #335 exactly (197 / 176 / 0 / 18 / 3).
