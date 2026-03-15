# Tactical Suite Expansion + UCI Depth/Infinite Fix Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand the tactical test suite from 3 to ~35 positions split across a fast and slow tier, and fix two UCI bugs where `go depth N` incorrectly hit a 10 s time cap and `go infinite` inherited stale time state from the previous move.

**Architecture:** UCI fix is a self-contained change to `UCIHandler.cpp`. The test suite work adds a shared header (`TacticalTestHelpers.h`), refactors the existing `TacticalTests.cpp` to use Catch2's `GENERATE`/`from_range` pattern, and introduces a new slow-tier file (`TacticalFullTests.cpp`). `build.ps1` gains a `~[slow]` default for `run-tests` and a new `extended-tests` target.

**Tech Stack:** C++20, Catch2 v3 (`from_range`, `GENERATE`, `INFO`), MSBuild `.vcxproj`/`.vcxproj.filters`, PowerShell 5+

---

## File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `StratEngine/UCIHandler.cpp` | Modify | Fix `cmd_go()` time-config block; reset `time_limit_` in `stop_and_join()` |
| `StratChessTests/TacticalTestHelpers.h` | **Create** | `TacticalCase` struct + `make_tactical_engine(depth)` helper |
| `StratChessTests/TacticalTests.cpp` | Modify | Refactor 3 individual TEST_CASEs → 1 `GENERATE`; add ~7 positions |
| `StratChessTests/TacticalFullTests.cpp` | **Create** | Slow-tier `[tactical_full][slow]`, ~25 positions at depth 6 |
| `StratChessTests/StratChessTests.vcxproj` | Modify | Add `TacticalFullTests.cpp` (ClCompile) + `TacticalTestHelpers.h` (ClInclude) |
| `StratChessTests/StratChessTests.vcxproj.filters` | Modify | Add Filter entries for both new files |
| `build.ps1` | Modify | `extended-tests` in ValidateSet; `~[slow]` as default; new `extended-tests` case |

---

## Chunk 1: UCI Depth/Infinite Fix

### Task 1: Fix `UCIHandler.cpp` — `cmd_go()` time-config + `stop_and_join()` reset

**Files:**
- Modify: `StratEngine/UCIHandler.cpp:112-120` (time-config block)
- Modify: `StratEngine/UCIHandler.cpp:170-175` (stop_and_join)

The current `else if (!p.infinite)` branch fires for `go depth N` (no time params), incorrectly
capping search at 10 s. `go infinite` also inherits stale `time_limit_` from the previous call.

- [ ] **Step 1: Read `UCIHandler.cpp` and locate the two sites**

  In `cmd_go()` (around line 112):
  ```cpp
  // BEFORE — broken
  } else if (!p.infinite) {
      ai_->SetTimeLimit(std::chrono::seconds(10));
  }
  ```
  In `stop_and_join()` (around line 174):
  ```cpp
  // BEFORE — missing reset
  if (ai_) ai_->SetMaxDepth(UCI_DEFAULT_DEPTH);
  ```

- [ ] **Step 2: Apply the fix to `cmd_go()` time-config block**

  Replace the entire time-config `if/else if/else if` chain (lines 112–120) with:
  ```cpp
  // Configure time — pick first matching case
  if (p.movetime > 0) {
      ai_->SetTimeLimit(std::chrono::milliseconds(p.movetime));
  } else if (p.wtime > 0 || p.btime > 0) {
      auto remaining = std::chrono::milliseconds(white ? p.wtime : p.btime);
      auto inc       = std::chrono::milliseconds(white ? p.winc  : p.binc);
      ai_->SetClockInfo(remaining, inc, p.movestogo);
  } else if (p.infinite || p.depth > 0) {
      // Pure depth constraint or infinite: give the engine a large time budget.
      // The depth cap in iterative_deepening() is the sole stopping criterion.
      // For go infinite, the UCI 'stop' command is the intended termination.
      // hours(1) avoids potential overflow from milliseconds::max() in comparisons.
      ai_->SetTimeLimit(std::chrono::hours(1));
  } else {
      // No constraints at all — apply a safe fallback.
      ai_->SetTimeLimit(std::chrono::seconds(10));
  }
  ```

- [ ] **Step 3: Add `time_limit_` reset to `stop_and_join()`**

  After `if (ai_) ai_->SetMaxDepth(UCI_DEFAULT_DEPTH);`, add:
  ```cpp
  if (ai_) ai_->SetTimeLimit(std::chrono::seconds(15));
  ```
  Full function after the change:
  ```cpp
  void UciHandler::stop_and_join()
  {
      if (ai_) ai_->StopSearch();
      if (search_thread_.joinable()) search_thread_.join();
      if (ai_) ai_->SetMaxDepth(UCI_DEFAULT_DEPTH);
      if (ai_) ai_->SetTimeLimit(std::chrono::seconds(15));
  }
  ```
  **Why 15 s:** matches the `time_limit_{ std::chrono::seconds(15) }` default in `PlayerAI.h`,
  so a subsequent bare `go` falls through to the 10 s safety fallback exactly as before.

- [ ] **Step 4: Build tests to confirm no compile errors**

  ```powershell
  .\build.ps1 tests
  ```
  Expected: clean build, zero errors/warnings.

- [ ] **Step 5: Run `[uci]` tag to confirm existing parse_go tests still pass**

  ```powershell
  .\build.ps1 run-tests "[uci]"
  ```
  Expected: all 8 `[uci]` tests pass, zero failures.

- [ ] **Step 6: Commit**

  ```
  git add StratEngine/UCIHandler.cpp
  git commit -m "fix: go depth N and go infinite no longer hit stale/wrong time limit"
  ```

---

## Chunk 2: Shared Header + Refactored TacticalTests

### Task 2: Create `TacticalTestHelpers.h`

**Files:**
- Create: `StratChessTests/TacticalTestHelpers.h`

This header is the single source of truth for the `TacticalCase` struct and the `make_tactical_engine` factory. Both `TacticalTests.cpp` and `TacticalFullTests.cpp` include it.

- [ ] **Step 1: Create `StratChessTests/TacticalTestHelpers.h`**

  ```cpp
  // TacticalTestHelpers.h — shared helpers for TacticalTests.cpp and TacticalFullTests.cpp
  #pragma once
  #include "AIPerplex.h"
  #include "PlayerBase.h"
  #include "Eval.h"
  #include "defines.h"
  #include <memory>

  // One row in the tactical test table.
  struct TacticalCase {
      const char* label;        // shown in Catch2 failure output via INFO()
      const char* fen;
      eSquare     expected_from;
      eSquare     expected_to;
      unsigned    depth;
  };

  // Create a fresh AIPerplex at the given depth, configured for test use.
  // Call AFTER Board::Instance().SetupFromFEN() — board state is read during search.
  inline std::unique_ptr<PlayerBase> make_tactical_engine(unsigned depth)
  {
      auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX, depth);
      AIPerplex::SetVerboseLogging(false);          // suppress after ctor re-enables it
      ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
      return ai;
  }
  ```

- [ ] **Step 2: Build tests to confirm the header compiles (will be included in next task)**

  The header is not yet included anywhere, so this step just verifies the project still builds clean after adding the file:
  ```powershell
  .\build.ps1 tests
  ```
  Expected: clean build.

### Task 3: Refactor `TacticalTests.cpp` — 3 individual TEST_CASEs → 1 GENERATE + ~7 new positions

**Files:**
- Modify: `StratChessTests/TacticalTests.cpp`

The current file has 3 independent `TEST_CASE` blocks and a local `make_engine()` helper. Replace
it entirely with a single `GENERATE`-driven test using `from_range(kFastCases)` and include the
shared header.

**Position table for `kFastCases` (depth 4, ~10 positions):**

All candidate positions must be verified against the current engine before committing: run `go depth 4`
on each FEN and confirm the engine returns the expected `bestmove`. If any position returns a
different move, replace it with an alternative that the engine does solve uniquely.

| # | Label | FEN | `from` | `to` | Notes |
|---|-------|-----|--------|------|-------|
| 1 | M1: rook back rank | `6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1` | `a1` | `a8` | existing — Ra8# |
| 2 | M1: queen back rank | `6k1/5ppp/8/8/8/8/3Q4/6K1 w - - 0 1` | `d2` | `d8` | existing — Qd8# |
| 3 | capture: hanging rook | `4k3/8/8/8/8/8/8/2rQK3 w - - 0 1` | `d1` | `c1` | existing — Qxc1 |
| 4 | M1: queen corner | `k7/2Q5/K7/8/8/8/8/8 w - - 0 1` | `c7` | `b8` | Qb8# — Ka8 boxed |
| 5 | M1: rook ladder | `7k/7R/6R1/8/8/8/8/7K w - - 0 1` | `g6` | `g8` | Rg8# — h7 covered by Rh7 |
| 6 | capture: hanging queen | `4k3/8/8/3q4/8/8/8/3QK3 w - - 0 1` | `d1` | `d5` | Qxd5 — unprotected |
| 7 | capture: hanging knight | `4k3/8/8/8/8/5n2/8/3BK3 w - - 0 1` | `d1` | `f3` | Bxf3 via e2 |
| 8 | fork: knight Nc7+ | `r3k3/8/8/3N4/8/8/8/4K3 w - - 0 1` | `d5` | `c7` | check + wins Ra8 |
| 9 | skewer: Re8+ wins Ra8 | `r3k3/8/8/8/8/8/8/4RK2 w - - 0 1` | `e1` | `e8` | Re8+ → Rxa8 |
| 10 | skewer: Qc8+ wins Rg8 | `4k1r1/8/5p2/8/2Q5/8/8/4K3 w - - 0 1` | `c4` | `c8` | pf6 blocks direct Qxg8 |

- [ ] **Step 1: Write failing test by replacing `TacticalTests.cpp` entirely**

  ```cpp
  // TacticalTests.cpp — fast tactical suite [tactical]
  //
  // Runs ~10 positions at depth 4 (< 5 s total in Release).
  // Each position has a single forced best move verified against the engine.
  //
  // Selection invariant: every position in kFastCases must have a unique best move
  // at depth 4. Verify with: go depth 4 on each FEN before committing new positions.
  //
  // See Docs/TestDesign.md §Phase 0 for rationale.

  #include <catch_amalgamated.hpp>
  #include "TacticalTestHelpers.h"
  #include "Board.h"

  // ---------------------------------------------------------------------------
  // Position table — fast tier (depth 4, ~10 positions)
  // ---------------------------------------------------------------------------

  static constexpr TacticalCase kFastCases[] = {
      // — Mate-in-1 ————————————————————————————————————————————————————————
      { "M1: rook back rank",
        "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1",   a1, a8, 4 },
      { "M1: queen back rank",
        "6k1/5ppp/8/8/8/8/3Q4/6K1 w - - 0 1",     d2, d8, 4 },
      { "M1: queen corner (Qb8#)",
        "k7/2Q5/K7/8/8/8/8/8 w - - 0 1",           c7, b8, 4 },
      { "M1: rook ladder (Rg8#)",
        "7k/7R/6R1/8/8/8/8/7K w - - 0 1",          g6, g8, 4 },
      // — Winning captures ——————————————————————————————————————————————————
      { "capture: hanging rook (Qxc1)",
        "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1",         d1, c1, 4 },
      { "capture: hanging queen (Qxd5)",
        "4k3/8/8/3q4/8/8/8/3QK3 w - - 0 1",        d1, d5, 4 },
      { "capture: hanging knight (Bxf3)",
        "4k3/8/8/8/8/5n2/8/3BK3 w - - 0 1",        d1, f3, 4 },
      // — Simple 2-ply tactics ——————————————————————————————————————————————
      { "fork: Nc7+ wins Ra8",
        "r3k3/8/8/3N4/8/8/8/4K3 w - - 0 1",        d5, c7, 4 },
      { "skewer: Re8+ wins Ra8",
        "r3k3/8/8/8/8/8/8/4RK2 w - - 0 1",         e1, e8, 4 },
      { "skewer: Qc8+ wins Rg8",
        "4k1r1/8/5p2/8/2Q5/8/8/4K3 w - - 0 1",     c4, c8, 4 },
  };

  // ---------------------------------------------------------------------------
  // Test
  // ---------------------------------------------------------------------------

  TEST_CASE("Tactical - fast suite", "[tactical]")
  {
      auto tc = GENERATE(from_range(kFastCases));

      INFO(tc.label);
      Board::Instance().SetupFromFEN(tc.fen);
      auto ai = make_tactical_engine(tc.depth);
      GameInfo info = Board::Instance().GetGameInfo();
      Move m = ai->GetMove(info);

      REQUIRE(m.from() == tc.expected_from);
      REQUIRE(m.to()   == tc.expected_to);
  }
  ```

- [ ] **Step 2: Build tests**

  ```powershell
  .\build.ps1 tests
  ```
  Expected: clean build, zero errors.

- [ ] **Step 3: Run `[tactical]` to verify all 10 positions pass**

  ```powershell
  .\build.ps1 run-tests "[tactical]"
  ```
  Expected: 10 assertions pass in < 5 s.

  **If any position fails:** the engine did not find the expected move at depth 4 for that
  position. Replace the failing row with an alternative position from the same category
  (another M1, capture, or fork) that the engine does solve uniquely. Rerun until all 10 pass.

- [ ] **Step 4: Commit**

  ```
  git add StratChessTests/TacticalTestHelpers.h StratChessTests/TacticalTests.cpp
  git commit -m "test: refactor TacticalTests to GENERATE pattern; add shared header; expand to 10 positions"
  ```

---

## Chunk 3: Slow Tier + Project File Updates

### Task 4: Create `TacticalFullTests.cpp` + update `.vcxproj` and `.vcxproj.filters`

**Files:**
- Create: `StratChessTests/TacticalFullTests.cpp`
- Modify: `StratChessTests/StratChessTests.vcxproj:179` (add ClCompile) and `:181` (add ClInclude)
- Modify: `StratChessTests/StratChessTests.vcxproj.filters:63-66` (add Filter entries)

#### 4a — Position selection for the slow tier

The slow tier targets ~25 positions at depth 6. Categories (from the spec):
- **10 mate-in-2**: forced in both lines, no defensive resource escapes at depth 6
- **15 WAC tactical patterns**: forks, pins, discovered attacks, back-rank combos, clearances

**Selection invariant:** before adding any position to `kSlowCases`, run the engine at depth 6
and confirm it returns the expected `bestmove`. Positions the engine does not solve are excluded
from the initial commit and tracked as future additions.

**Verification script** — run from the repo root (adjust path if needed):

```powershell
# uci_verify.ps1 — verify engine finds expected bestmove for a given FEN at depth N
param([string]$Fen, [string]$ExpectedMove, [int]$Depth = 6)
$input = "uci`nisready`nposition fen $Fen`ngo depth $Depth`nquit"
$result = $input | & .\StratChessTests\x64\Release\StratChessTests.exe 2>&1
# Note: use the main engine binary for UCI smoke, not the test binary.
# Use: & ..\x64\Release\StratChessEvolved.exe  (from StratChessEvolved\ directory)
$bm = ($result | Select-String "bestmove").Line
Write-Host "FEN: $Fen"
Write-Host "Result: $bm"
Write-Host "Expected: bestmove $ExpectedMove"
```

Run the engine for each candidate FEN and keep only those where `bestmove` matches the expected move.

**Starting candidate set — mate-in-2 (10):**

These positions are straightforward forced mates; verify each at depth 6 before including:

| Label | FEN | Expected from→to | Pattern |
|-------|-----|-----------------|---------|
| M2: back-rank rook | `6k1/5ppp/8/4R3/8/8/5PPP/6K1 w - - 0 1` | Verify | Re5-e8+, Re8-h8# |
| M2: queen + rook | `r5k1/pp4pp/8/3Q4/8/8/PP4PP/6K1 w - - 0 1` | Verify | Qd8+, Qxg8# |
| M2: smothered mate setup | `6rk/6pp/8/8/8/8/5PPP/4Q1K1 w - - 0 1` | Verify | Qe8+ → smothered |
| M2: bishop + rook | `r5k1/5ppp/8/8/8/5B2/5PPP/R5K1 w - - 0 1` | Verify | Ra8+, Bg2# |
| M2: queen diagonal | `6k1/6pp/8/8/8/8/1Q4PP/6K1 w - - 0 1` | Verify | Qb2-b7+, Qg7# |
| M2: double check | `r4rk1/ppp2ppp/2n5/3p4/3P2Q1/8/PPP2PPP/R3R1K1 w - - 0 1` | Verify | Re8+, Rxf8# |
| M2: rook + queen | `2r3k1/5ppp/8/3Q4/8/8/5PPP/6K1 w - - 0 1` | Verify | Qd8+, Qxc8# |
| M2: discovered mate | `6k1/R4ppp/8/8/8/5N2/5PPP/6K1 w - - 0 1` | Verify | Nf3-g5+, Ra7-a8# |
| M2: queen + bishop | `5bk1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1` | Verify | Qd8+, Qxf8# |
| M2: rook sac | `r5k1/pp3ppp/8/3Q4/8/8/PP3PPP/6K1 w - - 0 1` | Verify | Qd8+, Qxa8# |

**Starting candidate set — WAC-style tactical patterns (15):**

These are common tactical motifs. Verify each against the engine at depth 6:

| Label | FEN | Expected from→to | Pattern |
|-------|-----|-----------------|---------|
| fork: knight queen+rook | `r2qkb1r/ppp2ppp/2n2n2/3Pp3/2B5/2N2N2/PPP2PPP/R1BQK2R w KQkq - 0 7` | Verify | Nd5 fork |
| pin + win | `r1b1kb1r/ppqn1ppp/2p1p3/3pN3/3P4/3B1N2/PPP2PPP/R1BQK2R w KQkq - 0 9` | Verify | absolute pin |
| discovered attack | `r3kb1r/ppp2ppp/2n1q3/4p3/2B1P3/2N2N2/PPP2PPP/R1BQ1RK1 w kq - 0 9` | Verify | Nd5 disc. attack |
| back-rank weakness | `5rk1/pp4pp/3p1b2/3B1p2/8/1P3Q2/P4PPP/5RK1 w - - 0 1` | Verify | Qf3-c6+ |
| hanging queen sac | `r3r1k1/pp1q1ppp/3b4/3P4/8/2N2N2/PP3PPP/2RQR1K1 w - - 0 1` | Verify | Rd1-d4 pin |
| clearance sacrifice | `r4rk1/1pp2ppp/2n2n2/q3p3/2B5/P1B1PN2/1P3PPP/R2Q1RK1 w - - 0 12` | Verify | clearance |
| exchange win | `r1bq1rk1/pp2ppbp/2np1np1/8/3NP3/2N1BP2/PPP3PP/R2QKB1R w KQ - 0 9` | Verify | Nd5 |
| pawn promotion tactic | `8/1P4k1/8/8/8/8/8/4K3 w - - 0 1` | b7→b8 | promote + attack |
| rook + queen battery | `3r2k1/pp3ppp/2n5/3Q4/8/2N5/PP3PPP/3R2K1 w - - 0 1` | Verify | battery |
| fork: bishop + threats | `r4rk1/ppp2ppp/2n5/3qp3/3P4/2PB1N2/PP3PPP/R2Q1RK1 w - - 0 1` | Verify | Bg6 fork |
| overloaded defender | `r3r1k1/pp1bqppp/2p5/3p4/3P1B2/4RN2/PPP2PPP/R2Q2K1 w - - 0 1` | Verify | overload |
| decoy tactic | `2rr2k1/pp2bppp/1qn5/3p4/3P4/1PN1BN2/P3QPPP/R2R2K1 w - - 0 1` | Verify | decoy |
| winning exchange | `r1bq1rk1/pp2ppbp/2n3p1/3pN3/3P4/2N1BP2/PP4PP/R2QKB1R w KQ - 0 10` | Verify | Nxd5 |
| pin on king | `r1b1k2r/pp1pqppp/2n1pn2/8/1bBPP3/2N1BN2/PP3PPP/R2QK2R w KQkq - 0 9` | Verify | pin tactic |
| double attack | `r3k2r/pp2qppp/2n5/3p4/3P4/2N2N2/PP2QPPP/R3K2R w KQkq - 0 1` | Verify | double attack |

> **Important:** The "Verify" entries require you to run the engine on each FEN at depth 6 and
> confirm a unique best move. Replace any position where the engine finds a different move than
> expected, or where multiple moves score equally. The goal is a set of ~25 positions where the
> engine reliably finds the single correct answer.

#### 4b — Create `TacticalFullTests.cpp`

- [ ] **Step 1: Create `StratChessTests/TacticalFullTests.cpp` with verified positions**

  Run the engine on each candidate FEN (see verification script above), then build the file with
  only positions that pass. The structure is identical to `TacticalTests.cpp` — only the tag,
  table name, depth, and positions differ:

  ```cpp
  // TacticalFullTests.cpp — slow tactical suite [tactical_full][slow]
  //
  // Runs ~25 positions at depth 6 (< 60 s total in Release).
  // Tagged [slow] — excluded from default run-tests; use extended-tests or
  // run-tests "[tactical_full]" to run explicitly.
  //
  // Selection invariant: every position in kSlowCases must have a unique best move
  // at depth 6. Verify with: go depth 6 on each FEN before committing.
  //
  // See Docs/TestDesign.md §Phase 0 for rationale.

  #include <catch_amalgamated.hpp>
  #include "TacticalTestHelpers.h"
  #include "Board.h"

  // ---------------------------------------------------------------------------
  // Position table — slow tier (depth 6, ~25 positions)
  // Fill from verified candidates. Template row shown; replace with actual FENs.
  // ---------------------------------------------------------------------------

  static constexpr TacticalCase kSlowCases[] = {
      // — Mate-in-2 (10) — replace each FEN with verified positions ————————
      // { "M2: <label>", "<fen>", <from_sq>, <to_sq>, 6 },
      // ...

      // — WAC-style patterns (15) — replace each FEN with verified positions —
      // { "WAC: <label>", "<fen>", <from_sq>, <to_sq>, 6 },
      // ...
  };

  // ---------------------------------------------------------------------------
  // Test
  // ---------------------------------------------------------------------------

  TEST_CASE("Tactical - slow suite", "[tactical_full][slow]")
  {
      auto tc = GENERATE(from_range(kSlowCases));

      INFO(tc.label);
      Board::Instance().SetupFromFEN(tc.fen);
      auto ai = make_tactical_engine(tc.depth);
      GameInfo info = Board::Instance().GetGameInfo();
      Move m = ai->GetMove(info);

      REQUIRE(m.from() == tc.expected_from);
      REQUIRE(m.to()   == tc.expected_to);
  }
  ```

  After filling in at least one verified position, build and run to confirm the framework
  works before adding all 25.

- [ ] **Step 2: Build to confirm `TacticalFullTests.cpp` compiles before project file edits**

  ```powershell
  .\build.ps1 tests
  ```
  Expected: **build error** — `TacticalFullTests.cpp` is not yet in the `.vcxproj`. This
  confirms the file exists on disk before the project file is edited.

  > Actually: the file won't be compiled until added to the project. The build will succeed but
  > the new file will be ignored. That's fine — add it to the project in the next step.

#### 4c — Update `StratChessTests.vcxproj`

- [ ] **Step 3: Add `TacticalFullTests.cpp` (ClCompile) and `TacticalTestHelpers.h` (ClInclude) to the project**

  In `StratChessTests.vcxproj`, find the `<ClCompile Include="UCITests.cpp" />` line (currently
  the last test entry, around line 178). Add immediately after it:
  ```xml
      <ClCompile Include="TacticalFullTests.cpp" />
  ```

  Find the `<ItemGroup>` containing `<ClInclude Include="..\StratEngine\Tests\Perft.h" />` (around
  line 181). Add a second ClInclude inside the same ItemGroup:
  ```xml
      <ClInclude Include="TacticalTestHelpers.h" />
  ```

  The ClInclude ItemGroup after the change:
  ```xml
  <ItemGroup>
    <ClInclude Include="..\StratEngine\Tests\Perft.h" />
    <ClInclude Include="TacticalTestHelpers.h" />
  </ItemGroup>
  ```

#### 4d — Update `StratChessTests.vcxproj.filters`

- [ ] **Step 4: Add Filter entries for both new files**

  In `StratChessTests.vcxproj.filters`, find the `<ClCompile Include="UCITests.cpp">` entry (last
  test entry, around line 63). Add immediately after its closing `</ClCompile>` tag:
  ```xml
      <ClCompile Include="TacticalFullTests.cpp">
        <Filter>Source Files\Tests</Filter>
      </ClCompile>
  ```

  Find the `<ClInclude Include="..\StratEngine\Tests\Perft.h">` entry (around line 135).
  Add immediately after its closing `</ClInclude>` tag:
  ```xml
      <ClInclude Include="TacticalTestHelpers.h">
        <Filter>Header Files</Filter>
      </ClInclude>
  ```

- [ ] **Step 5: Build tests with both new files included**

  ```powershell
  .\build.ps1 tests
  ```
  Expected: clean build. `TacticalFullTests.cpp` is now compiled.

- [ ] **Step 6: Run `[tactical_full]` to confirm the slow suite builds and runs**

  ```powershell
  .\build.ps1 run-tests "[tactical_full]"
  ```
  Expected: all verified positions in `kSlowCases` pass. Total runtime < 60 s.

- [ ] **Step 7: Confirm `[tactical]` fast suite still passes**

  ```powershell
  .\build.ps1 run-tests "[tactical]"
  ```
  Expected: 10 passes, < 5 s.

- [ ] **Step 8: Commit**

  ```
  git add StratChessTests/TacticalFullTests.cpp StratChessTests/StratChessTests.vcxproj StratChessTests/StratChessTests.vcxproj.filters
  git commit -m "test: add TacticalFullTests slow tier (~25 positions depth 6); update project files"
  ```

---

## Chunk 4: `build.ps1` Changes

### Task 5: Add `extended-tests` target; default `run-tests` to `~[slow]`

**Files:**
- Modify: `build.ps1:30` (ValidateSet), `build.ps1:104-115` (run-tests case), add `extended-tests` case

**Current state of `build.ps1`:**
- Line 30: `[ValidateSet('main', 'tests', 'all', 'run-tests')]`
- `run-tests` case (lines 104–115): passes `$Tag` if non-empty, else runs bare (all tests)

**Changes required:**
1. Add `'extended-tests'` to the `[ValidateSet(...)]` attribute (line 30)
2. In `run-tests`: when `$Tag` is empty, pass `~[slow]` instead of running bare
3. Add new `extended-tests` switch case: build tests then run without tag filter (all tests)

- [ ] **Step 1: Update `[ValidateSet]` on line 30**

  Change:
  ```powershell
  [ValidateSet('main', 'tests', 'all', 'run-tests')]
  ```
  To:
  ```powershell
  [ValidateSet('main', 'tests', 'all', 'run-tests', 'extended-tests')]
  ```

- [ ] **Step 2: Update the `run-tests` switch case (lines 104–115)**

  Replace:
  ```powershell
  'run-tests' {
      Invoke-MSBuild $TestProj -Parallel
      Write-Host ""
      $tagLabel = if ($Tag) { " ($Tag)" } else { '' }
      Write-Host "==> Running tests$tagLabel" -ForegroundColor Cyan
      if ($Tag) {
          & $TestExe $Tag
      } else {
          & $TestExe
      }
      exit $LASTEXITCODE
  }
  ```
  With:
  ```powershell
  'run-tests' {
      Invoke-MSBuild $TestProj -Parallel
      Write-Host ""
      # Default excludes [slow] tests to keep the run fast (< 10 s).
      # Use extended-tests to run everything, or pass "[tactical_full]" explicitly.
      $effectiveTag = if ($Tag) { $Tag } else { '~[slow]' }
      Write-Host "==> Running tests ($effectiveTag)" -ForegroundColor Cyan
      & $TestExe $effectiveTag
      exit $LASTEXITCODE
  }
  ```

- [ ] **Step 3: Add the `extended-tests` switch case after `run-tests`**

  Add immediately after the `run-tests` case (before the closing `}`):
  ```powershell
  'extended-tests' {
      Invoke-MSBuild $TestProj -Parallel
      Write-Host ""
      Write-Host "==> Running extended tests (all tags)" -ForegroundColor Cyan
      & $TestExe
      exit $LASTEXITCODE
  }
  ```

- [ ] **Step 4: Verify `.DESCRIPTION` block at top of `build.ps1` documents the new verb**

  Add a line to the `.DESCRIPTION` block (around line 14):
  ```powershell
  #    extended-tests  Build tests then execute all tests including [slow]
  ```

  And update the `.EXAMPLE` section to include:
  ```powershell
  #    .\\build.ps1 extended-tests
  ```

- [ ] **Step 5: Test the `run-tests` default excludes `[slow]`**

  ```powershell
  .\build.ps1 run-tests
  ```
  Expected: runs and completes quickly (< 10 s). Output should show `(~[slow])` in the header
  line. The `[tactical_full]` tests must NOT appear in output.

- [ ] **Step 6: Test explicit tag still works**

  ```powershell
  .\build.ps1 run-tests "[tactical]"
  ```
  Expected: only fast tactical tests run (10 cases).

- [ ] **Step 7: Test `extended-tests` runs everything**

  ```powershell
  .\build.ps1 extended-tests
  ```
  Expected: all tests run including `[tactical_full]`. Total runtime < 70 s.

- [ ] **Step 8: Commit**

  ```
  git add build.ps1
  git commit -m "build: run-tests defaults to ~[slow]; add extended-tests target"
  ```

---

## Chunk 5: Full Validation

- [ ] **Step 1: Full build of main solution + tests**

  ```powershell
  .\build.ps1 all
  ```
  Expected: both the main solution and test project build cleanly.

- [ ] **Step 2: Fast suite timing check**

  ```powershell
  Measure-Command { .\build.ps1 run-tests "[tactical]" }
  ```
  Expected: wall time < 5 s. All 10 cases pass.

- [ ] **Step 3: Slow suite timing check**

  ```powershell
  Measure-Command { .\build.ps1 run-tests "[tactical_full]" }
  ```
  Expected: wall time < 60 s. All slow-tier cases pass.

- [ ] **Step 4: Default `run-tests` excludes `[slow]`**

  ```powershell
  .\build.ps1 run-tests
  ```
  Expected: `[tactical_full]` tests are absent from the output. Total time < 10 s.

- [ ] **Step 5: `extended-tests` runs everything**

  ```powershell
  .\build.ps1 extended-tests
  ```
  Expected: all test suites run. Zero failures.

- [ ] **Step 6: UCI depth smoke test**

  From the `StratChessEvolved/` directory (which has `logs/` and `game_settings.json`):
  ```powershell
  # Confirm go depth 5 terminates promptly after depth 5 completes (not at 10 s)
  $commands = "uci`nisready`nposition startpos`ngo depth 5`nquit"
  $result = $commands | & ..\x64\Release\StratChessEvolved.exe
  $result | Select-String "bestmove"
  ```
  Expected: `bestmove` line appears within a few seconds (depth 5 from startpos), not after 10 s.

- [ ] **Step 7: Confirm `[uci]` parse tests still pass**

  ```powershell
  .\build.ps1 run-tests "[uci]"
  ```
  Expected: all 8 `[uci]` tests pass.

---

## Key Correctness Properties

1. `[tactical]` is always included in default `run-tests`; `[tactical_full]` never is.
2. Every position in `kFastCases` and `kSlowCases` has been verified to return the expected single best move at the target depth before committing.
3. `go depth N` (no time) terminates at depth N, not at an arbitrary 10 s cutoff.
4. `go infinite` runs until an explicit `stop` is received (or the 1-hour safety ceiling).
5. `go movetime N` and `go wtime/btime` behaviour is **unchanged** — their branches are untouched.
6. A subsequent `go` after `stop`/`quit` picks up a clean `time_limit_` (15 s default, not the previous `hours(1)`).
7. `make_tactical_engine(depth)` in tests never calls `SetClockInfo` — `StartTimer()` uses the `time_limit_ = 15 s` default, which is never the binding constraint for depth 4 or depth 6.
