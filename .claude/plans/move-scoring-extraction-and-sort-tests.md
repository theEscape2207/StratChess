# Move Scoring Extraction: MoveSorter::ScoreMoves + [sort] tests

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** Extract the 25-line inline move-scoring loop from `pvs()` into `MoveSorter::ScoreMoves()`, then add `[sort]` tests that lock in the full priority ordering.

**Architecture:** `ScoreMoves` is a new static method on `MoveSorter` (in `Sort.h`/`Sort.cpp`). It receives board, side, pv_move, hash_move, two killers, and the history table, and fills an `std::array<std::pair<int,int>, MoveList::MAX_MOVES>` with `(score, original_index)` pairs — same structure pvs() already uses for sorted iteration. `pvs()` calls it instead of the inline loop and deletes the `TODO: Relocate MoveSorting` comment. The existing `SortMoves`/`SortMovesIter`/`SortMovesByValue` methods are **not touched** (used by `AIAgent`/`AIBasic`/`ABIterative`/`PlayerAI`).

**Tech Stack:** C++20, MSVC, MSBuild, Catch2 v3, `StratEngine/Sort.h`, `StratEngine/Sort.cpp`, `StratEngine/AIPerplex.cpp`, `StratChessTests/SortTests.cpp`, `StratChessTests/StratChessTests.vcxproj`, `StratChessTests/StratChessTests.vcxproj.filters`

---

## Background: what the inline loop does

In `AIPerplex.cpp` `pvs()`, lines 317–341:

```cpp
// Scores each move with an integer priority. Higher = searched first.
for (int i = 0; i < n; ++i) {
    const Move& mv = moveList[i];
    int s = 0;
    if (mv == pv_move)           { s = 2'000'000; }
    else if (mv == hash_move)    { s = 1'900'000; }
    else {
        const bool isCapture = MoveHelper::IsCapture(mv);
        const bool isKiller0 = !isCapture && (mv == killers_[ply][0]);
        const bool isKiller1 = !isCapture && (mv == killers_[ply][1]);
        const int  mvv_lva = MoveHelper::Value(mv, board.GetEffectiveMovPiece(mv),
                                                    board.GetCapturedPiece(mv));
        if      (isKiller0)                    s = 900'000;
        else if (isKiller1)                    s = 800'000;
        else if (isCapture && mvv_lva > 0)     s = 1'000'000 + mvv_lva; // winning
        else if (isCapture && mvv_lva == 0)    s = 700'000  + mvv_lva;  // equal
        else if (isCapture)                    s = -100'000 + mvv_lva;  // losing
        else                                   s = history_[side][mv.from()][mv.to()];
    }
    scored_idx[i] = { s, i };
}
std::sort(scored_idx.begin(), scored_idx.begin() + n,
    [](const auto& a, const auto& b) { return a.first > b.first; });
// TODO: Relocate MoveSorting to MoveSorter class   ← delete this after extraction
```

Priority order (highest to lowest):
1. PV move (2,000,000)
2. Hash move (1,900,000)
3. Winning captures — MVV-LVA > 0 (1,000,000 + mvv_lva)
4. Killer slot 0 (900,000) — quiet only
5. Killer slot 1 (800,000) — quiet only
6. Equal captures — MVV-LVA == 0 (700,000)
7. Losing captures — MVV-LVA < 0 (-100,000 + mvv_lva)
8. Quiet moves — scored by history table (can be 0 if never updated)

---

## Task 1: Write the failing [sort] test

**Files:**
- Create: `StratChessTests/SortTests.cpp`

**Step 1: Create SortTests.cpp**

Note: `ScoreMoves` doesn't exist yet, so this won't compile. That's intentional — TDD.

Test position: `4k3/8/8/8/8/8/r7/R3K3 w - - 0 1`
- White Ra1, Ke1 vs Black Ra2, Ke8
- `Ra1xa2` is the only capture; many quiet rook and king moves available
- MVV-LVA for Ra1xa2: `rook_value - rook_value/16` — positive, "winning capture" category

```cpp
// SortTests.cpp — Catch2 tests for MoveSorter::ScoreMoves() priority ordering
//
// Tests that moves are scored in the expected priority order:
//   PV move > hash move > winning captures > killer0 > killer1 > equal captures
//   > quiet history > losing captures (none in this position)

#include <catch_amalgamated.hpp>
#include "Board.h"
#include "Sort.h"
#include "MoveFactory.h"
#include "MoveGenerator.h"
#include "defines.h"

// Rook endgame: White Ra1, Ke1 vs Black Ra2, Ke8.
// Ra1xa2 is the only capture; all other legal white moves are quiet.
static constexpr const char* FEN_SORT =
    "4k3/8/8/8/8/8/r7/R3K3 w - - 0 1";

// Helper: find the score assigned to a specific move in out_scored_idx.
// Returns INT_MIN if not found.
static int FindScore(
    const std::array<std::pair<int,int>, MoveList::MAX_MOVES>& scored_idx,
    const MoveList& moveList,
    int n,
    const Move& target)
{
    for (int i = 0; i < n; ++i) {
        if (moveList[scored_idx[i].second] == target)
            return scored_idx[i].first;
    }
    return INT_MIN;
}

TEST_CASE("Sort - PV move scores 2'000'000", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());
    REQUIRE(n > 1);

    // Pick any legal move as the PV move (use the first one)
    const Move pv_move  = moveList[0];
    const Move null_move;
    const Move killer0, killer1;                       // null — no killers set
    int32_t history[2][64][64] = {};

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           pv_move, null_move,
                           killer0, killer1,
                           history, scored_idx);

    const int pv_score = FindScore(scored_idx, moveList, n, pv_move);
    REQUIRE(pv_score == 2'000'000);
    // PV move should be first after sorting
    REQUIRE(moveList[scored_idx[0].second] == pv_move);
}

TEST_CASE("Sort - Hash move scores 1'900'000 when not the PV move", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());
    REQUIRE(n > 2);

    const Move pv_move   = moveList[0];
    const Move hash_move = moveList[1]; // different from pv_move
    const Move killer0, killer1;
    int32_t history[2][64][64] = {};

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           pv_move, hash_move,
                           killer0, killer1,
                           history, scored_idx);

    REQUIRE(FindScore(scored_idx, moveList, n, hash_move) == 1'900'000);
}

TEST_CASE("Sort - Capture scores above 1'000'000 (winning capture category)", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());

    // Find Ra1xa2 in the move list
    const auto it = std::find_if(moveList.begin(), moveList.end(), [](const Move& m) {
        return m.from() == a1 && m.to() == a2;
    });
    REQUIRE(it != moveList.end());
    const Move capture = *it;

    const Move null_move, killer0, killer1;
    int32_t history[2][64][64] = {};

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           null_move, null_move,
                           killer0, killer1,
                           history, scored_idx);

    const int cap_score = FindScore(scored_idx, moveList, n, capture);
    REQUIRE(cap_score >= 1'000'000); // winning-capture category
}

TEST_CASE("Sort - Killer0 scores 900'000; beats quiet move with no history", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());

    // Find a quiet move to designate as killer0 (not the capture a1xa2)
    const auto it = std::find_if(moveList.begin(), moveList.end(), [](const Move& m) {
        return m.from() == a1 && m.to() != a2; // quiet rook move
    });
    REQUIRE(it != moveList.end());
    const Move killer0 = *it;

    // Find a different quiet move (will have history = 0)
    const auto it2 = std::find_if(moveList.begin(), moveList.end(), [&](const Move& m) {
        return m != killer0 && m.from() == e1; // quiet king move
    });
    REQUIRE(it2 != moveList.end());
    const Move quiet_no_history = *it2;

    const Move null_move, killer1;
    int32_t history[2][64][64] = {};

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           null_move, null_move,
                           killer0, killer1,
                           history, scored_idx);

    REQUIRE(FindScore(scored_idx, moveList, n, killer0)          == 900'000);
    REQUIRE(FindScore(scored_idx, moveList, n, quiet_no_history) == 0);
    REQUIRE(FindScore(scored_idx, moveList, n, killer0) >
            FindScore(scored_idx, moveList, n, quiet_no_history));
}

TEST_CASE("Sort - Quiet move with positive history scores exactly that history value", "[sort]")
{
    Board& board = Board::Instance();
    board.SetupFromFEN(FEN_SORT);

    GameInfo info = board.GetGameInfo();
    MoveList moveList;
    MoveGenerator::ComputeLegalMoves(info, moveList);
    const int n = static_cast<int>(moveList.size());

    // Find a quiet king move
    const auto it = std::find_if(moveList.begin(), moveList.end(), [](const Move& m) {
        return m.from() == e1 && m.to() == d1;
    });
    REQUIRE(it != moveList.end());
    const Move quiet_move = *it;

    const Move null_move, killer0, killer1;
    int32_t history[2][64][64] = {};
    history[WHITE][e1][d1] = 42; // inject a history score

    std::array<std::pair<int,int>, MoveList::MAX_MOVES> scored_idx;
    MoveSorter::ScoreMoves(moveList, n, board, WHITE,
                           null_move, null_move,
                           killer0, killer1,
                           history, scored_idx);

    REQUIRE(FindScore(scored_idx, moveList, n, quiet_move) == 42);
}
```

**Step 2: Try to build — expect a compile error**

```powershell
.\build.ps1 tests
```

Expected: compile error — `MoveSorter` has no member `ScoreMoves`.

---

## Task 2: Declare ScoreMoves in Sort.h

**Files:**
- Modify: `StratEngine/Sort.h`

**Step 1: Add the declaration**

In `Sort.h`, add after `SortMovesByValue` in the public section:

```cpp
// Score all moves in [0, n) into out_scored_idx as (score, original_index) pairs,
// sorted descending by score. Priority: PV move → hash move → winning captures →
// killer0 → killer1 → equal captures → history (quiet) → losing captures.
static void ScoreMoves(
    const MoveList&                                          moveList,
    int                                                      n,
    const Board&                                             board,
    eColor                                                   side,
    const Move&                                              pv_move,
    const Move&                                              hash_move,
    const Move&                                              killer0,
    const Move&                                              killer1,
    const int32_t                                            (&history)[2][64][64],
    std::array<std::pair<int, int>, MoveList::MAX_MOVES>&    out_scored_idx
);
```

`Sort.h` currently only includes `Move.h`. Verify `eColor` is reachable: `Move.h → PieceHelper.h → defines.h` — it is. No new includes needed.

---

## Task 3: Implement ScoreMoves in Sort.cpp

**Files:**
- Modify: `StratEngine/Sort.cpp`

**Step 1: Add the implementation at the end of Sort.cpp**

```cpp
void MoveSorter::ScoreMoves(
    const MoveList&                                         moveList,
    int                                                     n,
    const Board&                                            board,
    eColor                                                  side,
    const Move&                                             pv_move,
    const Move&                                             hash_move,
    const Move&                                             killer0,
    const Move&                                             killer1,
    const int32_t                                           (&history)[2][64][64],
    std::array<std::pair<int, int>, MoveList::MAX_MOVES>&   out_scored_idx)
{
    for (int i = 0; i < n; ++i) {
        const Move& mv = moveList[i];
        int s = 0;

        if (mv == pv_move) { s = 2'000'000; }
        else if (mv == hash_move) { s = 1'900'000; }
        else {
            const bool isCapture = MoveHelper::IsCapture(mv);
            const bool isKiller0 = !isCapture && (mv == killer0);
            const bool isKiller1 = !isCapture && (mv == killer1);
            const int  mvv_lva   = MoveHelper::Value(mv,
                                        board.GetEffectiveMovPiece(mv),
                                        board.GetCapturedPiece(mv));

            if      (isKiller0)                 s = 900'000;
            else if (isKiller1)                 s = 800'000;
            else if (isCapture && mvv_lva > 0)  s = 1'000'000 + mvv_lva;
            else if (isCapture && mvv_lva == 0) s = 700'000   + mvv_lva;
            else if (isCapture)                 s = -100'000  + mvv_lva;
            else                                s = history[side][mv.from()][mv.to()];
        }
        out_scored_idx[i] = { s, i };
    }

    std::sort(out_scored_idx.begin(), out_scored_idx.begin() + n,
        [](const auto& a, const auto& b) { return a.first > b.first; });
}
```

Note: `Sort.cpp` already includes `MoveHelper.h` and `Board.h`. No new includes needed.

---

## Task 4: Wire SortTests.cpp into the test project

**Files:**
- Modify: `StratChessTests/StratChessTests.vcxproj`
- Modify: `StratChessTests/StratChessTests.vcxproj.filters`

**Step 1: Add to vcxproj**

In `StratChessTests.vcxproj`, in the `<ItemGroup>` containing other test `.cpp` files (around line 171 where `BoardTests.cpp` appears), add:

```xml
<ClCompile Include="SortTests.cpp" />
```

**Step 2: Add to vcxproj.filters**

In `StratChessTests.vcxproj.filters`, in the `<ItemGroup>` where `BoardTests.cpp` and `TacticalTests.cpp` appear under `Source Files\Tests`, add:

```xml
<ClCompile Include="SortTests.cpp">
  <Filter>Source Files\Tests</Filter>
</ClCompile>
```

---

## Task 5: Build tests and run [sort] — expect all 5 to pass

**Step 1: Build**

```powershell
.\build.ps1 tests
```

Expected: clean build, no errors.

**Step 2: Run [sort] tests only**

```
StratChessTests/x64/Release/StratChessTests.exe [sort]
```

Expected output:
```
Filters: [sort]
===============================================================================
All tests passed (N assertions in 5 test cases)
```

If any test fails, read the failure message. Common issues:
- `FindScore` returns `INT_MIN` → the move wasn't found in `moveList` (check FEN and move generation).
- Score mismatch → the scoring logic in `ScoreMoves` has a conditional ordering error.

---

## Task 6: Replace the inline loop in pvs() with ScoreMoves

**Files:**
- Modify: `StratEngine/AIPerplex.cpp:317–344`

**Step 1: Replace the inline scoring block**

Find this block in `pvs()` (roughly lines 313–344):

```cpp
// Stack-allocated scored index array — zero heap allocation per call.
std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
const int n = static_cast<int>(moveList.size());
const eColor side = m_Board.GetCurrentColor();

for (int i = 0; i < n; ++i) {
    const Move& mv = moveList[i];
    int s = 0;
    if (mv == pv_move) { s = 2'000'000; }
    else if (mv == hash_move) { s = 1'900'000; }
    else {
        const bool isCapture = MoveHelper::IsCapture(mv);
        const bool isKiller0 = !isCapture && (mv == killers_[ply][0]);
        const bool isKiller1 = !isCapture && (mv == killers_[ply][1]);
        const int  mvv_lva = MoveHelper::Value(mv, m_Board.GetEffectiveMovPiece(mv), m_Board.GetCapturedPiece(mv));
        if (isKiller0)                    s = 900'000;
        else if (isKiller1)               s = 800'000;
        else if (isCapture && mvv_lva > 0) s = 1'000'000 + mvv_lva;
        else if (isCapture && mvv_lva == 0) s = 700'000 + mvv_lva;
        else if (isCapture)               s = -100'000 + mvv_lva;
        else                              s = history_[side][mv.from()][mv.to()];
    }
    scored_idx[i] = { s, i };
}
std::sort(scored_idx.begin(), scored_idx.begin() + n,
    [](const auto& a, const auto& b) { return a.first > b.first; });


// TODO: Relocate MoveSorting to MoveSorter class
```

Replace with:

```cpp
// Stack-allocated scored index array — zero heap allocation per call.
std::array<std::pair<int, int>, MoveList::MAX_MOVES> scored_idx;
const int n = static_cast<int>(moveList.size());
const eColor side = m_Board.GetCurrentColor();

MoveSorter::ScoreMoves(moveList, n, m_Board, side,
                       pv_move, hash_move,
                       killers_[ply][0], killers_[ply][1],
                       history_, scored_idx);
```

Note: `Sort.h` must be included in `AIPerplex.cpp`. Check the existing includes at the top of that file and add `#include "Sort.h"` if it is not already present.

---

## Task 7: Build full solution and run all tests

**Step 1: Build everything**

```powershell
.\build.ps1
```

Expected: clean build, no warnings.

**Step 2: Run the full fast test suite**

```
StratChessTests/x64/Release/StratChessTests.exe
```

Expected: all tests pass (count will be current total + 5 new [sort] assertions).

**Step 3: Run a quick self-play check**

From `StratChessEvolved/`:

```powershell
Start-Process "..\x64\Release\StratChessEvolved.exe" -PassThru -NoNewWindow -RedirectStandardOutput out.txt
Start-Sleep -Seconds 15
Stop-Process -Name StratChessEvolved -ErrorAction SilentlyContinue
Get-Content out.txt | Select-String "GetMove complete" | Select-Object -Last 5
```

Expected: several `GetMove complete: move=..., depth=..., nodes=...` lines — search is functioning normally.

---

## Task 8: Update TestDesign.md and commit

**Files:**
- Modify: `Docs/TestDesign.md`
- Modify: `StratEngine/Sort.h`
- Modify: `StratEngine/Sort.cpp`
- Modify: `StratEngine/AIPerplex.cpp`
- Create:  `StratChessTests/SortTests.cpp`
- Modify: `StratChessTests/StratChessTests.vcxproj`
- Modify: `StratChessTests/StratChessTests.vcxproj.filters`

**Step 1: Update TestDesign.md coverage map**

In the coverage table, change:
```
| Move ordering (Sort) | `[sort]` | ⏳ Phase 1 | `SortTests.cpp` (future) |
```
to:
```
| Move ordering (Sort) | `[sort]` | ✅ Phase 1 | `SortTests.cpp` |
```

Also update the Phase 1 `[sort]` section body to show it as completed with a summary of test cases.

**Step 2: Commit**

```bash
git add StratEngine/Sort.h StratEngine/Sort.cpp StratEngine/AIPerplex.cpp \
        StratChessTests/SortTests.cpp \
        StratChessTests/StratChessTests.vcxproj \
        StratChessTests/StratChessTests.vcxproj.filters \
        Docs/TestDesign.md
git commit -m "refactor: extract inline pvs() move scoring into MoveSorter::ScoreMoves; add [sort] tests"
```

---

## Key Correctness Properties

After this change, the following must hold:

1. `pvs()` produces identical move ordering to before — the extracted logic is a verbatim copy, no logic changes.
2. All existing tests pass: `[repetition]`, `[moves]`, `[perft]`, `[tt]`, `[eval]`, `[tactical]`, `[board]`, `[formatter]`.
3. [sort] tests pass: 5 test cases covering PV, hash, capture, killer, and history priority.
4. The `// TODO: Relocate MoveSorting to MoveSorter class` comment is gone.
5. `Sort.h` still compiles without any new `#include` directives.
