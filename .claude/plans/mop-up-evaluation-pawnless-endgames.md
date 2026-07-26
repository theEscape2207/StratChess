# Mop-Up Evaluation for Won Pawnless Endgames — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix issue #70 (Tier 0 of epic #110) — in decisively-won, pawnless endings (e.g. KQ vs KR),
`EvalComplex::Evaluate` currently plateaus at bare material and gives the search no gradient toward
driving the losing king to the edge and mating. Add a standalone mop-up evaluation term that rewards
(a) pushing the losing king toward the corner/edge and (b) closing the distance between the two kings,
gated so it only fires in pawnless endings with a decisive material lead.

**Architecture:** One self-contained addition to `EvalComplex::Evaluate` (`StratEngine/Eval.cpp`),
computed once after the existing per-piece loop, using only data already available in that function
(`matScoreWhite`/`matScoreBlack`, `white_pawns`/`black_pawns`, `gameStage`, and the king bitboards via
the existing `boards` span). Two new small `constexpr` distance helpers live alongside the existing
`getEvalBoard` helper in `EvalComplex`. No new files, no new dependencies, no change to `EvalManager`'s
statelessness contract.

**Tech Stack:** C++20, existing bitboard/mailbox `Board` API, Catch2 v3 for tests.

## Global Constraints

- C++20; Level4 + `/WX` enforced on both Debug and Release, `x64` only — any new compiler warning is a
  build error. No `#pragma warning(disable)`. Use `static_cast<>` for any intentional narrowing.
- `EvalManager`/`EvalComplex` must remain stateless (Lazy SMP sharing contract documented at the top of
  `Eval.h`) — new constants must be compile-time constants (`static const`/`constexpr`), and
  `EvalComplex::Evaluate` must stay `noexcept`, reading only its `const Board&` argument and read-only
  global tables.
- Naming and comments in English, unambiguous.
- No regressions in search accuracy or ELO without explicit justification — validate with
  `Scripts\Run-EloMatch.ps1` before merging (500 games ≈ ±25 Elo per `Docs/EloLog.md`).
- Any change to `Eval.cpp` requires dispatching the `eval-reviewer` subagent before opening a PR
  (per `CLAUDE.md`'s pre-PR checklist).
- Test philosophy in this repo is **direction over precision** (`Docs/TestDesign.md`): verify relative
  score ordering/magnitude, never hardcode exact centipawn values.
- FEN test-construction rules (`CLAUDE.md`): always include the side-to-move field; verify no White
  piece attacks the Black King before adding a position (bug #45/#46 — the engine silently accepts
  illegal FENs).

## Design Decisions

- **Gating conditions** (all three required): `gameStage != PlayState::MIDDLEGAME` (cheap extra safety
  belt, reuses the phase check already computed in `Evaluate`), `white_pawns == 0ULL && black_pawns ==
  0ULL` (mop-up is a *pawnless*-endgame technique per issue #70 — with pawns on the board, promotion
  races and pawn play should dominate, not king-hunting), and `abs(matScoreWhite - matScoreBlack) >=
  MOPUP_MATERIAL_THRESHOLD` (only apply once one side has a materially decisive lead — e.g. Q vs R,
  R vs nothing — not in roughly-balanced pawnless endings like opposite-colored bishops or R vs R,
  where centralizing toward the enemy king would be wrong).
- **`MOPUP_MATERIAL_THRESHOLD = 400`** — matches the Q-vs-R material gap (900−500=400) from the
  QFORK-001 position cited in issue #70, while excluding smaller/roughly-equal imbalances.
- **Formula**: `bonus = MOPUP_CMD_WEIGHT * CenterManhattanDistance(loserKing) + MOPUP_KINGDIST_WEIGHT *
  (MOPUP_MAX_KING_DISTANCE - KingDistance(winnerKing, loserKing))`, added to `bonusScore[winner]`.
  `CenterManhattanDistance` ranges 0 (center) to 6 (corner) — rewards the losing king being pushed to
  the edge. `KingDistance` is the Chebyshev (king-move) distance between the two kings, 0–7 — the
  `(7 - distance)` term rewards the winning king approaching. Weights (`CMD=10`, `KINGDIST=4`) are a
  reasoned starting point sized to be comparable to the existing bonus constants
  (`DOUBLED_PAWN_PENALTY=10`, `ROOK_ON_7TH_BONUS=20`, etc.) — not a precisely sourced constant, and a
  natural first candidate for issue #117 (automated Texel-style tuning) once that lands.
- **Why this differs from the existing King endgame PST**: `g_Eval_Bitboards[6]` (the endgame King PST)
  already rewards *each* king independently for centralizing — but it has no notion of the *other*
  king's position, so it can't specifically encode "chase the enemy king to the corner." Mop-up models
  the interaction between both kings, which is the qualitatively new signal issue #70 is missing.
- **Distance helpers use plain grid math**, not `std::abs`/`std::min`/`std::max` on `eSquare` directly,
  to avoid any signed/unsigned or narrowing-conversion warnings under `/WX` — mirrors the existing
  `int rank = Rank(square); int file = File(square);` pattern already used in `Evaluate`.

## Files Changed

- Modify: `StratEngine/Eval.h` — add 4 mop-up constants + 3 `constexpr` static helper functions to
  `EvalComplex`.
- Modify: `StratEngine/Eval.cpp` — add the gated mop-up scoring block to `EvalComplex::Evaluate`.
- Modify: `StratChessTests/EvalTests.cpp` — add 6 FEN constants + 3 new `TEST_CASE`s tagged `[eval]`.
- Modify: `Docs/Changelog.md` — one-line entry once validated (Task 2).

## Step-by-Step Changes

### Task 1: Implement the mop-up evaluation term (TDD)

**Files:**
- Modify: `StratEngine/Eval.h`
- Modify: `StratEngine/Eval.cpp`
- Test: `StratChessTests/EvalTests.cpp`

**Interfaces:**
- Consumes: `EvalComplex::Evaluate(const Board&)` (existing, unchanged signature), `Board::GetBitBoards()`,
  `Board::GetFirstPiece(BITBOARD)`, `Board::GetMaterialScore(eColor)` — all existing, unchanged.
- Produces: `EvalComplex::CenterManhattanDistance(eSquare)`, `EvalComplex::KingDistance(eSquare, eSquare)`
  — private `static constexpr int`, not consumed outside this task/file.

- [ ] **Step 1: Write the failing tests**

Add to `StratChessTests/EvalTests.cpp`, after the existing `FEN_ROOK_ON_7TH` constant:

```cpp
// Mop-up evaluation (issue #70 / epic #110): White King+Queen vs Black King+Rook,
// pawnless, decisive material lead (900 - 500 = 400 cp). Black king cornered (a8)
// vs centered (c6) — everything else identical. White to move.
static constexpr const char* FEN_MOPUP_LOSER_KING_CORNER =
    "k6r/8/8/8/3Q4/8/8/4K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_LOSER_KING_CENTER =
    "r7/8/2k5/8/3Q4/8/8/4K3 w - - 0 1";

// Same as above, but with one pawn each (Pa2/pa7) — mop-up must be gated off
// once pawns are on the board.
static constexpr const char* FEN_MOPUP_LOSER_KING_CORNER_WITH_PAWNS =
    "k6r/p7/8/8/3Q4/8/P7/4K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_LOSER_KING_CENTER_WITH_PAWNS =
    "r7/p7/2k5/8/3Q4/8/P7/4K3 w - - 0 1";

// White King+Knight vs Black King+Bishop, pawnless, materially EQUAL (300 - 300 = 0).
// Same corner/center king placement idea — mop-up must be gated off below the
// decisive-material threshold.
static constexpr const char* FEN_MOPUP_MARGINAL_CORNER =
    "7k/8/8/8/5N2/8/8/b3K3 w - - 0 1";
static constexpr const char* FEN_MOPUP_MARGINAL_CENTER =
    "8/8/2k5/8/5N2/8/8/b3K3 w - - 0 1";
```

Add these three `TEST_CASE`s at the end of the file:

```cpp
TEST_CASE("Eval - EvalComplex mop-up: decisively-won pawnless ending scores higher with the losing king cornered", "[eval]")
{
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board cornerBoard(FEN_MOPUP_LOSER_KING_CORNER);
    Board centerBoard(FEN_MOPUP_LOSER_KING_CENTER);

    int cornerScore = eval->Evaluate(cornerBoard);
    int centerScore = eval->Evaluate(centerBoard);

    // Same material both sides (Q vs R, 400 cp lead) — the only difference is
    // how cornered the losing (black) king is. Mop-up must prefer the corner.
    REQUIRE(cornerScore > centerScore);
}

TEST_CASE("Eval - EvalComplex mop-up: gated off once pawns are on the board", "[eval]")
{
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board pawnlessCorner(FEN_MOPUP_LOSER_KING_CORNER);
    Board pawnlessCenter(FEN_MOPUP_LOSER_KING_CENTER);
    int pawnlessDelta = eval->Evaluate(pawnlessCorner) - eval->Evaluate(pawnlessCenter);

    Board pawnsCorner(FEN_MOPUP_LOSER_KING_CORNER_WITH_PAWNS);
    Board pawnsCenter(FEN_MOPUP_LOSER_KING_CENTER_WITH_PAWNS);
    int withPawnsDelta = eval->Evaluate(pawnsCorner) - eval->Evaluate(pawnsCenter);

    // Both variants have the identical king-placement swing available to them;
    // only the pawnless one should get the (larger) mop-up contribution on top.
    REQUIRE(pawnlessDelta > withPawnsDelta);
}

TEST_CASE("Eval - EvalComplex mop-up: gated off below the decisive material threshold", "[eval]")
{
    auto eval = EvalManager::Create(EvalManager::EvalTypes::COMPLEX);

    Board decisiveCorner(FEN_MOPUP_LOSER_KING_CORNER);
    Board decisiveCenter(FEN_MOPUP_LOSER_KING_CENTER);
    int decisiveDelta = eval->Evaluate(decisiveCorner) - eval->Evaluate(decisiveCenter);

    Board marginalCorner(FEN_MOPUP_MARGINAL_CORNER);
    Board marginalCenter(FEN_MOPUP_MARGINAL_CENTER);
    int marginalDelta = eval->Evaluate(marginalCorner) - eval->Evaluate(marginalCenter);

    // The 400 cp Q-vs-R lead should swing far more from cornering than the
    // materially-equal N-vs-B case, which gets no mop-up bonus at all.
    REQUIRE(decisiveDelta > marginalDelta);
}
```

- [ ] **Step 2: Build tests and run to verify the new cases fail**

```
.\build.ps1 tests
StratChessTests\x64\Release\StratChessTests.exe [eval]
```

Expected: the two gating tests may pass by coincidence (both sides currently get 0 mop-up
contribution, so the deltas could tie or go either way), but the first test
("...scores higher with the losing king cornered") is expected to **FAIL or pass only by the
small existing King-PST margin** — confirm by temporarily checking the actual printed score
values (Catch2 `-s`) match bare-PST expectations, not a clear mop-up-sized swing. This step's
real purpose is to confirm the harness builds and runs before writing the fix.

- [ ] **Step 3: Add the mop-up constants and distance helpers**

In `StratEngine/Eval.h`, inside `class EvalComplex final`, after the existing bonus constants:

```cpp
	static const short DOUBLED_PAWN_PENALTY		= 10;
	static const short ISOLATED_PAWN_PENALTY	= 20;
	static const short BACKWARDS_PAWN_PENALTY	= 5;
	static const short PASSED_PAWN_BONUS		= 20;
	static const short ROOK_ON_7TH_BONUS		= 20;
	static const short HALF_OPEN_FILE			= 10;
	static const short OPEN_FILE				= 15;

	// Mop-up evaluation (won pawnless endgames) — see issue #70 / epic #110.
	// Gated on: pawnless + decisive material lead. Rewards pushing the losing
	// king to the edge/corner and closing the distance between the two kings.
	static const short MOPUP_MATERIAL_THRESHOLD	= 400;	// min material lead (cp) before mop-up applies
	static const short MOPUP_CMD_WEIGHT		= 10;	// weight on losing king's center-manhattan-distance
	static const short MOPUP_KINGDIST_WEIGHT	= 4;	// weight on (MOPUP_MAX_KING_DISTANCE - king-to-king distance)
	static const short MOPUP_MAX_KING_DISTANCE	= 7;	// max Chebyshev distance on an 8x8 board

	// Distance helpers for mop-up scoring — plain grid math, orientation-independent
	// (works the same whether the square belongs to White or Black).
	static constexpr int CenterAxisDistance(int coord) noexcept
	{
		return (coord <= 3) ? (3 - coord) : (coord - 4);
	}
	static constexpr int CenterManhattanDistance(eSquare square) noexcept
	{
		return CenterAxisDistance(File(square)) + CenterAxisDistance(Rank(square));
	}
	static constexpr int AbsDiff(int a, int b) noexcept
	{
		return (a > b) ? (a - b) : (b - a);
	}
	static constexpr int KingDistance(eSquare a, eSquare b) noexcept
	{
		const int fileDiff = AbsDiff(File(a), File(b));
		const int rankDiff = AbsDiff(Rank(a), Rank(b));
		return (fileDiff > rankDiff) ? fileDiff : rankDiff;
	}
```

- [ ] **Step 4: Add the mop-up scoring block to `EvalComplex::Evaluate`**

In `StratEngine/Eval.cpp`, in `EvalComplex::Evaluate`, insert this block right after the main
per-piece `while (remaining)` loop ends, and before `const eColor color = board.GetCurrentColor();`:

```cpp
	// Mop-up evaluation: in decisively-won, pawnless endings, reward driving the
	// losing king to the edge/corner and closing the distance between the two
	// kings — the win may lie beyond the search horizon otherwise (issue #70).
	if (gameStage != PlayState::MIDDLEGAME &&
		white_pawns == 0ULL && black_pawns == 0ULL)
	{
		const int matDiff = matScoreWhite - matScoreBlack;
		const int absMatDiff = (matDiff >= 0) ? matDiff : -matDiff;

		if (absMatDiff >= MOPUP_MATERIAL_THRESHOLD)
		{
			const eColor winner = (matDiff > 0) ? WHITE : BLACK;
			const eColor loser = (winner == WHITE) ? BLACK : WHITE;

			const eSquare winnerKingSq = Board::GetFirstPiece(
				boards[(winner == WHITE) ? ePiece::WHITE_KING : ePiece::BLACK_KING]);
			const eSquare loserKingSq = Board::GetFirstPiece(
				boards[(loser == WHITE) ? ePiece::WHITE_KING : ePiece::BLACK_KING]);

			bonusScore[winner] += MOPUP_CMD_WEIGHT * CenterManhattanDistance(loserKingSq) +
				MOPUP_KINGDIST_WEIGHT * (MOPUP_MAX_KING_DISTANCE - KingDistance(winnerKingSq, loserKingSq));
		}
	}

```

- [ ] **Step 5: Rebuild and run the eval tests to verify they pass**

```
.\build.ps1 tests
StratChessTests\x64\Release\StratChessTests.exe [eval]
```

Expected: `All tests passed` including the 3 new mop-up `TEST_CASE`s.

- [ ] **Step 6: Commit**

```bash
git add StratEngine/Eval.h StratEngine/Eval.cpp StratChessTests/EvalTests.cpp
git commit -m "Add mop-up evaluation for won pawnless endgames (#70)"
```

### Task 2: Validate and document

**Files:**
- Modify: `Docs/Changelog.md`

**Interfaces:**
- Consumes: the completed Task 1 build/binaries.
- Produces: nothing new — this task is validation + documentation only.

- [ ] **Step 1: Full build + extended test tier**

```
.\build.ps1 all
StratChessTests\x64\Release\StratChessTests.exe
```

Expected: full solution builds clean (Level4/`/WX`, no warnings), all existing tests still pass
(no regressions in doubled-pawn, rook-on-7th, or other existing `[eval]` cases).

- [ ] **Step 2: Self-play sanity check**

Run an AIPerplex-vs-AIPerplex self-play game (`game_settings.json` `"type": 6` both sides) from
`StratChessEvolved/`, headless, and confirm it terminates normally (no crash, no illegal-move
stall). This is a quick sanity check, not a strength measurement.

- [ ] **Step 3: Dispatch the eval-reviewer subagent**

Per `CLAUDE.md`'s pre-PR checklist, dispatch the `eval-reviewer` subagent against the diff in
`StratEngine/Eval.cpp`/`Eval.h` before opening the PR. Address any findings.

- [ ] **Step 4: Measure ELO impact**

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-EloMatch.ps1"
```

500 games ≈ ±25 Elo (per `Docs/EloLog.md`). This change is expected to matter almost entirely in
pawnless decisive endings, which are rare in a 500-game batch from a standard opening book — a
result within ±error of 0 is plausible and does **not** mean the fix failed; the tactical-level
fix (no-progress plateau in KQ vs KR, per issue #70) is the primary signal, not the batch Elo
delta. Record the result as a new row in `Docs/EloLog.md` regardless of outcome.

- [ ] **Step 5: Update the changelog**

Add a one-line dated entry to `Docs/Changelog.md` (see `feedback_roadmap_conciseness` convention —
concise, no PR-time rationale dump): mop-up evaluation added for pawnless won endgames, closes #70.

- [ ] **Step 6: Commit**

```bash
git add Docs/Changelog.md Docs/EloLog.md
git commit -m "Document mop-up evaluation ELO measurement and changelog entry"
```

- [ ] **Step 7: Open the PR**

Follow the repo's pre-PR checklist (sync with `origin/main`, this is a code change so
`Validate-PrePR.ps1` applies, not the doc-only fast path). PR body: "Closes #70." and a note that
this is Tier 0 of epic #110.

## Validation Plan

- Build: `.\build.ps1 all` (full solution, Level4/`/WX` clean).
- Tests: `StratChessTests\x64\Release\StratChessTests.exe [eval]` during TDD; full suite
  (`StratChessTests\x64\Release\StratChessTests.exe`, no tag filter) before commit/PR.
- Self-play: one headless AIPerplex-vs-AIPerplex game, confirm normal termination.
- ELO: `Scripts\Run-EloMatch.ps1`, 500-game batch, recorded in `Docs/EloLog.md`.
- Review: `eval-reviewer` subagent dispatched on the `Eval.cpp`/`Eval.h` diff before PR.

## Key Correctness Properties

- Mop-up must contribute **zero** score change whenever either side has any pawn on the board.
- Mop-up must contribute **zero** score change whenever the material lead is below
  `MOPUP_MATERIAL_THRESHOLD` (400 cp).
- Given two positions identical except for the losing king's square, the position where that king
  is closer to a corner (higher `CenterManhattanDistance`) must score at least as well for the
  winning side, all else equal.
- `EvalComplex::Evaluate` must remain `noexcept` and stateless — no new mutable members, no
  runtime-computed statics (only `static const`/`constexpr`).
- No existing `[eval]` test (doubled pawn, isolated pawn, rook-on-7th, starting-position symmetry,
  material-advantage direction) may regress.
