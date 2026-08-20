# One reversible position record on the Board; the outcome leaves it — Design

**Issue:** #348 (Stage 2)

## Goal

Stage 1 made `Board` the only object that answers what the en-passant square, castling rights,
halfmove clock and last move are. It did not change how the board *stores* them.

`GameInfo` still carries two unrelated things in one struct — reversible per-move position state, and
the game's outcome — and the board's undo stack is still four parallel `MAX_PLY` arrays that every
make writes separately. Three costs follow. `gameState` rides a per-ply save/restore it never needs,
because it is only ever written under a `ply == 0` guard. `DoMove` and `DoNullMove` scatter four
stores across four 256-entry arrays where one contiguous record would do. And the undo state is
9,472 bytes per `Board` — copied whole for every Lazy SMP helper thread.

This change splits `GameInfo` in two. The reversible half becomes a single `PositionState` record
that absorbs all four arrays; the outcome half becomes a per-search value reported through the
`SearchResult` channel Stage 1 built. `GameInfo` is then deleted rather than kept as a shim.

## Scope

**This change will:**

- Add a private `Board::PositionState` (24 bytes, laid out in D2) and replace `zobrist_history_`,
  `irreversiblePlyHistory_`, `gameInfoHistory_` and `capturedHistory_` with a single
  `std::array<PositionState, MAX_PLY> state_history_`, plus a live `state_` in place of `gameInfo_`.
- Narrow `halfmove_clock` and `fullmove_count` to `uint16_t` and `last_irreversible_ply` to
  `uint32_t`, and bound the three inputs that feed them — the two FEN counters and the UCI
  `position … moves` list — at the limits of the game itself, rejecting anything past them with a
  diagnostic. The increments get a Debug assert and no Release-time check (D4).
- Move the outcome out of `Board`: `ThreadData::root_game_state` and
  `PlayerAiBase::root_game_state_` replace `Board::SetGameState` and `GameInfo::gameState`, which are
  both deleted (D5). The `m_Board` write-back at `AIPerplex.cpp:242-244` goes with them.
- Reset the root verdict per `GetMove` call, which changes behaviour on one reachable path (D6).
- Delete `GameInfo`, including `UpdateBoardInfo`, `UpdateCastlingState`, `UpdateHalfmoveClock`,
  `Reset()`, `GameEnded()` and the defaulted constructor/destructor. `GameState.h` keeps `GameStates`,
  the `CastlingRights` flags and `HALFMOVE_CLOCK_LIMIT`.
- Delete `Board::GetGameInfo()` and migrate its 59 occurrences outside `Archived/` — 52 single-field
  reads, five whole-struct copies, the declaration and one stale comment — to narrow accessors,
  adding `fullmove_count()` alongside Stage 1's four (D7).
- Rewrite `FiftyMoveRuleTests.cpp`'s five `GameInfo`-level cases against `Board` (D7).
- Land a transitional commit that keeps both representations and asserts they agree after every make
  *and* unmake, deleted in the same PR (D8).

**This change will not:**

- Add the halfmove clock to the transposition-table identity — **#347**. It is a behaviour change and
  would cost this PR its equivalence gate, which is the only thing that makes a storage rewrite
  provable. This design fixes where the clock lives so #347 can be decided on its own terms next; that
  is the dependency #347 records.
- Take the record from 24 bytes to 16 — **#292**, re-scoped a second time (D3). This PR's bench is
  what tells #292 whether the last eight bytes are worth a second change.
- Merge `position_history_` (a game-length `std::vector<uint64_t>` for repetition detection) into the
  ply-indexed record. Different lifetime, different indexing, no shared consumer.
- Fix the quiet-promotion halfmove-clock bug — **#351**.
- Touch move generation, evaluation, move ordering, or any search heuristic. Nothing in this change
  may alter a node count.
- Decouple `AIPerplex` from `PlayerAiBase` or remove the `dynamic_cast`s — **#256**. Deleting
  `Board::SetGameState` narrows the surface; it does not close the gap.

## Decisions

### D1: One reversible record replaces four parallel arrays

`DoMove` and `DoNullMove` currently write `capturedHistory_[ply]`, `gameInfoHistory_[ply]`,
`irreversiblePlyHistory_[ply]` and `zobrist_history_[ply]` — four stores into four arrays whose
entries for a given ply are 2 KB, 5 KB and 256 bytes apart. `UndoMove` reads three of them and
`UndoNullMove` all four. They have identical lifetimes and identical indexing; nothing ever reads one
ply's entry from one array against another ply's from a second.

Merging them makes a ply's undo state one contiguous 24-byte object: one store on the way down, one
load on the way up, one cache line instead of four.

Rejected: merging only `gameInfoHistory_` and `capturedHistory_` and leaving the hash and
irreversible-ply arrays alone. It captures a fraction of the locality win and leaves two save/restore
conventions where the point is to have one.

The record mixes two tenses on purpose: every field except `captured_piece` is the state *before* the
move, while `captured_piece` describes the move itself. That is what the four arrays already do, and
separating them would need a fifth. It is worth one comment on the struct, because a reader who
assumes a uniform tense will index it wrong.

### D2: 24 bytes, with the layout fixed by the design rather than left to the compiler

```cpp
struct PositionState {              // bytes
    uint64_t zobrist_hash;          //  8   was zobrist_history_
    uint32_t last_irreversible_ply; //  4   was irreversiblePlyHistory_ (size_t)
    eSquare  ep_square;             //  4   int-sized; unchanged (see D3)
    Move     last_move;             //  2
    uint16_t halfmove_clock;        //  2
    uint16_t fullmove_count;        //  2
    uint8_t  castling_rights;       //  1
    ePiece   captured_piece;        //  1   uint8_t-backed
};                                  // 24 total, alignment 8
```

Undo storage per `Board` goes from 256 × (8 + 8 + 20 + 1) = 9,472 bytes to 256 × 24 = 6,144.

The claim the order buys is **no padding**, not a specific set of offsets — nothing reads this record
by offset, so offsets are a consequence rather than a requirement.
`static_assert(sizeof(PositionState) == 24)` is exactly the right guard for that claim: the member
sizes sum to 24, so equality means no byte was inserted anywhere. It is deliberately fragile in one
direction — if `eSquare` ever narrows (D3, #292) the assert fires, which forces that change to
re-choose the field order rather than silently leaving four bytes of padding behind.

`fullmove_count` stays in the reversible record rather than becoming a `Board` scalar. It is genuine
reversible per-move state — `DoMove` increments it, `UndoMove` must put it back — and here it occupies
bytes that would otherwise be padding. Making it a scalar would buy nothing and would add an explicit
decrement to all four make/unmake paths, with a symmetry bug available in each.

Rejected: leaving `halfmove_clock` and `fullmove_count` as `int` and `last_irreversible_ply` as
`size_t`, which needs no parser change and lands at 32 bytes. Still better than four arrays, and it is
the fallback if D4 is judged not worth it — but it spends eight bytes per ply to avoid a range check
in the FEN parser and two asserts.

### D3: The 16-byte variant is deferred, and #292's stated reason for caution is wrong

Getting to 16 bytes needs `eSquare : uint8_t`, `halfmove_clock` as `uint8_t` and
`last_irreversible_ply` as `uint16_t`. #292 warns that someone tried the `eSquare` change before and
reverted it for unrecorded reasons, and treats that as a risk to investigate first.

**That is not what happened.** The change was enabled during local testing and then parked pending
#292 itself; there was no failure and nothing to explain. #292's text should be corrected so the next
reader does not budget for a ghost.

It is still deferred, for a different reason: this PR's entire correctness argument is a bit-identical
`Compare-SearchEquivalence` run, and `eSquare`'s underlying type changes integer promotion at every
site that does arithmetic on a square. That is a second, independent change with its own failure mode,
and bundling it means a single differing `info` line no longer says which half caused it. Land the
structural merge, measure it, and let #292 decide whether the last eight bytes are worth a second pass
— with a real number instead of an estimate.

Rejected: doing both at once. Saves one PR; costs the ability to bisect the one gate that matters.

### D4: bound the inputs at the limits of the game; the increments carry a Debug assert only

`uint16_t` caps both counters at 65,535, and the question is what stops anything reaching it. The
answer is not a check on the increment — it is a bound on the three inputs that feed it, set where
they arrive and set at a value that means something in chess rather than something about `uint16_t`.

| Input | Bound | Where it comes from |
|---|---|---|
| FEN halfmove clock (field 5) | **150** | The 75-move rule's 150 halfmoves — this engine adjudicates at 100, and 150 leaves room to support the FIDE rule without revisiting the bound. Clears the corpus maximum of 120 |
| FEN fullmove counter (field 6) | **5899** | The theoretical longest possible chess game is 5898.5 moves¹ |
| UCI `position … moves` list | **11797 plies** | The same limit expressed in halfmoves: 2 × 5898.5 |

¹ <https://wismuth.com/chess/longest-game.html>

**Why reject rather than accept and cope:** these are the limits of chess, not of `uint16_t`. A
halfmove clock past 150, a fullmove counter past 5899 or a move list past 11797 plies does not
describe a game that can be played, so it is malformed input by definition — and the threat model's
goal for malformed input is a clear diagnostic, not a silently substituted value. Deriving the bounds
from the game also means they do not move when the storage does: the `uint16_t` fields in D2 are then
provably sufficient rather than coincidentally large enough, and #292's narrowing can be judged
against the same numbers.

The FEN half is a tightening of a rule the parser already has — it returns `"invalid halfmove clock"`
/ `"invalid fullmove counter"` when `std::stoi` throws past `INT_MAX`. The full numeric policy, which
is **not** what an earlier draft of this document claimed:

| FEN counter value | Today | After |
|---|---|---|
| negative | **rejected** — `FENParser.cpp:75`'s regex is `\s+\d+`, so a sign never reaches `std::stoi`; the message is `"overall format invalid"` | unchanged |
| `0` (halfmove) | accepted as 0 | unchanged |
| `0` (fullmove) | **accepted and repaired to 1** by `std::max(1, full)` — a FEN number is 1-based | unchanged |
| 1 … bound | accepted | unchanged |
| bound+1 … `INT_MAX` | accepted | **rejected, with the existing diagnostic** |
| above `INT_MAX` | rejected — `std::stoi` throws | unchanged |

Fullmove `0` is the one input the parser normalises rather than passing through, so it is the one case
where load and `ExtractFEN` legitimately disagree — the round-trip invariant is worded against the
parser's normalised domain for that reason, and the case gets its own test. `std::max(0, half)` is
dead given the regex; it can go with the rest of this or stay as belt-and-braces, but it should not be
read as evidence that negatives are clamped.

The UCI half reuses the move list's existing all-or-nothing semantics: an over-long list is refused
whole, with an `info string`, exactly as an illegal token already is.

This is the threat model's stated goal for external input — "a clear diagnostic and a clean exit" —
and `SetupFromFEN` already guarantees the board is left exactly as it was on a rejected FEN. It does
**not** belong under the *repair* contract: repairs exist for metadata inconsistent with the position
(an en-passant square with no pawn behind it, castling rights with no rook), where the position is
still legal and the fix is unambiguous. A counter of 900,000 is not a position problem, and silently
substituting a plausible number for it would change what `ExtractFEN` reports without telling anyone.

**What the three bounds buy together is the important part.** They close the last unbounded path into
the counters. A `position` command rebuilds the board from scratch — `SetupFromFEN` then replay — so
the worst case a caller can construct is the FEN bound plus a full-length move list:

| Counter | Worst reachable through UCI | Field maximum | Headroom |
|---|---|---|---|
| `fullmove_count` | 5899 + 11797/2 = 11,797 | 65,535 | 5.5× |
| `halfmove_clock` | 150 + 11,797 = 11,947 | 65,535 | 5.5× |

Game mode is bounded far tighter, by `Game::Run` adjudicating the fifty-move rule at 100. Nothing a
caller can send gets near the field maximum.

Which is what settles the increment.

**The assert fires at the field maximum, not at the input bound.** That distinction is the whole
point: the input bounds say what a caller may *send*, and the assert says what the engine may
*produce*. Crossing 5899 by playing moves is entirely legal — a position loaded at the bound and
played on must reach 5900 without complaint — whereas reaching 65,535 means the engine drove itself
somewhere no input could. Wiring the assert to the input bound would convert a validation rule into a
false runtime invariant, and it gets a test of its own for exactly that reason.

**An increment that reaches the field maximum is, by construction, an engine bug — so it gets a Debug
assert.** Not a saturating check. `Board::DoMove` already answers this exact question for the
analogous bound, ten lines above where the counters live:

```cpp
// Defense in depth: currentPly_ should never reach MAX_PLY now that
// ResetSearchDepth() decouples it from total game length (issue #53).
assert(currentPly_ < MAX_PLY);
```

The counters follow it, and for the same reason: with the inputs bounded, a counter at 65,535 means
the engine drove itself there, which is a defect to find rather than a condition to absorb.
Saturating would absorb it — and the threat model lists "a silently wrong answer" as a failure mode
alongside a crash, which is precisely what a saturated counter reporting a plausible number is. The
assert surfaces the runaway in Debug and costs nothing in Release, in a change whose entire
justification is per-node cost.

Rejected: saturating increments. They spend a compare per make on every node to convert a diagnosable
bug into an undiagnosable one. Rejected: bounding the inputs at the field maximum (65,535) rather than
at the game's limit — representable is not the same as possible, and a bound derived from `uint16_t`
would have to move every time the field does. Rejected: a wider field to sidestep the
question (D2's fallback) — still available if something here proves wrong, but it buys nothing the
bounds and the assert do not. Rejected: clamping instead of rejecting, which hides the input problem
rather than reporting it.

All three bounds are now at chess's own limits rather than at a comfortable multiple of observed
usage, so none of them can refuse a game that could actually be played. The halfmove bound is the only
one set by judgement rather than by the game: 150 supports the 75-move rule this engine does not yet
implement, against the 100 it adjudicates at today. If that support never arrives, 100 would do — but
150 costs nothing and removes a bound that would otherwise have to move.

`last_irreversible_ply` needs nothing at all: it is assigned from `position_history_.size()`, never
incremented, and `uint32_t` is four billion plies. Its narrowing cast is explicit, which `/WX`
requires anyway.

### D5: The outcome leaves `Board` entirely

`GameStates` is not position state. It is not part of the Zobrist identity, it is not reversible, and
it is written only under a `ply == 0` guard (`ThreadData::update_game_state`,
`PlayerAiBase::UpdateGameState`) — yet today it sits inside `gameInfo_` and is therefore saved and
restored at every ply by both make paths.

It becomes `ThreadData::root_game_state` and `PlayerAiBase::root_game_state_` — named for the
`SearchResult::game_state` field they feed. Not `root_state`: this design already has a
`PositionState` and the search already has a `SearchState`, and a third bare "state" beside them says
nothing about which one it is.

All six consumers are inside the player layer and were enumerated, not assumed:

| Site | Today | After |
|---|---|---|
| `AIPerplex.cpp:242-244` | reads `td_.board`'s state, writes it to `m_Board` | reads `td_.root_game_state`; the `m_Board` write-back is deleted |
| `AIPerplex.cpp:1143` (emergency path) | `td.board.GetGameInfo().gameState` | `td.root_game_state` |
| `PlayerAI.h:86` (`MakeResult`) | `m_Board.GetGameInfo().gameState` | `root_game_state_` |
| `PlayerAI.h:168` (`UpdateGameState`) | `m_Board.SetGameState` | `root_game_state_` |
| `PlayerAI.cpp:107`, `PlayerAiIterBase.h:51` | asserts on `m_Board`'s state | asserts on `root_game_state_` |
| `ThreadData.h:155` (`update_game_state`) | `board.SetGameState` | `root_game_state` |

Nothing outside a player reads it. `Game` has carried its own `game_state_` since Stage 1, fed from
`SearchResult::game_state`, and no test reads `Board`'s copy.

**Two carriers, not one, and that is forced rather than chosen.** AIPerplex's has to be per-thread:
`adjustScoreForGameState` runs on every Lazy SMP helper, so a single player-level member would be
written concurrently by every helper at its own ply 0 — the exact defect #358 records for `_bestScore`
one line away. Putting it on `ThreadData` is what keeps this change from adding a second instance of
that race. The legacy agents are single-threaded and have no `ThreadData`, so theirs sits on
`PlayerAiBase`. Two carriers means two independent reset paths, and therefore two tests (D6).

Rejected: keeping it on `Board` as a non-reversible scalar with a `game_state()` accessor. Smaller
diff, but it leaves `Board` owning something that is not a property of the position — and this issue
exists because `GameInfo` conflated exactly those two categories.

### D6: The root verdict is reset per `GetMove` call, and that is a deliberate fix

Today the search inherits its starting `gameState` from `td_.board = m_Board`, so a verdict written by
one call is visible to the next. `adjustScoreForGameState` writes a definite value on every completing
`pvs(ply == 0)` — `STILL_PLAYING`, `DRAW_PAT`, or a win — so on any search that finishes a root call
the inherited value is overwritten and invisible.

**It is not invisible if the search aborts before the first root call returns.** Then today's
`searched_state` is the *previous* call's verdict, and Stage 1 made `Game::Run` terminate on exactly
that field: a stale `WHITE_WON` ends the game on a move that decided nothing. A per-call member
initialised to `STILL_PLAYING` reports "no verdict", which is the truth.

This is the one place the change is not bit-identical, and it is stated rather than smuggled in. It
does not weaken the equivalence gate: `game_state` is never emitted over UCI, and every `position`
command rebuilds the board, so the stale path is unreachable under the protocol the gate drives.

**Both carriers need covering.** D5's two members are reset independently, so one test proves one of
them. `AIPerplex` (`ThreadData::root_game_state`) and one legacy agent — `AIAgent`, which has the
richest `UpdateGameState` usage of the three — each get the two-call test. Self-play cannot substitute:
it never issues a search that aborts before its first root call returns.

Keeping it in its own commit, separate from the storage merge, is what lets the equivalence run
before it stand as the proof for everything else.

Rejected: seeding the carrier from the previous call to preserve the behaviour exactly. It preserves a
bug whose blast radius Stage 1 increased. Rejected: centralising the reset so one mechanism covers
both — the two carriers live on different types for the thread-safety reason in D5, and a shared base
member would reintroduce exactly the race that separation avoids.

### D7: `GameInfo` is deleted, not kept as a facade

`GetGameInfo()` could synthesise a `GameInfo` from the split record and spare 59 call sites. That
keeps alive the type whose conflation of position state and outcome is the defect, and it keeps
`GetGameInfo()`'s by-value return — the silent write-through-a-temporary trap in **#355** — for no
gain beyond a smaller diff.

The migration is mechanical and the field breakdown is known: `castlingRights` ×15, `epSquare` ×15,
`halfmoveClock` ×11, `fullMoveCount` ×6, `gameState` ×5, five whole-struct copies, one declaration and
one comment. 49 of the 59 are in tests. Closes #355.

Two consequences are worth naming rather than discovering:

- **`FiftyMoveRuleTests.cpp` is rewritten, and gains coverage.** Its five `GameInfo`-level cases drive
  `UpdateBoardInfo`, which Stage 1 left with no engine callers at all — they currently assert against
  dead code. Redriving them through `Board::DoMove` points them at the path that actually runs.
- **#356 dissolves** rather than being fixed. Its two defects — the defaulted constructor/destructor
  suppressing move operations, and `fullMoveCount{0}` contradicting `Reset()`'s `1` — are properties
  of a struct that stops existing. `PositionState` is an aggregate with no user-declared special
  members, and defaults `fullmove_count{1}`, so the invariant #356 reports as false becomes true.

### D8: A transitional commit carries both representations and asserts they agree

The first commit adds `PositionState` and writes it **alongside** the four existing arrays, with a
Debug-only field-for-field comparison against them. The commit that deletes the arrays deletes the
comparison.

**The comparison runs forward as well as backward.** Checking only at `UndoMove`/`UndoNullMove` proves
the snapshot was saved and restored correctly and nothing else: a broken forward update of the
en-passant square, castling rights, either counter, `last_move`, the hash or the irreversible ply
would be written wrongly into *both* representations, restore perfectly, and pass. The oracle
therefore fires at four points:

- after `DoMove` completes, comparing the live record against the live `gameInfo_` and scalars;
- after `DoNullMove`, which has its own forfeit-the-ep-square and fullmove-increment logic;
- on `DoMove`'s rollback path, where a move that leaves its own king in check is undone before
  `change_player()` — the one place make and unmake interleave;
- after `UndoMove` / `UndoNullMove`, against the restored values.

That is what makes the transitional commit a direct oracle rather than a check on half the mechanism.

This is Stage 1's D7 pattern, which earned its keep there. `Compare-SearchEquivalence` is the stronger
gate but drives UCI only; the dual-write comparison also covers game mode and the legacy agents.

Rejected: an uncommitted local probe — same evidence, surviving only as prose in a PR body. Rejected:
relying on the equivalence run alone, which cannot distinguish "the record is correct" from "the
positions we searched never exercised the difference".

The assert must run against a **Debug** build; Release hides this class of thing.

### D9: `PositionState` is private to `Board`

Nothing outside `Board` needs the type: the accessors return fields, and the record is never passed,
returned or stored elsewhere. Declaring it private in `Board.h` makes that structural instead of
conventional — the original defect in #348 was precisely that a position-metadata struct *could* be
handed around and arrive stale.

Rejected: a public header. It would be needed only if something outside `Board` had to hold one, and
if that ever becomes true it is a change worth noticing.

## Assumptions I cannot verify from the code

- **The bench direction is unmeasured.** One contiguous 24-byte store replacing four scattered ones,
  and 3,328 fewer bytes copied per helper-thread `Board`, should be neutral-to-positive; Stage 1's
  comparable change measured +3.0%. Nothing in this change works against it: D4 adds no Release-time
  instruction to any make path. "Should" is still not a measurement. `Run-Bench.ps1` before and after,
  same compiler, matched thread counts; the number is reported, not predicted, and it is also the
  input #292 needs.
- **D6's stale-verdict path is argued, not observed.** That an abort can return before the first root
  `pvs` completes is a reading of `iterative_deepening`; no test reaches it today. It is settled by
  the new test named in Validation, which drives two `GetMove` calls and asserts the second does not
  inherit the first's verdict — that test is the verification, and it must fail against the current
  code or it is proving nothing.
- ~~No committed FEN exceeds D4's bounds.~~ **Checked, not assumed.** A scan of every `.cpp`, `.h`,
  `.epd`, `.fen`, `.pgn`, `.txt`, `.json` and `.md` in the tree for six-field FENs found a maximum
  halfmove clock of **120** and a maximum fullmove counter of **60**, both inside D4's bounds.
  Nothing in the corpora, the perft suite or the tactical suite is affected. The halfmove figure is
  the close one — 120 against a bound of 150, and above the 100 this engine adjudicates at — which is
  why the bound is 150 rather than 100.
- **The cited 5898.5-move maximum is taken from a source, not derived here.** It is the standard
  construction under the 50-move and threefold rules; this design uses it only as an upper bound, so
  being wrong in the *conservative* direction is harmless and being wrong the other way would need the
  true maximum to exceed 65,535 moves, which no analysis suggests. Not independently verified.

## Invariants

- `Compare-SearchEquivalence.ps1 -BaselineRef origin/main` is **identical** — every per-iteration
  `info` line and the `bestmove`, at fixed depth, `Threads=1`. This is the behaviour-preserving claim
  for everything except D6, which the protocol cannot reach.
- Node counts at fixed depth are unchanged. Nothing here may alter what the search visits.
- Perft counts are unchanged across the 142,953-position corpus.
- `UndoMove` and `UndoNullMove` restore the pre-move position exactly, including the Zobrist hash,
  the halfmove clock, the last-irreversible ply and `last_move` — the property D8's assert checks
  directly and the existing `[board_state]` / `[board_api]` cases check behaviourally.
- `sizeof(PositionState) == 24`, asserted at compile time — the no-padding claim, not an offset claim.
- `ExtractFEN` round-trips unchanged every counter value in the parser's **normalised** domain —
  halfmove 0…150, fullmove 1…5899. Fullmove `0` is outside it by construction: the parser repairs it
  to 1 on load, so it round-trips as 1, and that is correct rather than a violation.
- Every input that can raise a counter is bounded at chess's own limit, and an input past it is
  rejected with a diagnostic leaving the board exactly as it was (D4) — no value is silently
  substituted, and no input path can drive a counter to its field maximum.
- **The input bound is not a runtime invariant.** A position loaded at the fullmove bound and played
  on reaches 5900 and beyond without asserting; the Debug assert guards the field maximum only (D4).
- No Release-time cost is added to any make path. D4's runtime guard is a Debug assert.
- The root verdict reaches a caller through `SearchResult::game_state` and no other channel; no object
  outside a player holds a `GameStates` written by the search.
- An aborted frame still mutates nothing — this change adds no store to the move loop.

## Validation

Engine tier, so `Validate-PrePR.ps1` runs the full local gate. Beyond it:

- **`Compare-SearchEquivalence.ps1 -After <exe> -BaselineRef origin/main`** — identical. Re-run after
  each risky step, not only at the end; Stage 1's experience was that this is what localises a
  regression to one commit.
- **`Run-PerftCheck.ps1`** — make/unmake state handling changes, which is exactly what perft
  exercises. A clean sweep is the script's classification, not zero failures.
- **`Run-Bench.ps1`** before and after, clang-cl both sides, matched thread counts. Reported as
  measured. This number is also #292's input.
- **Debug-build test run with D8's dual-write oracle in place**, before the deletion commit. The
  suite has to reach all four comparison points, including `DoMove`'s rollback path — `[board_state]`
  already covers a move rejected for leaving the king in check.
- **Two `[search]` tests for D6**, one per carrier: `AIPerplex` and `AIAgent`. Two `GetMove` calls on
  one player where the first reaches a terminal verdict and the second is aborted immediately; the
  second must report `STILL_PLAYING`. Falsify both against current `main` — if either passes there,
  it is testing nothing.
- **The rewritten `[fifty_move]` cases**, driving `Board::DoMove` rather than the deleted
  `GameInfo::UpdateBoardInfo`.
- **D4 bound tests**, one per input: a FEN above the halfmove bound and one above the fullmove bound
  are each rejected with a diagnostic naming the counter and leave the board unchanged — the contract
  `SetupFromFEN` already documents; a FEN at exactly each bound loads and `ExtractFEN`s back
  identically; and a `position … moves` list past the ply bound is refused whole with an
  `info string`, leaving the previous position in place, matching the illegal-token path beside it.
- **A D4 "the cap is not a runtime invariant" test, run in Debug**: load a FEN at the fullmove bound
  (5899) with Black to move, make a quiet move, confirm the counter reaches 5900 without tripping the
  assert, then undo and confirm 5899. This is the test that fails if the assert is ever wired to the
  input bound instead of the field maximum — which is the easy mistake here, and a silent one in
  Release.
- **A fullmove-`0` normalisation test**: a FEN naming fullmove `0` loads as 1 and `ExtractFEN`s as 1.
  Pins the one input the parser deliberately repairs, and the one exception in the round-trip
  invariant.
- **Self-play twice**: AIPerplex vs AIPerplex (`"type": 6`) and AIAgent vs AIAgent (`"type": 3`),
  because `PlayerAiBase` changes.
- **`search-reviewer` dispatch** — the diff touches `AIPerplex.cpp` and `ThreadData.h`.

**No Elo match.** The search is behaviour-identical at `Threads=1` and the equivalence check proves it
directly; an Elo match cannot resolve a zero-Elo change and would only add noise. If the bench moves
by more than ~5% in either direction that is worth reporting, and the measurement budget is the user's
call — it is not a precondition for merging.

Closes #355 and #356. Re-scopes #292 to a single measured question.

## Harvest

| Decision / rationale | Lands in |
|---|---|
| `Board` owns one reversible `PositionState` per ply; the outcome is not board state | `CLAUDE.md` → Key Source Facts (extends Stage 1's entry), comment on `PositionState` |
| The record is pre-move state plus the move's own `captured_piece` — two tenses, deliberately | comment on `PositionState`; a reader who assumes one tense indexes it wrong |
| `sizeof(PositionState) == 24` and why the field order is not arbitrary | `static_assert` plus a one-line comment — the assert is the durable form |
| The root verdict travels only via `SearchResult::game_state`; no player writes it to a `Board` | `CLAUDE.md` → Key Source Facts. **`CLAUDE.md:153` currently states the opposite** — "root state is propagated back in `GetMove()`" — and must be corrected, not merely extended |
| Why the verdict lives on `ThreadData`, not `PlayerAiBase`, for AIPerplex | comment on `ThreadData::root_game_state` — it is a thread-safety constraint, and the obvious simplification reintroduces #358's race |
| D6's per-call reset, and the stale-verdict bug it fixes | `Docs/Changelog.md`, the PR body, and the test names |
| D4's three input bounds, as named constants with their chess justification and the source URL — not `uint16_t` maxima | the constants themselves, sited with the parser and the UCI move-list handler; the numbers are meaningless without the reason, and 5899 is unguessable without the citation |
| The input bound is a validation rule, not a runtime invariant — the assert guards the field maximum | comment at the assert, and the test name; wiring the two together is the easy mistake |
| The parser's real numeric policy: negatives die at the regex, fullmove `0` is repaired to 1 | comment at the counter parse — an earlier draft of this design got both wrong from reading `std::max` alone |
| D4: an out-of-range input is rejected, not repaired — and why the increments get a Debug assert rather than a Release check | comment at the rejection, next to `SetupFromFEN`'s repair note (which it is deliberately *not* an instance of), and at the assert |
| #292's "reverted for unrecorded reasons" is false — it was validated locally and parked | comment on #292, with the measured 24-byte bench as its new input |
| Measured bench before/after | `Docs/Changelog.md`, and the PR body |
| Where the halfmove clock now lives, for #347 to build on | comment on #347 |
| The rewritten fifty-move cases, the D4 boundary test and the two D6 stale-verdict tests | `Docs/TestDesign.md` — and its row 78, "Board GameInfo state lifecycle", is renamed: the type it names stops existing |
| `Move.h:16` and `MoveFactory.h:8` both point at `Board::capturedHistory_[]` as where the captured piece is tracked | both comments, repointed at `PositionState::captured_piece` — they are the only documentation of that contract for a `Move` reader |

**Approved decisions that changed during implementation.** *(Fill in before opening the PR.)*
