# Reject FENs whose waiting side is in check (issue #45)

## Goal

A FEN describing a position where the side **not** to move has its king attacked is illegal — it
cannot arise from a legal game, because the mover would have had to leave their king en prise. The
engine currently loads such a position without complaint and returns a king capture as its best move
(`position fen 4k3/8/8/8/8/5b2/8/4RK2 w - - 0 1` → `bestmove e1e8`). Reject it at load, reporting
through the channel #155 built.

## Scope limits

- **One rule only**: the waiting side's king must not be attacked. Not a general position-legality
  validator (no reachability analysis, no "could this arise from the starting position").
- **No new UCI policy.** #155 already decided what a rejected `position` does: decline the command,
  keep the board, log at debug. Nothing to add.
- The issue's second suggestion — accept the position but answer `bestmove 0000` — is **not** taken.
  Declining the load is strictly simpler, and it makes the illegal position unrepresentable rather
  than something every consumer of the board has to remember to special-case.

## Design decisions

### 1. `Board::WaitingSideInCheck()`, mirroring `InCheck()`

`InCheck()` (`Board.cpp:595`) tests whether **`sideToMove_`'s** king is attacked, which for a
freshly loaded FEN is the legal case — the side to move being in check is just a check position. The
illegal case is its mirror, so it needs its own query:

```cpp
bool Board::WaitingSideInCheck() const noexcept
{
    const eColor waiting = (sideToMove_ == WHITE) ? BLACK : WHITE;
    const BITBOARD bb = MoveGenerator::GetAttackBoard(*this, sideToMove_);
    return Bits::isAnyBitSet(bb, bitboards_.at(static_cast<BITBOARD>(KING) + waiting));
}
```

Public and documented, next to `InCheck()`. The two are one character apart in meaning and easy to
confuse, so each comment says which side it tests.

**Adjacent kings are subsumed.** `GetAttackBoard` includes king attacks (`g_bbKingMoves`,
`MoveGenerator.cpp:525`), so a position with the kings on adjacent squares makes the waiting king
attacked and is rejected by the same rule. No separate check needed — verified by reading the
generator, and pinned by a test.

### 2. The check runs on a scratch board, before `*this` is touched

`SetupFromFEN`'s contract (#155) is that a rejected FEN leaves the board **untouched**. Legality
needs attack generation, which needs a *populated* board — so validating after applying the position
would mean mutating the board and then discovering it must be rejected.

So a scratch `Board` is populated from the parsed piece list and probed before `*this` is modified:

```cpp
Board probe;
probe.setup_board(pieces);
probe.sideToMove_ = state.sideToMove;
if (probe.WaitingSideInCheck()) { /* log, return false */ }
```

`Board` is a copyable value type (#67) and search already keeps a copy per thread, so this is
ordinary use. `setup_board` runs twice on a successful load; that is deliberate and cheap (32
`add_piece` calls), and it buys the atomicity guarantee. The alternative — build a full candidate and
commit it by assignment — was rejected as a larger diff to a load-bearing function for no gain here.

Only piece placement and side to move are needed for the probe: `GetAttackBoard` reads bitboards and
(for the ep target) `gameInfo_`, and none of the ep/castling metadata can affect whether a king is
attacked.

### 3. Legality lives in `Board`, not `FENParser`

`FENParser::ParseFEN` produces a piece list and has no board, so it cannot do attack detection
without reimplementing it. Consequence worth naming: `FenBatch::ClassifyLine` (which calls `ParseFEN`
directly) therefore reports an illegal-but-parseable FEN as `Valid`, and the batch eval runner's
`!SetupFromFEN` branch — dead when written in #162, since `ClassifyLine` had already run the same
parser — **becomes reachable and is now the thing that keeps illegal positions out of a tuning
corpus**. Its comment must stop saying the two parsers disagreeing is the only way to get there.

## Files changed

| File | Change |
|---|---|
| `StratEngine/Board.h` | declare `WaitingSideInCheck()`; sharpen `InCheck()`'s comment |
| `StratEngine/Board.cpp` | implement it; probe block in `SetupFromFEN` |
| `StratChessEvolved/StratChessEvolved.cpp` | batch-eval skip comment: now a real legality gate |
| `StratEngine/Utils/FenBatch.h` | note that `ClassifyLine` validates syntax, not legality |
| `StratChessTests/UCITests.cpp` | new `[uci]` cases (see below) |
| `Docs/TestDesign.md` | the "engine accepts illegal FENs silently" note is now false |
| `Docs/Changelog.md` | entry |

## Step-by-step

1. `WaitingSideInCheck()` + comments.
2. Probe block in `SetupFromFEN`.
3. Tests.
4. Comment corrections in `FenBatch.h` / `evalrunner`.
5. Build Release **and** Debug.
6. **Sweep the existing FEN corpus** (below) — this is the step that decides whether the rule can
   land as-is.
7. Docs.

## The sweep, and why it is the real risk

A new rejection rule can reject FENs already used by the suite. #162 makes this cheap to detect
rather than a silent behaviour change:

- Every `SetupFromFEN` test call site is wrapped in `REQUIRE(...)`, so an illegal FEN fails loudly.
- The ~145 `Board board(FEN)` constructor sites assert — **Debug only**, so the sweep must run the
  Debug binary.

So: run the **Debug extended tier** plus the tactical suites. Any failure is unambiguous ("this FEN
has the waiting side in check"), and each one is then a genuine pre-existing defect in a test
position, to be fixed in this PR — `Docs/TestDesign.md` already tells authors to verify this by hand
precisely because the engine could not (bug #45).

Perft positions are standard published ones and expected clean; the tactical suites (38 FENs) were
authored under the hand-verification rule and are expected clean, but expected is not verified.

## Validation plan

`Engine` tier (changes UCI behaviour on malformed input). New `[uci]` cases:

- The issue's exact repro is rejected, and the board keeps its previous position.
- **Control**: the same position with `b` to move — the side to move being in check — still loads.
  This is the test that proves the rule tests the right side.
- Adjacent kings (`8/8/8/3kK3/8/8/8/8 w - - 0 1`) is rejected.
- A legal check position loads and `InCheck()` is true, so no legal position regressed.
- `position fen <illegal>` is declined by `cmd_position`, board unchanged.

No Elo match: no legal position's evaluation or search changes.

## Invariants that must hold afterwards

- No position with the waiting side in check can be loaded through `SetupFromFEN`.
- A position with the *side to move* in check still loads — the common case in tactical tests.
- A rejected FEN mutates nothing, illegality included.
- Perft node counts unchanged (all suite positions legal).
