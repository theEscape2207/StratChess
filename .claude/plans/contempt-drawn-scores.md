# Contempt for drawn scores — Design

**Issue:** #452

## Goal

Every draw the search detects scores exactly `GameValues::Draw` (0), so "draw now" and "equal
position, play on" are indistinguishable and the engine takes a repetition whenever the alternative
evaluates at or below 0. The Tier 1 move-quality scan of run `33215162562` puts 22.0% of 19,980
games (4,393) on threefold repetition, with 99.9% of them ending with both sides reporting under
50 cp — the engine repeats in positions it believes are equal. Whether declining those draws is
worth Elo is a playing-policy question, not a bug, and its sign is not obvious. This change adds the
mechanism (a signed contempt offset on search-detected draw scores) and the instrument to settle it,
shipping the offset disabled so the merge itself cannot change playing strength.

## Scope

**This change will:**

- Add one `SearchTuning` field, `contempt`, default `0`, domain `[-100, 100]`, exposed over UCI as
  `Contempt` and bindable from `game_settings.json`.
- Route the four search-detected draw endpoints through one helper, `draw_score(td)`:
  `pvs()`'s `check_draws` exit (`AIPerplex.cpp:652`), `quiescence()`'s `check_draws` exit
  (`:1258`), the stalemate return in `adjustScoreForGameState()` (`:1133`), and the bare-king
  stalemate return and its TT store in `quiescence()` (`:1313-1317`).
- Capture the root side to move per search and clear the transposition table when it flips while
  contempt is non-zero (D3).
- Measure the term in the CI strength lab against the merge base at `Contempt=20`, with the
  ship/reject rule recorded below **before** the run.

**This change will not:**

- Change the abort and time-limit unwind returns (`:646`, `:649`, `:1224`, `:1227`). Those are
  fabricated values for a frame that searched nothing, not game results, and must stay neutral.
- Change mate or stalemate *adjudication*, `Evaluator::Evaluate`'s scale-0 dead-drawn material
  return (`Eval.cpp:1088`), or any evaluation term. Contempt belongs to the search's terminal
  scores, not to static evaluation.
- Decay contempt with game phase, material deficit or remaining time (D6).
- Enable contempt by default. A default change is a separate, measurement-gated PR.
- Resolve #347 (halfmove clock outside the TT identity) or run a weaker-opponent panel (D4, D5).

## Decisions

### D1: Sign from the node's side to move against the captured root colour, not ply parity

`draw_score(td)` returns `td.board.GetCurrentColor() == root_color_ ? -contempt : +contempt`, so a
positive `contempt` makes a draw a small loss **for the side the engine is playing**, expressed in
the negamax perspective of the node that reports it. Getting this backwards makes the engine avoid
draws when it is losing, which is the exact failure mode #452 names.

Ply parity (`ply % 2`) is the cheaper and more common formulation and was rejected: null-move
pruning flips the side to move while *incrementing* ply (`AIPerplex.cpp:770-772`), so below a null
move parity no longer tracks the side to move and the sign inverts for that whole subtree. Reading
the board's colour costs one member read on a cold path — draw endpoints only, never per node — and
is correct under null moves, singular verification frames (which re-enter at the parent's ply and
the parent's colour) and any future ply-shifting extension.

`root_color_` is an `AIPerplex` member written in `Search()` before helper threads are spawned and
read-only for the rest of the search, exactly as `tuning_` already is. Helpers seed from the same
root board, so they share the value.

### D2: A runtime integer option defaulting to 0, not a compile gate

`contempt` is one `TUNING_FIELD` entry. `0` reproduces today's arithmetic exactly — the helper
returns `GameValues::Draw` unchanged — which makes acceptance criterion 1 a compile-time-free
identity rather than a second code path. A compile gate was rejected: the feature has no hot-path
cost to gate away, and standing practice is that a compile gate is a temporary state whose end point
is a default-true runtime flag or deletion. The domain `[-100, 100]` admits negative values, which
are the natural way to *seek* draws (a defensive or losing-on-time posture) and cost nothing to
allow; the value under test is `20`, mid-range of the typical 10–30 cp.

### D3: Clear the TT when the root colour flips, gated on contempt being non-zero

A contempt-derived draw score propagates into parent entries, so a stored bound depends on which
colour the search was favouring — context the Zobrist key does not carry. This is the root-
perspective hole the triage flagged, and it is separate from #347's clock hole.

In normal play the hole is unreachable: a UCI engine searches only its own moves, so the root colour
is constant for a whole game and `StartNewGame()` clears the TT between games. It becomes reachable
only when a caller searches alternating colours without `ucinewgame` — analysis, the tactical
runner, tests. So: `Search()` compares the root colour against the previous search's; if they differ
and `tuning_.contempt != 0`, it clears the TT before searching. `StartNewGame()` resets the stored
colour to unset.

Rejected alternatives: encoding the root colour in the entry (no spare bits — #534 left
`PackedEntry` at 16 bytes with 3 reserved metadata bits, and #347 already wants them); suppressing
score reuse across perspectives (same storage problem, plus it costs cutoffs in the common case
where the perspective never changes); accepting the pollution (bounded at 2×contempt, but it is an
unforced silent wrong answer in exactly the analysis setting where a user would notice). The chosen
option costs a TT clear per colour flip in analysis and *nothing at all* at the shipped default,
where the guard is one integer comparison per search.

### D4: #347 is not a blocker and this change does not widen it

The rule-50 draw score already depends on the halfmove clock and is already cached under a
clock-independent key; #347 measured that error at 535 cp. Contempt changes the *value* of such an
entry by at most `|contempt|` (≤ 100 cp by domain, 20 cp as tested) and introduces no new context
into the key — the clock dependency is unchanged in kind and in reachability. #347 is parked with
zero observed rejections (#549). Treating it as a hard blocker was therefore rejected; the
interaction is recorded here and in the source comment on the helper.

### D5: Pre-registered ship/reject rule for the strength lab

The lab plays the candidate against a near-equal merge base. Contempt is defined relative to the
opponent's strength, so peer self-play is close to its worst case and may measure ~0 for a term that
helps against a weaker field. Deciding the bar afterwards would make any outcome a pass, so it is
fixed here. One run, `Contempt=20`, candidate vs. merge base, ~20,000 games, ~±4 Elo:

| Outcome | Action |
|---|---|
| CI lower bound > 0 | Open a follow-up PR flipping the default to 20, citing this run. |
| CI contains 0 | Merge as-is, default 0. Contempt is a neutral-in-peer-play style option; record the interval and close #452 on that basis. |
| CI upper bound < 0 | Merge as-is, default 0, and record that contempt at 20 cp is measurably harmful in peer play. Do not test further magnitudes without new evidence. |

A weaker-opponent panel is **not** required to merge this PR, because this PR makes no
weaker-opponent claim. It would be required before anyone asserts "contempt gains Elo against weaker
fields" — that is a separate experiment and a separate issue.

Acceptance criterion 4 of #452 ("the threefold share must move") is replaced, per the triage's item
6, by a measured before/after: re-scan the candidate PGNs with the Tier 1 scan and report the
threefold-repetition share and W/D/L against the same figures from the reference side of the same
run. Activity is demonstrated by a difference, not by a threshold on 22.0%.

### D6: No phase, deficit or time-based decay

Decaying contempt toward 0 in the endgame or when far behind on material is standard in some
engines and was rejected for now: there is no evidence in the scan for any particular decay shape,
and adding one before the flat term has been measured moves two variables at once, answering
neither. A flat term is the thing the single lab run can attribute.

## Assumptions I cannot verify from the code

- **The lab's ~±4 Elo interval at ~20,000 games holds for this run.** Taken from skill
  `measure-strength` and past runs, not re-derived. Verified by the run's own reported interval; if
  it comes back materially wider, the D5 table is applied to the interval actually measured, not to
  the assumed one.
- **`Contempt=20` is a sensible first magnitude.** Taken from the 10–30 cp range in #452, which
  cites general engine practice rather than a repository measurement. Not verified, and not
  verifiable without spending further lab runs on a magnitude sweep; the D5 table deliberately does
  not treat a null result at 20 as a verdict on every magnitude.
- **Nothing outside `AIPerplex` reads a drawn score and depends on it being exactly 0.** Checked by
  grep for `GameValues::Draw` across `StratEngine` — the remaining uses are the abort returns this
  change leaves alone and `Eval.cpp`'s scale-0 path, which is out of scope. Closed by the
  zero-contempt equivalence run in Validation rather than by the grep alone.

## Invariants

1. With `contempt == 0`, the search is node-identical and move-identical to `origin/main` at
   `Threads=1`. No new work runs on any per-node path at any contempt value: the helper is called
   only at draw endpoints, and the D3 guard once per `Search()`.
2. A drawn score is negative for the side the engine is playing and positive for its opponent, at
   every node, including below a null move and inside a singular verification frame.
3. Abort and time-limit unwind returns stay exactly `GameValues::Draw` at every contempt value.
4. No TT entry produced under one root colour is consumed by a search under the other while
   contempt is non-zero.
5. `Validate()` rejects a contempt outside `[-100, 100]`; the UCI `Contempt` spin advertises that
   domain.

## Validation

Search tier. Evidence, in the order it is produced:

- **Unit tests** (`StratChessTests/SearchContemptTests.cpp`, `[search][contempt]`), driving `pvs()`
  through `AIPerlexTestFixture::search_node_after`:
  - `contempt == 0` returns exactly `GameValues::Draw` at a repetition, at the fifty-move limit and
    at a stalemate — invariant 1's local half.
  - Both root colours, on a repetition position: the returned score is `-contempt` when the drawing
    node's side to move is the root colour and `+contempt` when it is not. Asserted at an even and
    an odd ply, and once below a null move, so ply parity alone cannot pass the test — invariant 2.
  - The abort path returns `GameValues::Draw` with `contempt = 50` set — invariant 3.
  - The flip-clear fires exactly when D3 says: plant a marker entry, run `Search()` on a root of
    the other colour with `contempt = 20`, and require the marker to be gone; repeat with
    `contempt = 0` and require it to survive; repeat on a same-colour root and require it to
    survive — invariant 4, and the inert-at-default half of invariant 1.
  - `SearchTuningTests.cpp`: domain rejection at ±101 and acceptance at ±100; `UCITests.cpp`: the
    `Contempt` option is advertised with the right type and bounds — invariant 5.
- **`Compare-SearchEquivalence.ps1 -After <worktree exe>`** at the default `Contempt=0` — identical
  node counts and best moves against the merge-base build at `Threads=1`. This is the gate for
  invariant 1 and closes the third assumption above.
- **`Run-Bench.ps1`, alternating, candidate vs. merge base.** Not required by the standing rule —
  the diff adds no per-node work (invariant 1) — but it is cheap and it is the only thing that would
  catch an accidental hot-path read, e.g. if the D3 colour capture were placed inside `pvs()`
  instead of `Search()`. Pass bar: nps within the run's own noise band.
- **Full test suite** including `[slow]`, then `Validate-PrePR.ps1`.
- **One CI strength-lab run** against the merge base at `Contempt=20`, judged by the D5 table, plus
  the Tier 1 move-quality re-scan of the candidate PGNs described there. An Elo match *is* required
  here: the whole point of the issue is that the sign of this policy is unknown, and no cheaper
  instrument resolves single-digit Elo.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| Why the sign comes from the board's colour and not ply parity (null move breaks parity) | source comment on `draw_score()` |
| Why abort/time-limit returns stay at neutral zero | source comment at the `draw_score()` call sites' neighbours in `pvs()`/`quiescence()` |
| Root colour is TT context the key does not carry; the flip-clear is what closes it, and it is inert at contempt 0 | source comment on the guard in `Search()`, and `Docs/EngineContracts.md` → search internals |
| Contempt does not widen #347, and why | comment on #347, so the parked issue records it |
| The pre-registered D5 bar, the measured interval and the re-scan numbers | `Docs/Changelog.md`, the PR body, and a comment on #452 |
| Whether the default should change | follow-up issue or PR, per the D5 table |
