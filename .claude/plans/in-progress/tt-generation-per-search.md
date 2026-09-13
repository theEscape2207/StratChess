# TT generation per search — Design

**Issue:** #544

## Goal

The TT age is an 8-bit counter advanced once per iterative-deepening depth. Replacement ranks
entries by `(current - entry) & 0xFF` at 512 score units per generation, so recency is periodic:
at a typical 14-depth search an entry's age penalty peaks after ~9 own moves and vanishes after
~18, when it aliases back to "fresh". `hashfull` and `EvictedCurrentSearch` use the same modular
window, so surviving entries from a full cycle ago also count as written this search. The wrap
happens routinely inside lab-length games.

## Scope

**This change will:**

- Advance the TT generation exactly once per `AIPerplex::Search()`, before helpers start.
- Remove the per-depth advance from `iterative_deepening()`.
- Update TT fixtures and comments that describe per-iteration ages.
- Let a deeper same-phase same-key store displace the stored entry regardless of the PV bonus (D4).

**This change will not:**

- Change `PackedEntry` layout (16 bytes), TT capacity, replacement constants, hash sizing or
  unrelated stats.
- Adopt a more permissive same-key overwrite (e.g. Stockfish's "unless >3 plies shallower"); that is
  a separate policy change needing its own measurement.
- Widen the age field.

## Decisions

### D1: Per-search recency, not per-iteration recency

Chosen: one generation per `Search()`. Within a search, depth, phase and node type decide
replacement; the age term only separates searches. Rejected: keeping per-depth ages, which gives
earlier depths of the same search a recency penalty against later ones — content that is still
useful to the current search — and is what makes the counter wrap every ~18 moves.

### D2: Keep the 8-bit age; accept a 256-search residual wrap

The alias period becomes 256 searches by one engine process (normally 256 own moves). That is
beyond practically every game, but not every legal one: captures and pawn moves reset the
fifty-move clock, so an arbitrarily long game can still wrap. After ~256 searches without a
`ucinewgame`/`clear()`, an ancient surviving entry reads as fresh again — the same failure as
today, at a ~14x longer period. Rejected: widening `age` (no spare byte in the 16-byte entry;
repacking alters capacity and needs its own validation) and saturating ages (needs a table sweep).

### D3: Rename `newSearchIteration()` to `newSearch()`

The name states the new contract, so a future caller adding a per-depth bump sees it is wrong.

### D4: A deeper same-key store wins before the ranking is consulted

The issue listed same-key tie-breakers as a non-goal; review showed D1 cannot ship without this.
`replacementScore` prices the PV bonus at two plies to rank *which position* to keep. On the same key
it was also deciding *which result* to keep, so a PV entry at depth d declined a CUT/ALL store at
d+1. Per-depth ages (-512) used to cancel the bonus between iterations and hid this; with one age
per search it would hold for the whole search, discarding deeper results and hash moves exactly
where the PV changes or aspiration fails. Chosen: same phase and greater depth wins outright.
Rejected: dropping the PV bonus at `age_diff == 0` (also changes equal-depth outcomes), and a
Stockfish-style lenient overwrite (larger policy change). Across searches nothing changes: an age
gap already let a deeper incoming store win.

## Assumptions I cannot verify from the code

- The Elo effect is unknown. Verified only by the CI strength lab against merge-base (owner-approved).
  The capacity replay characterises replacement, not strength.

## Invariants

- Exactly one generation per `Search()` call, independent of depth reached, aspiration retries,
  interruption or thread count. Helpers never advance it.
- `hashfull` / `EvictedCurrentSearch` count every entry written by this search and none from the
  immediately preceding one; the 255 → 0 boundary stays correct.
- A same-phase same-key store never loses to a shallower stored entry.

## Validation

Engine tier. Not behaviour-preserving, so no search-equivalence gate. Focused `[tt]`, `[search]`,
`[uci]` tests plus the full fast suite; a regression test that a multi-depth `Search()` advances
the age by exactly one. Bench for nps (no per-node work is added; expected neutral). Strength: CI
strength lab vs merge-base, owner-approved.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| One generation per search; residual 256-search wrap | source comment on `newSearch()` |
| Deeper same-key store beats the PV bonus | source comment in `sameKeyStoreWins()` |
| Per-search vs per-iteration rationale, lab result | `Docs/Changelog.md`, PR body |
| `hashfull` covers one generation | `Docs/TestDesign.md` TT coverage line |
