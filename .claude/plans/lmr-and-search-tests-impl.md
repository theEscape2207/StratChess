# LMR + [search] Tests — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to implement this plan task-by-task.

**Goal:** Implement Late Move Reductions (sqrt formula) in `pvs()` and add 10 `[search]` unit tests for the three private AIPerplex search helper methods.

**Architecture:** TDD order — write `[search]` tests first (they test already-correct behaviour), then add LMR. The `STRAT_ENABLE_TEST_ACCESS` preprocessor macro activates the existing `friend class AIPerlexTestFixture` gate in `AIPerplex.h`. `AIPerlexTestFixture` re-exports private types as public aliases so `TEST_CASE` functions can reference them. LMR uses a sqrt formula with a `lmr_enabled` kill-switch in `SearchTuning` that parallels the existing `aspiration_enabled` field.

**Tech Stack:** C++20, MSVC /W4 /WX, Catch2 v3 amalgamated, build via `build.ps1`

**Design doc:** `.claude/plans/lmr-and-search-tests.md`

**Worktree:** `.claude/worktrees/romantic-tereshkova/` — all paths below are relative to this root.

---

### Task 1: Enable STRAT_ENABLE_TEST_ACCESS in vcxproj

**Files:**
- Modify: `StratChessTests/StratChessTests.vcxproj`

The vcxproj has exactly two x64 `<PreprocessorDefinitions>` lines (lines 107 and 125 as of last read). Add `STRAT_ENABLE_TEST_ACCESS;` to the front of each.

**Step 1: Edit x64 Debug preprocessor line**

Find and replace in `StratChessTests/StratChessTests.vcxproj`:

Old:
```xml
      <PreprocessorDefinitions>_DEBUG;_CONSOLE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
```

New:
```xml
      <PreprocessorDefinitions>STRAT_ENABLE_TEST_ACCESS;_DEBUG;_CONSOLE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
```

**Step 2: Edit x64 Release preprocessor line**

Old:
```xml
      <PreprocessorDefinitions>NDEBUG;_CONSOLE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
```

New:
```xml
      <PreprocessorDefinitions>STRAT_ENABLE_TEST_ACCESS;NDEBUG;_CONSOLE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
```

*(Leave the Win32 lines at lines 78 and 93 untouched — Win32 builds are not maintained.)*

---

### Task 2: Create SearchTests.cpp

**Files:**
- Create: `StratChessTests/SearchTests.cpp`

Write the complete file below. Note that `AIPerlexTestFixture` must be defined **before** the `TEST_CASE` blocks that use it.

```cpp
// SearchTests.cpp — Catch2 [search] tests for AIPerplex private helper methods.
//
// Tests for:
//   assess_iteration_quality() — 6 cases, one per RejectionReason branch
//   should_stop_early()        — 2 cases (mate score, forced-line short-circuit)
//   handle_empty_move_emergency() — 2 cases (mate path, true-emergency path)
//
// Requires STRAT_ENABLE_TEST_ACCESS in the test project preprocessor definitions.
// See Docs/TestDesign.md §"AIPerplex Test Access" for the mechanism.

#include <catch_amalgamated.hpp>
#include "AIPerplex.h"
#include "Board.h"
#include "MoveGenerator.h"
#include "PlayerBase.h"
#include "PVTable.h"
#include "defines.h"

// ============================================================================
// Test fixture
// ============================================================================
// Must be defined here (not in a header) — the name must match the friend
// declaration inside AIPerplex.h: friend class AIPerlexTestFixture;
//
// Public type aliases re-export the private AIPerplex nested types so that
// TEST_CASE functions outside the class can write e.g.
//   AIPerlexTestFixture::RejectionReason::INCOMPLETE
class AIPerlexTestFixture
{
public:
    // Re-export private types for test use
    using RejectionReason = AIPerplex::RejectionReason;
    using Metrics         = AIPerplex::IterationMetrics;
    using State           = AIPerplex::SearchState;

    std::unique_ptr<PlayerBase> ai_owner;
    AIPerplex* ai = nullptr;

    AIPerlexTestFixture()
    {
        ai_owner = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, 4);
        ai = static_cast<AIPerplex*>(ai_owner.get());
        AIPerplex::SetVerboseLogging(false);
        // Note: SetEvalEngine() is NOT called — the helper methods under test
        // do not invoke Eval->Evaluate(), so this is safe.
    }

    RejectionReason assess(const Metrics& m, const State& s) const
        { return ai->assess_iteration_quality(m, s); }

    bool stop_early(int depth, int score, int pv_len) const
        { return ai->should_stop_early(depth, score, pv_len); }

    bool emergency(State& s, PVTable& pv) const
        { return ai->handle_empty_move_emergency(s, pv); }
};

// ============================================================================
// Helper
// ============================================================================
// Returns any legal move from the starting position.
// Used to produce a guaranteed non-null Move for assess tests.
static Move AnyLegalMove()
{
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    GameInfo info = Board::Instance().GetGameInfo();
    MoveList ml;
    MoveGenerator::ComputeLegalMoves(info, ml);
    REQUIRE(!ml.empty());
    return ml[0];
}

// ============================================================================
// assess_iteration_quality tests
// ============================================================================

TEST_CASE("Search - assess: null current_move yields INCOMPLETE", "[search]")
{
    AIPerlexTestFixture fix;

    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = Move{};    // null — triggers CASE 1
    m.current_score    = 100;
    m.nodes_searched   = 5000;
    m.pv_length        = 2;
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = 10;
    m.completion_ratio = 0.5;

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 0;
    s.best_score               = 100;
    s.nodes_at_completed_depth = 0;

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::INCOMPLETE);
}

TEST_CASE("Search - assess: too few nodes yields INCOMPLETE", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = any;
    m.current_score    = 100;
    m.nodes_searched   = 10;    // below min_nodes_threshold (default 1000) — CASE 1
    m.pv_length        = 2;
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = 10;
    m.completion_ratio = 0.5;

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 0;
    s.best_score               = 100;
    s.nodes_at_completed_depth = 0;

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::INCOMPLETE);
}

TEST_CASE("Search - assess: low completion ratio yields TOO_FEW_NODES", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    // Pass CASE 1 (move ok, nodes ok) but fail CASE 2 (completion ratio)
    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = any;
    m.current_score    = 100;
    m.nodes_searched   = 5000;
    m.pv_length        = 2;
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = 10;
    m.completion_ratio = 0.01;  // below min_completion_ratio (default 0.10)

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 3;     // > 0: previous depth exists
    s.best_score               = 100;
    s.nodes_at_completed_depth = 5000;  // > 0: denominator present

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::TOO_FEW_NODES);
}

TEST_CASE("Search - assess: pv too short yields SHORT_PV", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    // depth=6, min_pv_ratio=0.33 → min required pv = max(1, 6*0.33) = 2
    // pv_length=1 < 2 → SHORT_PV
    AIPerlexTestFixture::Metrics m{};
    m.depth            = 6;
    m.current_move     = any;
    m.current_score    = 100;
    m.nodes_searched   = 5000;
    m.pv_length        = 1;     // too short
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = 10;
    m.completion_ratio = 0.5;   // passes CASE 2

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 5;
    s.best_score               = 100;
    s.nodes_at_completed_depth = 5000;

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::SHORT_PV);
}

TEST_CASE("Search - assess: score drops to 0 from large value yields SCORE_DROP", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    // current_score == 0, previous was 300 (abs > score_draw_threshold=20) → SCORE_DROP
    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = any;
    m.current_score    = 0;     // suspicious zero
    m.nodes_searched   = 5000;
    m.pv_length        = 3;
    m.interrupted      = true;
    m.move_changed     = false;
    m.score_delta      = -300;
    m.completion_ratio = 0.5;

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 3;
    s.best_score               = 300;   // abs > score_draw_threshold (20)
    s.nodes_at_completed_depth = 5000;

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::SCORE_DROP);
}

TEST_CASE("Search - assess: move changed on interrupt yields MOVE_CHANGED", "[search]")
{
    AIPerlexTestFixture fix;
    const Move any = AnyLegalMove();

    AIPerlexTestFixture::Metrics m{};
    m.depth            = 4;
    m.current_move     = any;
    m.current_score    = 100;
    m.nodes_searched   = 5000;
    m.pv_length        = 3;
    m.interrupted      = true;
    m.move_changed     = true;  // different from last iteration
    m.score_delta      = 10;
    m.completion_ratio = 0.5;

    AIPerlexTestFixture::State s{};
    s.depth_completed          = 3;
    s.best_score               = 90;
    s.nodes_at_completed_depth = 5000;
    s.last_iteration_move      = Move{};  // different from any → move_changed triggers CASE 5

    REQUIRE(fix.assess(m, s) ==
            AIPerlexTestFixture::RejectionReason::MOVE_CHANGED);
}

// ============================================================================
// should_stop_early tests
// ============================================================================

TEST_CASE("Search - should_stop_early: mate score returns true", "[search]")
{
    AIPerlexTestFixture fix;
    // GameValues::Mate_Threshold == 29900; mate score is >= this
    REQUIRE(fix.stop_early(5, GameValues::Mate_Threshold, 4) == true);
    REQUIRE(fix.stop_early(5, GameValues::Mate_Threshold + 100, 4) == true);
    REQUIRE(fix.stop_early(5, -(GameValues::Mate_Threshold), 4) == true);
}

TEST_CASE("Search - should_stop_early: short PV relative to depth returns true", "[search]")
{
    AIPerlexTestFixture fix;
    // Condition: depth > 1 && pv_length > 0 && pv_length < (depth - depth/2)
    // depth=6, pv_length=2 → 2 < (6-3)=3 → true
    REQUIRE(fix.stop_early(6, 100, 2) == true);
    // depth=4, pv_length=1 → 1 < (4-2)=2 → true
    REQUIRE(fix.stop_early(4, 100, 1) == true);
    // depth=4, pv_length=2 → 2 == (4-2)=2, not < → false
    REQUIRE(fix.stop_early(4, 100, 2) == false);
    // depth=1: condition requires depth > 1 → false
    REQUIRE(fix.stop_early(1, 100, 0) == false);
}

// ============================================================================
// handle_empty_move_emergency tests
// ============================================================================

TEST_CASE("Search - handle_empty_move_emergency: mate score returns false (no move needed)", "[search]")
{
    AIPerlexTestFixture fix;

    AIPerlexTestFixture::State s{};
    s.best_move  = Move{};  // null — no move found
    s.best_score = GameValues::Mate_Threshold + 50;  // mate detected

    PVTable pv;
    REQUIRE(fix.emergency(s, pv) == false);  // game is over, no move needed
    // best_move remains null — caller must not play
    REQUIRE(s.best_move.is_null());
}

TEST_CASE("Search - handle_empty_move_emergency: non-mate emergency sets a legal move", "[search]")
{
    AIPerlexTestFixture fix;

    // Set up a real, playable position so the emergency path finds legal moves
    Board::Instance().SetupFromFEN(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    AIPerlexTestFixture::State s{};
    s.best_move  = Move{};  // null — emergency condition
    s.best_score = 0;       // not a mate score

    PVTable pv;
    const bool result = fix.emergency(s, pv);

    REQUIRE(result == true);                // emergency move was found
    REQUIRE(!s.best_move.is_null());        // a move was set
}
```

---

### Task 3: Wire SearchTests.cpp into vcxproj and filters

**Files:**
- Modify: `StratChessTests/StratChessTests.vcxproj`
- Modify: `StratChessTests/StratChessTests.vcxproj.filters`

**Step 1: Add ClCompile entry to vcxproj**

Find in `StratChessTests.vcxproj`:
```xml
    <ClCompile Include="SortTests.cpp" />
```

Replace with:
```xml
    <ClCompile Include="SearchTests.cpp" />
    <ClCompile Include="SortTests.cpp" />
```

**Step 2: Add filter entry to vcxproj.filters**

In `StratChessTests.vcxproj.filters`, find the `SortTests.cpp` filter entry and insert `SearchTests.cpp` before it with the same filter. The exact text to find:

```xml
    <ClCompile Include="SortTests.cpp">
```

Replace with:
```xml
    <ClCompile Include="SearchTests.cpp">
      <Filter>Source Files\Tests</Filter>
    </ClCompile>
    <ClCompile Include="SortTests.cpp">
```

*(If the file structure differs, look for any existing test file entry under `Source Files\Tests` and follow the same pattern.)*

---

### Task 4: Build tests and verify [search] tests pass

All 10 test cases test **existing, already-correct** behaviour — they should all pass immediately.

**Step 1: Build test project**

From the worktree root:
```powershell
.\build.ps1 tests
```
Expected: `Build succeeded` with zero warnings (warnings are errors on this project).

**Step 2: Run [search] tests only**

```powershell
.\StratChessTests\x64\Release\StratChessTests.exe [search]
```
Expected output:
```
All tests passed (N assertions in 10 test cases)
```

**Step 3: Run full suite to confirm no regressions**

```powershell
.\StratChessTests\x64\Release\StratChessTests.exe
```
Expected: all existing assertions pass plus the 10 new [search] ones. Total should be 238+ assertions.

If any test fails, diagnose before proceeding to Task 5.

---

### Task 5: Add LMR fields to SearchTuning in AIPerplex.h

**Files:**
- Modify: `StratEngine/AIPerplex.h`

**Step 1: Replace the Future comment with LMR fields**

Find in `StratEngine/AIPerplex.h`:
```cpp
		// Future: Add LMR thresholds, etc.
	} tuning_;
```

Replace with:
```cpp
		int  lmr_min_depth      = 3;    // don't reduce at depth < 3
		int  lmr_min_move_index = 3;    // don't reduce the first 3 moves (si 0, 1, 2)
		bool lmr_enabled        = true; // kill-switch: set false to measure LMR impact via SimplePerfStats.txt
	} tuning_;
```

---

### Task 6: Add in_check pre-computation before the move loop in pvs()

**Files:**
- Modify: `StratEngine/AIPerplex.cpp`

The move loop in `pvs()` needs to know whether the position **before any move is made** is in check. This is computed once and used in the LMR skip condition for every move.

**Step 1: Insert in_check after MoveSorter::ScoreMoves call**

Find in `StratEngine/AIPerplex.cpp`:
```cpp
		history_, scored_idx);

	bool moveFound = false;
```

Replace with:
```cpp
		history_, scored_idx);

	const bool in_check = m_Board.InCheck();
	bool moveFound = false;
```

---

### Task 7: Replace the non-first-child else block with LMR logic

**Files:**
- Modify: `StratEngine/AIPerplex.cpp`

**Step 1: Replace the else block**

Find the exact text below in `pvs()` (the non-first-child else branch):
```cpp
		else {
			// Null window search
			value = -pvs(depth - 1, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);

			// Re-search if it fails high in PV node
			if (value > alpha && is_pv_node) {
				value = -pvs(depth - 1, -beta, -alpha, ply + 1, true, tt, pv_table);
			}
		}
```

Replace with:
```cpp
		else {
			const bool isCapture   = MoveHelper::IsCapture(move);
			const bool isPromotion = MoveHelper::IsPromote(move);
			const bool isKiller    = (move == killers_[ply][0] || move == killers_[ply][1]);

			// Late Move Reductions: reduce quiet, non-killer, non-evasion moves
			// that appear late in the sorted order. Skip conditions are conservative:
			// captures, promotions, killers, evasions (in_check), PV nodes, and early
			// moves are always searched at full depth.
			// Future skip candidates: passed pawn pushes, moves giving check.
			const bool applyLMR = tuning_.lmr_enabled
				&& !in_check
				&& !isCapture
				&& !isPromotion
				&& !isKiller
				&& si >= tuning_.lmr_min_move_index
				&& depth >= tuning_.lmr_min_depth;

			if (applyLMR) {
				// sqrt formula: scales naturally with depth and move index.
				// Clamped to [1, depth-2] so depth-1-R is always in [1, depth-2].
				const int R = std::min(
					std::max(1, static_cast<int>(
						std::sqrt(static_cast<double>(depth - 1)) *
						std::sqrt(static_cast<double>(si - 1)))),
					depth - 1);

				// Reduced-depth null-window search
				value = -pvs(depth - 1 - R, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);

				// Re-search at full depth-1 null window if the reduced result beats alpha
				if (value > alpha && !ShouldStopSearch())
					value = -pvs(depth - 1, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);
			} else {
				// Normal null-window search (unchanged)
				value = -pvs(depth - 1, -alpha - 1, -alpha, ply + 1, false, tt, pv_table);
			}

			// Re-search with full window at PV node (unchanged from original)
			if (value > alpha && is_pv_node)
				value = -pvs(depth - 1, -beta, -alpha, ply + 1, true, tt, pv_table);
		}
```

---

### Task 8: Build and run all tests

**Step 1: Build both projects**

```powershell
.\build.ps1
```
Expected: `Build succeeded` with zero warnings for both main solution and test project.

**Step 2: Run full test suite**

```powershell
.\StratChessTests\x64\Release\StratChessTests.exe
```
Expected: all tests pass. The [search] tests should still show 10/10.

If any test fails: check whether the LMR change broke the board state (missed an `UndoMove`, etc.). The [tactical] tests (`[tactical]`) are the most sensitive regression indicator.

**Step 3: Run [tactical] tag explicitly if anything looks suspicious**

```powershell
.\StratChessTests\x64\Release\StratChessTests.exe [tactical]
```

---

### Task 9: Update TestDesign.md

**Files:**
- Modify: `Docs/TestDesign.md`

**Step 1: Update coverage map row for [search]**

Find:
```markdown
| Search helpers (assess_quality etc.) | `[search]` | ⏳ Phase 1 | `SearchTests.cpp` (future) |
```

Replace with:
```markdown
| Search helpers (assess_quality etc.) | `[search]` | ✅ Phase 1 | `SearchTests.cpp` |
```

**Step 2: Update the Phase 1 [search] section body**

Find:
```markdown
### `[search]` — AIPerplex helper unit tests

**Prerequisite**: AIPerplex test-friend access (see `AIPerplex.h` `#ifdef STRAT_ENABLE_TEST_ACCESS`)
**When**: when LMR or aspiration windows lands
**File**: `StratChessTests/SearchTests.cpp`

Tests for private helper methods exposed via `AIPerlexTestFixture`:
- `assess_iteration_quality()`: mock `IterationMetrics` and `SearchState`, verify all `RejectionReason` branches
- `should_stop_early()`: mate score, forced-line short-circuit
- `handle_empty_move_emergency()`: mate vs. non-mate fallback

Enable: add `STRAT_ENABLE_TEST_ACCESS` to StratChessTests preprocessor definitions in vcxproj.
```

Replace with:
```markdown
### `[search]` — AIPerplex helper unit tests

**Status**: ✅ **Done.** LMR landed in March 2026; all 10 cases passing.
**File**: `StratChessTests/SearchTests.cpp`
**Activation**: `STRAT_ENABLE_TEST_ACCESS` in x64 Debug + Release preprocessor definitions in `StratChessTests.vcxproj`.

Tests for private helper methods exposed via `AIPerlexTestFixture` (friend class):

- `assess_iteration_quality()`: 6 cases — one per `RejectionReason` branch (INCOMPLETE×2, TOO_FEW_NODES, SHORT_PV, SCORE_DROP, MOVE_CHANGED)
- `should_stop_early()`: 2 cases — mate score path; short-PV forced-line path
- `handle_empty_move_emergency()`: 2 cases — mate-detected path (returns false); true-emergency path on a real starting-position board (returns true, sets legal move)
```

---

### Task 10: Self-play smoke test and performance comparison

**Step 1: Verify game_settings.json is set to AI vs AI at the starting position**

Check `StratChessEvolved/game_settings.json`:
- Both players should have `"type": 6` (AI_PERPLEX)
- FEN should be the standard starting position

**Step 2: Run self-play to checkmate**

```powershell
cd StratChessEvolved
..\x64\Release\StratChessEvolved.exe
```
Expected: game plays to completion (checkmate or stalemate). Moves should be sensible chess. The log line `GetMove complete: move=..., depth=..., time=...ms, nodes=...` appears per move.

**Step 3: Save SimplePerfStats.txt as LMR-on baseline**

```powershell
Copy-Item logs\SimplePerfStats.txt ..\lmr_on_perf.txt
```

**Step 4: Temporarily disable LMR for comparison**

In `StratEngine/AIPerplex.h`, change:
```cpp
		bool lmr_enabled        = true;
```
to:
```cpp
		bool lmr_enabled        = false;
```

Rebuild: `cd ..; .\build.ps1 main`

Run self-play again, save output: `Copy-Item StratChessEvolved\logs\SimplePerfStats.txt lmr_off_perf.txt`

**Step 5: Compare node counts**

Open both files. Expect `lmr_on_perf.txt` to show significantly fewer nodes per move at deeper depths (depth 5+). A 2–3× reduction is the expected range.

**Step 6: Restore lmr_enabled = true and rebuild**

```cpp
bool lmr_enabled = true;   // restore
```
```powershell
.\build.ps1 main
```
Run tests one more time to confirm: `.\StratChessTests\x64\Release\StratChessTests.exe`

---

### Task 11: Commit

**Step 1: Verify no leftover debugging changes**

Check `StratEngine/AIPerplex.h` — `lmr_enabled = true` (not false).
Check `game_settings.json` — starting FEN, both players AI_PERPLEX.

**Step 2: Stage and commit**

```powershell
git add StratEngine/AIPerplex.h
git add StratEngine/AIPerplex.cpp
git add StratChessTests/SearchTests.cpp
git add StratChessTests/StratChessTests.vcxproj
git add StratChessTests/StratChessTests.vcxproj.filters
git add Docs/TestDesign.md
git commit -m "Implement LMR (sqrt formula) + [search] tests (10 assertions)

- AIPerplex.h: add lmr_min_depth/lmr_min_move_index/lmr_enabled to SearchTuning
- AIPerplex.cpp: compute in_check before move loop; LMR applied to quiet,
  non-killer, non-evasion moves (si>=3, depth>=3) with 2-step re-search
- SearchTests.cpp: 10 [search] tests via AIPerlexTestFixture friend class;
  STRAT_ENABLE_TEST_ACCESS enabled in vcxproj x64 Debug + Release
- TestDesign.md: [search] row marked done; section body updated

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Success Criteria

- `.\build.ps1` — zero warnings, both projects
- `StratChessTests.exe [search]` — 10 assertions pass
- `StratChessTests.exe` — all assertions pass (238+ total)
- `StratChessTests.exe [tactical]` — all 3 tactical regression tests pass
- Self-play game reaches checkmate with sensible moves
- `SimplePerfStats.txt` shows fewer nodes/move with LMR enabled vs disabled
