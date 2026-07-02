# Tactical Suite Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand `Tests/tactical_test_cases.json` from 8 to ~30+ engine-verified positions (WAC mate-in-2/3/4 sets + non-mate tactical wins) and enforce the roadmap's "100% mate tests" acceptance criterion in the runner — step 1 of the Roadmap Near-Term Sequence.

**Architecture:** Pure content expansion of the existing JSON-driven exe tactical suite (`StratChessEvolved.exe tactical test`, gated in `Validate-PrePR.ps1` Step 3), plus two small runner changes: (a) a unit-testable suite-verdict policy that requires 100% pass in `mate*` categories, (b) an optional JSON-filename argument so candidate batches can be verified through the exact same code path as the gate before being merged.

**Tech Stack:** C++20/MSVC x64 Release, nlohmann::json, Catch2 v3, python-chess (dev-side verification only, never shipped in the binary).

## Global Constraints

- Every new position MUST be verified against the engine (via the staging-file run) before being merged into `tactical_test_cases.json` — never trust manual analysis (memory rule 3; how issue #66's class of bug is kept out).
- Every FEN MUST have all 6 fields (side/castling/ep/halfmove/fullmove) — memory rule 2.
- All candidate FENs in this plan were already legality-checked with python-chess (board.is_valid()), and all mate keys ground-truth confirmed with a forced-mate solver (see Data Provenance below). Do not add positions from other sources without repeating both checks.
- Exe suite must be run from the `Tests/` directory (`load_test_cases` resolves `current_path().parent_path()/Tests/`).
- Total gated suite runtime budget: **< 60 s** (currently < 1 s for 8 positions). If exceeded at final assembly, drop the slowest non-mate positions first, then reduce mate-in-4 tier.
- Only x64 Release is maintained. Build with `.\build.ps1` targets. PowerShell 7.
- `StratChessEvolved/game_settings.json` FEN must stay at the starting position (pre-commit gate checks it) — nothing in this plan touches it.
- Commit messages: short, single-line summary style (user preference), ending with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Data Provenance

- WAC source: `wac.epd` (300 positions) from `https://raw.githubusercontent.com/jwiegley/emacs-chess/master/wac.epd` (fetched 2026-07-02; also cached in session scratchpad).
- Candidate filter: EPD `acd` (analysis depth) ≤ 12 for mates, shallowest-`acd` non-mates with `ce` ≥ 150.
- SAN→UCI conversion: python-chess `board.parse_san()` — no manual conversion anywhere.
- Mate ground truth: every mate key below CONFIRMED by exhaustive forced-mate search (python-chess, checks-first ordering) on 2026-07-02. `Scripts/verify_mate_key.py` (added in Task 3) re-runs this check for any alternative move the engine plays.

---

### Task 1: Suite-verdict policy — 100% mate categories (TDD)

**Files:**
- Modify: `StratEngine/Tests/TacticalTestRunner.h`
- Modify: `StratEngine/Tests/TacticalTestRunner.cpp`
- Modify: `StratChessTests/StratChessTests.vcxproj` (add TacticalTestRunner.cpp)
- Modify: `StratChessTests/StratChessTests.vcxproj.filters` (same entry)
- Create: `StratChessTests/SuitePolicyTests.cpp`

**Interfaces:**
- Consumes: existing `TacticalResult` (id, description, engine_move_uci, passed, time_ms).
- Produces (later tasks and the gate rely on these exact names):
  ```cpp
  // in TacticalResult:
  std::string category;   // copied from TacticalPosition by run_position

  // new in TacticalTestRunner:
  struct SuiteVerdict {
      bool ok = false;
      int passed = 0;
      int total = 0;
      double pass_rate = 0.0;
      std::vector<std::string> failed_mate_ids;  // ids of failed positions in mate* categories
  };
  [[nodiscard]] static SuiteVerdict evaluate_results(
      const std::vector<TacticalResult>& results, double required_pass_rate);
  ```
  Policy: `ok` is true iff `total > 0` AND `pass_rate >= required_pass_rate` AND `failed_mate_ids.empty()`. A category is a mate category iff it starts with `"mate"` (covers existing `mate_in_1`, `mate_in_2` and new `mate_in_3`, `mate_in_4`).

- [ ] **Step 1: Commit this plan file**

```bash
git add .claude/plans/tactical-suite-expansion.md
git commit -m "Add tactical suite expansion plan

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 2: Add TacticalTestRunner.cpp to the test project**

In `StratChessTests/StratChessTests.vcxproj`, next to the existing `<ClCompile Include="..\StratEngine\Tests\Perft.cpp" />` entry, add:

```xml
    <ClCompile Include="..\StratEngine\Tests\TacticalTestRunner.cpp" />
```

In `StratChessTests/StratChessTests.vcxproj.filters`, mirror however `..\StratEngine\Tests\Perft.cpp` is filtered there (same filter group, same element shape).

- [ ] **Step 3: Write the failing tests**

Create `StratChessTests/SuitePolicyTests.cpp`:

```cpp
// SuitePolicyTests.cpp — Catch2 tests for TacticalTestRunner::evaluate_results()
//
// Roadmap acceptance criterion for the tactical suite: pass 80%+ tactical
// tests, 100% mate tests. evaluate_results enforces: overall pass rate >=
// required threshold AND zero failures in any category starting with "mate".

#include <catch_amalgamated.hpp>
#include "Tests/TacticalTestRunner.h"

using Testing::TacticalResult;
using Testing::TacticalTestRunner;

static TacticalResult make_result(const char* id, const char* category, bool passed)
{
    TacticalResult r;
    r.id = id;
    r.category = category;
    r.passed = passed;
    return r;
}

TEST_CASE("all positions passing yields ok", "[suite_policy]")
{
    std::vector<TacticalResult> results = {
        make_result("M1-001", "mate_in_1", true),
        make_result("WAC-001", "mate_in_2", true),
        make_result("HANG-001", "hanging_piece", true),
    };
    const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
    CHECK(v.ok);
    CHECK(v.passed == 3);
    CHECK(v.total == 3);
    CHECK(v.failed_mate_ids.empty());
}

TEST_CASE("one mate failure fails the suite even above the overall threshold", "[suite_policy]")
{
    // 19/20 = 95% overall — above 90% — but the failed position is a mate.
    std::vector<TacticalResult> results;
    for (int i = 0; i < 19; ++i)
        results.push_back(make_result("T", "tactical_win", true));
    results.push_back(make_result("WAC-004", "mate_in_2", false));

    const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
    CHECK_FALSE(v.ok);
    REQUIRE(v.failed_mate_ids.size() == 1);
    CHECK(v.failed_mate_ids[0] == "WAC-004");
}

TEST_CASE("non-mate failures within the threshold still pass", "[suite_policy]")
{
    // 9/10 = 90%, failure is non-mate -> ok at 0.90 threshold.
    std::vector<TacticalResult> results;
    for (int i = 0; i < 9; ++i)
        results.push_back(make_result("T", "tactical_win", true));
    results.push_back(make_result("T-FAIL", "tactical_win", false));

    const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
    CHECK(v.ok);
    CHECK(v.failed_mate_ids.empty());
}

TEST_CASE("non-mate failures below the threshold fail", "[suite_policy]")
{
    // 8/10 = 80% < 90%.
    std::vector<TacticalResult> results;
    for (int i = 0; i < 8; ++i)
        results.push_back(make_result("T", "tactical_win", true));
    results.push_back(make_result("T-FAIL1", "tactical_win", false));
    results.push_back(make_result("T-FAIL2", "tactical_win", false));

    const auto v = TacticalTestRunner::evaluate_results(results, 0.90);
    CHECK_FALSE(v.ok);
}

TEST_CASE("empty result set fails safe", "[suite_policy]")
{
    const auto v = TacticalTestRunner::evaluate_results({}, 0.90);
    CHECK_FALSE(v.ok);
    CHECK(v.total == 0);
}
```

Add `SuitePolicyTests.cpp` to `StratChessTests.vcxproj` and `.filters` next to the other `*Tests.cpp` entries.

- [ ] **Step 4: Run tests to verify they fail to compile**

```powershell
.\build.ps1 tests
```
Expected: **build FAILS** — `category` is not a member of `TacticalResult`, `evaluate_results`/`SuiteVerdict` undeclared.

- [ ] **Step 5: Implement**

`StratEngine/Tests/TacticalTestRunner.h` — add `category` to `TacticalResult` (after `id`):

```cpp
struct TacticalResult {
    std::string id;
    std::string category;
    std::string description;
    std::string engine_move_uci;
    bool passed = false;
    int64_t time_ms = 0;
};
```

and add to the `TacticalTestRunner` class (public section, above `run_test_suite`):

```cpp
    struct SuiteVerdict {
        bool ok = false;
        int passed = 0;
        int total = 0;
        double pass_rate = 0.0;
        std::vector<std::string> failed_mate_ids;
    };

    // Pure verdict policy (unit-tested in SuitePolicyTests.cpp):
    // ok iff total > 0, pass_rate >= required_pass_rate, and no failures
    // in any category whose name starts with "mate".
    [[nodiscard]] static SuiteVerdict evaluate_results(
        const std::vector<TacticalResult>& results, double required_pass_rate);
```

`StratEngine/Tests/TacticalTestRunner.cpp`:

In `run_position`, after `result.id = pos.id;` add:

```cpp
    result.category    = pos.category;
```

Add the new method:

```cpp
TacticalTestRunner::SuiteVerdict TacticalTestRunner::evaluate_results(
    const std::vector<TacticalResult>& results, double required_pass_rate)
{
    SuiteVerdict v;
    v.total = static_cast<int>(results.size());
    for (const auto& r : results) {
        if (r.passed)
            ++v.passed;
        else if (r.category.rfind("mate", 0) == 0)
            v.failed_mate_ids.push_back(r.id);
    }
    v.pass_rate = (v.total > 0) ? static_cast<double>(v.passed) / v.total : 0.0;
    v.ok = v.total > 0
        && v.pass_rate >= required_pass_rate
        && v.failed_mate_ids.empty();
    return v;
}
```

Rewrite the tail of `run_test_suite` (from `if (result.passed) ++passed; else ++failed;` through the return) to collect results and delegate:

```cpp
        results.push_back(result);
    }

    const SuiteVerdict v = evaluate_results(results, required_pass_rate);

    std::cout << "========================================\n";
    std::cout << "Results: " << v.passed << "/" << v.total << " passed"
              << " (" << static_cast<int>(v.pass_rate * 100) << "%)\n";
    std::cout << "Required: " << static_cast<int>(required_pass_rate * 100)
              << "% overall, 100% in mate categories\n";
    if (!v.failed_mate_ids.empty()) {
        std::cout << "Mate-category failures (always fatal):";
        for (const auto& id : v.failed_mate_ids) std::cout << " " << id;
        std::cout << "\n";
    }
    std::cout << (v.ok ? "PASS" : "FAIL") << "\n";
    std::cout << "========================================\n\n";

    return v.ok;
```

(declare `std::vector<TacticalResult> results;` before the loop, replacing the `int passed = 0; int failed = 0;` counters).

- [ ] **Step 6: Run tests to verify they pass**

```powershell
.\build.ps1 tests
StratChessTests\x64\Release\StratChessTests.exe "[suite_policy]"
```
Expected: `All tests passed` (5 test cases). Then the full fast suite:

```powershell
StratChessTests\x64\Release\StratChessTests.exe "~[slow]"
```
Expected: all pass (was 132 test cases / 1563 assertions at baseline; now 137 cases).

- [ ] **Step 7: Verify the exe gate still passes (8/8)**

```powershell
.\build.ps1 game
cd Tests
..\x64\Release\StratChessEvolved.exe tactical test
cd ..
```
Expected: `Results: 8/8 passed (100%)` … `PASS`.

- [ ] **Step 8: Commit**

```bash
git add StratEngine/Tests/TacticalTestRunner.h StratEngine/Tests/TacticalTestRunner.cpp StratChessTests/SuitePolicyTests.cpp StratChessTests/StratChessTests.vcxproj StratChessTests/StratChessTests.vcxproj.filters
git commit -m "Tactical runner: enforce 100% pass in mate categories

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: `tactical test [filename]` — staging-file support

**Files:**
- Modify: `StratEngine/Tests/TacticalTestRunner.h`
- Modify: `StratEngine/Tests/TacticalTestRunner.cpp`
- Modify: `StratChessEvolved/StratChessEvolved.cpp:105-117` (`tacticalrunner`)

**Interfaces:**
- Consumes: Task 1's runner.
- Produces: `run_test_suite(double required_pass_rate = 0.90, bool verbose = true, const std::string& json_filename = "tactical_test_cases.json")` and CLI `tactical test [filename]`. Tasks 3–6 verify candidate batches with `..\x64\Release\StratChessEvolved.exe tactical test tactical_staging.json`.

- [ ] **Step 1: Extend the signatures**

`TacticalTestRunner.h`:

```cpp
    [[nodiscard]] static bool run_test_suite(double required_pass_rate = 0.90,
                                             bool verbose = true,
                                             const std::string& json_filename = "tactical_test_cases.json");
```

`TacticalTestRunner.cpp` — matching definition; replace the hardcoded load with:

```cpp
    auto positions = load_test_cases(json_filename);
```

and include the filename in the banner:

```cpp
    std::cout << "Tactical Test Suite (" << positions.size() << " positions, "
              << json_filename << ")\n";
```

`StratChessEvolved/StratChessEvolved.cpp`, in `tacticalrunner` replace the `command == "test"` branch body:

```cpp
    if (command == "test") {
        const std::string filename = (argc >= 3) ? argv[2] : "tactical_test_cases.json";
        const bool ok = Testing::TacticalTestRunner::run_test_suite(0.90, true, filename);
        return ok ? 0 : 1;
    }
```

and update both usage strings to `"Usage: tactical test [filename]\n"`.

- [ ] **Step 2: Build and verify both invocations**

```powershell
.\build.ps1 all
cd Tests
..\x64\Release\StratChessEvolved.exe tactical test
```
Expected: 8/8 PASS, banner names `tactical_test_cases.json`.

```powershell
..\x64\Release\StratChessEvolved.exe tactical test no_such_file.json
cd ..
```
Expected: `ERROR: Cannot open no_such_file.json`, non-zero exit (`$LASTEXITCODE` ≠ 0 — the gate's failure path still works for a bad filename).

- [ ] **Step 3: Run fast Catch2 suite (regression)**

```powershell
StratChessTests\x64\Release\StratChessTests.exe "~[slow]"
```
Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add StratEngine/Tests/TacticalTestRunner.h StratEngine/Tests/TacticalTestRunner.cpp StratChessEvolved/StratChessEvolved.cpp
git commit -m "Tactical runner: optional JSON filename for staging batches

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Mate-in-2 batch (8 WAC positions, depth 5) + verifier script

**Files:**
- Create: `StratChessEvolved/Scripts/verify_mate_key.py`
- Create (transient): `Tests/tactical_staging.json` (deleted at end of Task 6)
- Modify: `Tests/tactical_test_cases.json`

**Interfaces:**
- Consumes: Task 2's `tactical test tactical_staging.json`.
- Produces: 6–8 new `mate_in_2` entries in `tactical_test_cases.json`; `verify_mate_key.py "FEN" uci_move N` (exit 0 = move forces mate in ≤ N full moves) used by Tasks 4–5 too.

- [ ] **Step 1: Add the mate-key verifier script**

Create `StratChessEvolved/Scripts/verify_mate_key.py`:

```python
"""Verify that a move forces checkmate within N full moves.

Usage:    python verify_mate_key.py "<FEN>" <uci_move> <N>
Exit 0 and prints CONFIRMED if the move forces mate in <= N moves,
exit 1 and prints REFUTED otherwise.

Requires: pip install python-chess
Purpose:  ground-truth check when the engine answers a tactical-suite mate
position with a move other than the EPD key — CONFIRMED alternatives may be
added to best_moves; REFUTED ones may not (memory rule: verify against
engine/ground truth, never trust manual analysis).
"""
import sys
import chess


def exists_forcing(board, n):
    # Side to move has some move forcing mate in <= n (checks-first ordering
    # prunes hard: mate keys are almost always checks).
    moves = sorted(board.legal_moves, key=lambda m: not board.gives_check(m))
    return any(forces(board, m, n) for m in moves)


def forces(board, mv, n):
    board.push(mv)
    try:
        if board.is_checkmate():
            return True
        if n <= 1 or board.is_game_over():
            return False
        for reply in list(board.legal_moves):
            board.push(reply)
            try:
                if not exists_forcing(board, n - 1):
                    return False
            finally:
                board.pop()
        return True
    finally:
        board.pop()


if __name__ == "__main__":
    fen, uci, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
    board = chess.Board(fen)
    ok = forces(board, chess.Move.from_uci(uci), n)
    print(f"{'CONFIRMED' if ok else 'REFUTED'}: {uci} mate<={n} in {fen}")
    sys.exit(0 if ok else 1)
```

- [ ] **Step 2: Create the staging file**

Create `Tests/tactical_staging.json` (all keys pre-confirmed by forced-mate search; `w`/`b` per FEN — three are Black to move):

```json
{
  "tactical_test_cases": [
    { "id": "WAC-001", "category": "mate_in_2", "description": "WAC.001 Qg6: queen sac decoy, Nxg6# follows", "fen": "2rr3k/pp3pp1/1nnqbN1p/3pN3/2pP4/2P3Q1/PPB4P/R4RK1 w - - 0 1", "best_moves": ["g3g6"], "depth": 5 },
    { "id": "WAC-004", "category": "mate_in_2", "description": "WAC.004 Qxh7+: queen sac, hxg6# follows", "fen": "r1bq2rk/pp3pbp/2p1p1pQ/7P/3P4/2PB1N2/PP3PPR/2KR4 w - - 0 1", "best_moves": ["h6h7"], "depth": 5 },
    { "id": "WAC-005", "category": "mate_in_2", "description": "WAC.005 Qc4+ (Black): deflection, bxc4# follows", "fen": "5k2/6pp/p1qN4/1p1p4/3P4/2PKP2Q/PP3r2/3R4 b - - 0 1", "best_moves": ["c6c4"], "depth": 5 },
    { "id": "WAC-012", "category": "mate_in_2", "description": "WAC.012 Qxf3+ (Black): queen sac on f3, Rg1# follows", "fen": "4k1r1/2p3r1/1pR1p3/3pP2p/3P2qP/P4N2/1PQ4P/5R1K b - - 0 1", "best_moves": ["g4f3"], "depth": 5 },
    { "id": "WAC-027", "category": "mate_in_2", "description": "WAC.027 Qf8+: deflect the knight, back-rank mate", "fen": "7k/pp4np/2p3p1/3pN1q1/3P4/Q7/1r3rPP/2R2RK1 w - - 0 1", "best_moves": ["a3f8"], "depth": 5 },
    { "id": "WAC-054", "category": "mate_in_2", "description": "WAC.054 Qh1+ (Black): corner check, mate on h-file", "fen": "r3kr2/1pp4p/1p1p4/7q/4P1n1/2PP2Q1/PP4P1/R1BB2K1 b q - 0 1", "best_moves": ["h5h1"], "depth": 5 },
    { "id": "WAC-099", "category": "mate_in_2", "description": "WAC.099 Rh5: rook lift, gxh5 Qxh5#", "fen": "r1bq1r1k/1pp1Np1p/p2p2pQ/4R3/n7/8/PPPP1PPP/R1B3K1 w - - 0 1", "best_moves": ["e5h5"], "depth": 5 },
    { "id": "WAC-246", "category": "mate_in_2", "description": "WAC.246 Qh5+: king hunt on the h-file", "fen": "6R1/4qp1p/ppr1n1pk/8/1P2P1QP/6N1/P4PP1/6K1 w - - 0 1", "best_moves": ["g4h5"], "depth": 5 }
  ]
}
```

- [ ] **Step 3: Run the staging batch through the real gate path**

```powershell
cd Tests
..\x64\Release\StratChessEvolved.exe tactical test tactical_staging.json
cd ..
```
Expected: verbose per-position output with engine move, expected move, verdict, ms.

- [ ] **Step 4: Reconcile failures (repeat until stable)**

For each FAIL where the engine played move `X` ≠ key:

```powershell
python StratChessEvolved\Scripts\verify_mate_key.py "<that FEN>" X 2
```

- `CONFIRMED` → engine found an equally fast mate: add `X` to that position's `best_moves`, keep the position.
- `REFUTED` → engine is wrong at this depth: retry the position with `"depth": 6` in the staging file; if the engine still plays a refuted move at depth 6, **delete the position** and note the id + engine move in the commit body (candidate for a future GitHub issue if a pattern emerges).

Re-run Step 3 after each edit. Done when the staging run reports 100%.

- [ ] **Step 5: Merge survivors into the real suite**

Append the surviving entries to the `tactical_test_cases` array in `Tests/tactical_test_cases.json` (after `M2-001`, keeping the existing 8 entries untouched). Then:

```powershell
cd Tests
..\x64\Release\StratChessEvolved.exe tactical test
cd ..
```
Expected: `Results: N/N passed (100%) … PASS` where N = 8 + survivors (target ≥ 14).

- [ ] **Step 6: Commit**

```bash
git add Tests/tactical_test_cases.json StratChessEvolved/Scripts/verify_mate_key.py
git commit -m "Tactical suite: add WAC mate-in-2 batch (engine-verified)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

(Do NOT commit `Tests/tactical_staging.json` — it stays a scratch file until deleted in Task 6.)

---

### Task 4: Mate-in-3 batch (8 WAC positions, depth 6)

**Files:**
- Modify (transient): `Tests/tactical_staging.json` (replace contents)
- Modify: `Tests/tactical_test_cases.json`

**Interfaces:**
- Consumes: Task 2 staging runs, Task 3's `verify_mate_key.py` (with N=3).
- Produces: 6–8 new `mate_in_3` entries.

- [ ] **Step 1: Replace staging file contents**

```json
{
  "tactical_test_cases": [
    { "id": "WAC-050", "category": "mate_in_3", "description": "WAC.050 Rxb6+: rook sac rips the king shelter", "fen": "k4r2/1R4pb/1pQp1n1p/3P4/5p1P/3P2P1/r1q1R2K/8 w - - 0 1", "best_moves": ["b7b6"], "depth": 6 },
    { "id": "WAC-057", "category": "mate_in_3", "description": "WAC.057 Rf8+: deflection into back-rank mate", "fen": "r3q1kr/ppp5/3p2pQ/8/3PP1b1/5R2/PPP3P1/5RK1 w - - 0 1", "best_moves": ["f3f8"], "depth": 6 },
    { "id": "WAC-064", "category": "mate_in_3", "description": "WAC.064 g4+: pawn check starts king hunt", "fen": "8/6pp/3q1p2/3n1k2/1P6/3NQ2P/5PP1/6K1 w - - 0 1", "best_moves": ["g2g4"], "depth": 6 },
    { "id": "WAC-079", "category": "mate_in_3", "description": "WAC.079 Qxh2+ (Black): queen sac on h2, king hunt", "fen": "r3k2r/pbp2pp1/3b1n2/1p6/3P3p/1B2N1Pq/PP1PQP1P/R1B2RK1 b kq - 0 1", "best_moves": ["h3h2"], "depth": 6 },
    { "id": "WAC-097", "category": "mate_in_3", "description": "WAC.097 Qa8+: long-diagonal switchback mate", "fen": "6k1/5p2/p5np/4B3/3P4/1PP1q3/P3r1QP/6RK w - - 0 1", "best_moves": ["g2a8"], "depth": 6 },
    { "id": "WAC-136", "category": "mate_in_3", "description": "WAC.136 Rc8+: back-rank breakthrough", "fen": "6kr/1q2r1p1/1p2N1Q1/5p2/1P1p4/6R1/7P/2R3K1 w - - 0 1", "best_moves": ["c1c8"], "depth": 6 },
    { "id": "WAC-173", "category": "mate_in_3", "description": "WAC.173 Qh6+: queen invades, mate on the h-file/g7", "fen": "2r1b3/1pp1qrk1/p1n1P1p1/7R/2B1p3/4Q1P1/PP3PP1/3R2K1 w - - 0 1", "best_moves": ["e3h6"], "depth": 6 },
    { "id": "WAC-197", "category": "mate_in_3", "description": "WAC.197 Qf1+ (Black): deflection, promotion mate follows", "fen": "7k/1p4p1/7p/3P1n2/4Q3/2P2P2/PP3qRP/7K b - - 0 1", "best_moves": ["f2f1"], "depth": 6 }
  ]
}
```

- [ ] **Step 2: Run staging, reconcile, merge**

Same loop as Task 3 Steps 3–5, with `verify_mate_key.py "<FEN>" X 3` for alternatives and depth escalation to 7 (not further) before dropping. Merge survivors into `tactical_test_cases.json`, re-run the full `tactical test`, expect 100%.

- [ ] **Step 3: Commit**

```bash
git add Tests/tactical_test_cases.json
git commit -m "Tactical suite: add WAC mate-in-3 batch (engine-verified)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Mate-in-4 stretch tier (4 WAC positions, depth 7)

**Files:**
- Modify (transient): `Tests/tactical_staging.json`
- Modify: `Tests/tactical_test_cases.json`

**Interfaces:**
- Consumes: staging runs + `verify_mate_key.py` N=4 (slower — up to ~1 min per check is normal).
- Produces: 0–4 `mate_in_4` entries. This tier is optional: any position slower than **2000 ms** per run or failing at depth 8 is dropped without ceremony.

- [ ] **Step 1: Replace staging file contents**

```json
{
  "tactical_test_cases": [
    { "id": "WAC-035", "category": "mate_in_4", "description": "WAC.035 Rxh7+: rook sac opens the h-file", "fen": "r3r2k/2R3pp/pp1q1p2/8/3P3R/7P/PP3PP1/3Q2K1 w - - 0 1", "best_moves": ["h4h7"], "depth": 7 },
    { "id": "WAC-139", "category": "mate_in_4", "description": "WAC.139 Nf6+: knight fork-check starts mating attack", "fen": "rnb3kr/ppp2ppp/1b6/3q4/3pN3/Q4N2/PPP2KPP/R1B1R3 w - - 0 1", "best_moves": ["e4f6"], "depth": 7 },
    { "id": "WAC-161", "category": "mate_in_4", "description": "WAC.161 Qxd8+: rook grab with forced mate behind it", "fen": "3r3k/3r1P1p/pp1Nn3/2pp4/7Q/6R1/Pq4PP/5RK1 w - - 0 1", "best_moves": ["h4d8"], "depth": 7 },
    { "id": "WAC-282", "category": "mate_in_4", "description": "WAC.282 Rh8+: rook sac, knight-and-bishop mating net", "fen": "6k1/2p3p1/1p1p1nN1/1B1P4/4PK2/8/2r3b1/7R w - - 0 1", "best_moves": ["h1h8"], "depth": 7 }
  ]
}
```

- [ ] **Step 2: Run staging, reconcile, merge**

Same loop; escalation ladder: depth 7 → depth 8 → drop. Alternative engine moves checked with `verify_mate_key.py "<FEN>" X 4`. Watch per-position ms in the verbose output — drop anything > 2000 ms even if passing.

- [ ] **Step 3: Commit**

```bash
git add Tests/tactical_test_cases.json
git commit -m "Tactical suite: add WAC mate-in-4 stretch tier

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Non-mate tactical batch (11 candidates, depth 6)

**Files:**
- Modify (transient, then delete): `Tests/tactical_staging.json`
- Modify: `Tests/tactical_test_cases.json`

**Interfaces:**
- Consumes: staging runs.
- Produces: ≥ 6 surviving `tactical_win`/`endgame_win` entries. Acceptance here is **stricter** than the mate tiers: the engine's move must equal an EPD `bm` move — there is no solver to bless alternatives, and rationalizing "also looks winning" is exactly what memory rule 3 forbids. If the engine plays anything else: try depth 7; still different → drop.

- [ ] **Step 1: Replace staging file contents**

(`ce` = EPD centipawn eval, for context in review. WAC-043 and WAC-041 have two EPD-sanctioned moves; both listed.)

```json
{
  "tactical_test_cases": [
    { "id": "WAC-209", "category": "tactical_win", "description": "WAC.209 Rxe5+: exchange sac wins decisive material (ce +1696)", "fen": "4kb1r/2q2p2/r2p4/pppBn1B1/P6P/6Q1/1PP5/2KRR3 w - - 0 1", "best_moves": ["e1e5"], "depth": 6 },
    { "id": "WAC-043", "category": "tactical_win", "description": "WAC.043 Be7/Qxa8: skewer or direct rook win (ce +1909)", "fen": "r2q3k/p2P3p/1p3p2/3QP1r1/8/B7/P5PP/2R3K1 w - - 0 1", "best_moves": ["a3e7", "d5a8"], "depth": 6 },
    { "id": "WAC-124", "category": "tactical_win", "description": "WAC.124 g3 (Black): passed-pawn breakthrough (ce +431)", "fen": "6k1/3r4/2R5/P5P1/1P4p1/8/4rB2/6K1 b - - 0 1", "best_moves": ["g4g3"], "depth": 6 },
    { "id": "WAC-148", "category": "tactical_win", "description": "WAC.148 Rxg7: rook sac destroys king cover (ce +2296)", "fen": "2r1k3/6pr/p1nBP3/1p3p1p/2q5/2P5/P1R4P/K2Q2R1 w - - 0 1", "best_moves": ["g1g7"], "depth": 6 },
    { "id": "WAC-287", "category": "tactical_win", "description": "WAC.287 Qh5: mating attack on f7/h7 wins material (ce +751)", "fen": "rn3k1r/pp2bBpp/2p2n2/q5N1/3P4/1P6/P1P3PP/R1BQ1RK1 w - - 0 1", "best_moves": ["d1h5"], "depth": 6 },
    { "id": "WAC-065", "category": "tactical_win", "description": "WAC.065 Ne7+: royal fork setup (ce +1316)", "fen": "1r1r1qk1/p2n1p1p/bp1Pn1pQ/2pNp3/2P2P1N/1P5B/P6P/3R1RK1 w - - 0 1", "best_moves": ["d5e7"], "depth": 6 },
    { "id": "WAC-082", "category": "tactical_win", "description": "WAC.082 Bh7+: discovered attack wins the queen (ce +814)", "fen": "3rr1k1/pp3pp1/4b3/8/2P1B2R/6QP/P3q1P1/5R1K w - - 0 1", "best_moves": ["e4h7"], "depth": 6 },
    { "id": "WAC-085", "category": "tactical_win", "description": "WAC.085 Na6: quiet knight move, mating net + material (ce +1774)", "fen": "kr2R3/p4r2/2pq4/2N2p1p/3P2p1/Q5P1/5P1P/5BK1 w - - 0 1", "best_moves": ["c5a6"], "depth": 6 },
    { "id": "WAC-240", "category": "tactical_win", "description": "WAC.240 Qxc6: queen grabs, tactics hold it (ce +1786)", "fen": "2b4k/p1b2p2/2p2q2/3p1PNp/3P2R1/3B4/P1Q2PKP/4r3 w - - 0 1", "best_moves": ["c2c6"], "depth": 6 },
    { "id": "WAC-045", "category": "tactical_win", "description": "WAC.045 Qxa1 (Black): back-rank pin wins the rook (ce +778)", "fen": "7k/2p1b1pp/8/1p2P3/1P3r2/2P3Q1/1P5P/R4qBK b - - 0 1", "best_moves": ["f1a1"], "depth": 6 },
    { "id": "WAC-041", "category": "endgame_win", "description": "WAC.041 Ka5/Kc5: K+R+2P vs K+R, king walk wins (zugzwang-adjacent, NMP guard coverage; ce +1608)", "fen": "1k6/5RP1/1P6/1K6/6r1/8/8/8 w - - 0 1", "best_moves": ["b5a5", "b5c5"], "depth": 6 }
  ]
}
```

- [ ] **Step 2: Run staging, reconcile, merge**

Same staging loop; escalation: depth 6 → depth 7 → drop (no solver blessing for alternatives). Note dropped ids + engine moves in the commit body. Merge survivors, re-run full `tactical test`, expect 100%.

- [ ] **Step 3: Delete the staging file**

```powershell
Remove-Item Tests\tactical_staging.json -Confirm:$false
```

- [ ] **Step 4: Commit**

```bash
git add Tests/tactical_test_cases.json
git commit -m "Tactical suite: add WAC non-mate tactical batch

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Final assembly — budget check, full gates, docs

**Files:**
- Modify: `Docs/TestDesign.md` (tactical-suite section: position list, count, policy)
- Modify: `Docs/Roadmap.md` (Near-Term Sequence step 1 + "Create Automated Test Suite" item status)
- Possibly modify: `Tests/tactical_test_cases.json` (only if over budget)

**Interfaces:**
- Consumes: everything above.
- Produces: the finished PR branch.

- [ ] **Step 1: Timing budget check**

```powershell
cd Tests
..\x64\Release\StratChessEvolved.exe tactical test
cd ..
```
Sum the per-position ms from the verbose output (or wall-clock the run). Budget: **< 60 s total**. If over: drop the slowest non-mate positions first, then trim the mate-in-4 tier; re-run until under budget. Record the final total runtime for the docs.

- [ ] **Step 2: Full pre-PR validation**

```powershell
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PreCommit.ps1"
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1"
```
Expected: both PASS (Pre-PR runs full build + extended tests + this expanded tactical suite + self-play).
Prerequisite reminder: `game_settings.json` must have `"type": 6` for both players for the self-play step.

- [ ] **Step 3: Update docs**

`Docs/TestDesign.md` — in the "Full tactical suite in main executable" section: update position count and the position list (id, one-line description, depth per entry — same format as the existing 8), document the two policy changes (mate categories require 100%; `tactical test [filename]` staging support), and note total suite runtime. Replace the "Expansion note" paragraph (the expansion has now happened) with a short "Growing the suite" paragraph: candidates go through `tactical_staging.json` + `verify_mate_key.py`, never straight into the gated file.

`Docs/Roadmap.md` —
- Near-Term Sequence step 1: mark done, e.g. append "— ✅ done (July 2026, PR #NN)".
- "Create Automated Test Suite" item: move the near-term-slice bullet into a ✅ line with the final position count; leave the Deferred and Ongoing bullets.

- [ ] **Step 4: Commit docs**

```bash
git add Docs/TestDesign.md Docs/Roadmap.md
git commit -m "Docs: tactical suite expansion status + test design update

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Push branch and open PR (master → main workflow)**

Branch `worktree-tactical-suite-expansion` was cut fresh from `origin/main`, so it can be PR'd directly:

```bash
git push -u origin worktree-tactical-suite-expansion
gh pr create --base main --title "Tactical suite expansion: WAC mate + tactical batches, 100% mate policy" --body "Step 1 of the Roadmap Near-Term Sequence.

- ~2x-4x more gated tactical positions (WAC mate-in-2/3/4 + non-mate tactical wins), every one engine-verified via the staging path
- Runner: mate categories now require 100% pass (roadmap acceptance criterion); [suite_policy] unit tests
- Runner: 'tactical test [filename]' staging support so future candidates are verified through the exact gate code path
- Scripts/verify_mate_key.py: ground-truth forced-mate checker for reconciling alternative engine moves

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

Reviewer gate (per CLAUDE.md / memory): the runner changes touch search-adjacent test infrastructure but not `pvs()`/`qsearch()`/move ordering/eval — if any search or eval source file ends up modified during execution (it should not), dispatch search-reviewer/eval-reviewer before the PR.

- [ ] **Step 6: Report** — final position count by category, dropped candidates (id + reason), total suite runtime, PR link. User merges via GitHub web UI; after merge, fast-forward `master` to `origin/main` and push.

---

## Attrition Reference (what "done" means)

| Tier | Candidates | Target survivors | Depth | Alternative-move policy |
|------|-----------|------------------|-------|------------------------|
| mate_in_2 | 8 | ≥ 6 | 5 (→6) | accept if `verify_mate_key N=2` CONFIRMED |
| mate_in_3 | 8 | ≥ 6 | 6 (→7) | accept if `verify_mate_key N=3` CONFIRMED |
| mate_in_4 | 4 | ≥ 0 (optional) | 7 (→8) | accept if `verify_mate_key N=4` CONFIRMED; drop if > 2000 ms |
| tactical_win / endgame_win | 11 | ≥ 6 | 6 (→7) | none — engine move must match EPD bm |

Existing 8 positions are untouched. Worst case ≥ 26 total gated positions; realistic ~30–36.

## Self-Review (done at plan time)

- Spec coverage: WAC subset ✅ (Tasks 3–6, 31 candidates), mate-in-2/3 set ✅ (Tasks 3–4), 100%-mate acceptance ✅ (Task 1), regression-position workflow documented ✅ (Task 7 docs), roadmap status update ✅ (Task 7).
- Placeholders: none — all FENs, UCI moves, code, and commands are concrete.
- Type consistency: `SuiteVerdict`/`evaluate_results`/`category` names match across Task 1 header, impl, and tests; `run_test_suite` 3-arg signature matches Task 2 caller; staging filename `tactical_staging.json` consistent across Tasks 2–6.
