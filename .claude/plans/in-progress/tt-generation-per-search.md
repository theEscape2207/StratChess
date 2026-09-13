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

**This change will not:**

- Change `PackedEntry` layout (16 bytes), TT capacity, replacement constants, same-key
  tie-breakers, hash sizing or unrelated stats.
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

## Assumptions I cannot verify from the code

- The Elo effect is unknown. The #544 capacity replay shows replacement changes materially
  (declined same-key stores ~1.8% → ~5.4%, depth +0.05–0.10 ply), which is not a strength result.
  Verified only by the CI strength lab against merge-base; owner approval required.

## Invariants

- Exactly one generation per `Search()` call, independent of depth reached, aspiration retries,
  interruption or thread count. Helpers never advance it.
- `hashfull` / `EvictedCurrentSearch` count every entry written by this search and none from the
  immediately preceding one; the 255 → 0 boundary stays correct.

## Validation

Engine tier. Not behaviour-preserving, so no search-equivalence gate. Focused `[tt]`, `[search]`,
`[uci]` tests plus the full fast suite; a regression test that a multi-depth `Search()` advances
the age by exactly one. Bench for nps (no per-node work is added; expected neutral). Strength: CI
strength lab vs merge-base, owner-approved.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| One generation per search; residual 256-search wrap | source comment on `newSearch()` |
| Per-search vs per-iteration rationale, lab result | `Docs/Changelog.md`, PR body |
| `hashfull` covers one generation | `Docs/TestDesign.md` TT coverage line |
