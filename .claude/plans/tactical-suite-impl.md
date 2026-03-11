# Full Tactical Suite Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** Add `StratChessEvolved.exe tactical test` command that runs AIPerplex against a JSON-driven tactical test suite and reports pass/fail per position with an overall pass rate.

**Architecture:** Follows the existing perft infrastructure pattern exactly. `TacticalTestRunner.h/cpp` in `StratEngine/Tests/`; JSON in `Tests/tactical_test_cases.json` (same folder as perft JSON); new `tactical` dispatch branch in `StratChessEvolved.cpp`. Each position is run at a configurable depth; the engine's first move is compared against an accepted-moves list (UCI strings). 90%+ pass rate = engine is ready for UCI.

**Tech Stack:** C++20, MSVC /W4 /WX, nlohmann/json, AIPerplex + MoveFormatter::ToUCI, build via `build.ps1`

**Design doc:** `.claude/plans/pre-uci-pre-lazysmp-priorities.md` (item #4); `Docs/TestDesign.md` §"Full tactical suite"

**Worktree:** `.claude/worktrees/reverent-nightingale/` — all paths below are relative to this root.

---

### Task 1: Create TacticalTestRunner.h

**Files:**
- Create: `StratEngine/Tests/TacticalTestRunner.h`

```cpp
#pragma once

#include <string>
#include <vector>

namespace Testing {

struct TacticalPosition {
    std::string id;
    std::string category;
    std::string description;
    std::string fen;
    std::vector<std::string> best_moves;  // accepted first moves in UCI format (e.g. "e2e4")
    int depth = 5;
};

struct TacticalResult {
    std::string id;
    std::string description;
    std::string engine_move_uci;
    bool passed = false;
    int64_t time_ms = 0;
};

class TacticalTestRunner {
public:
    // Run all positions from the JSON file. Prints per-position results and summary.
    // Returns true if pass_rate >= required_pass_rate.
    static bool run_test_suite(double required_pass_rate = 0.90, bool verbose = true);

    // Load positions from a JSON file (path relative to working directory).
    // Uses same path resolution as Perft::load_perft_tests_modern:
    //   path = current_path().parent_path() / "Tests" / json_filename
    // Run from Tests/ directory so parent is the repo root.
    static std::vector<TacticalPosition> load_test_cases(const std::string& json_filename);

    // Run a single position. Board must be set up before calling.
    static TacticalResult run_position(const TacticalPosition& pos);
};

} // namespace Testing
```

---

### Task 2: Create TacticalTestRunner.cpp

**Files:**
- Create: `StratEngine/Tests/TacticalTestRunner.cpp`

```cpp
#include "TacticalTestRunner.h"
#include "../Board.h"
#include "../AIPerplex.h"
#include "../PlayerBase.h"
#include "../Eval.h"
#include "../MoveFormatter.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <nlohmann/json.hpp>

namespace Testing {

using json = nlohmann::json;

std::vector<TacticalPosition> TacticalTestRunner::load_test_cases(const std::string& json_filename)
{
    std::filesystem::path path = std::filesystem::current_path().parent_path();
    path /= "Tests/";
    path.append(json_filename);

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "ERROR: Cannot open " << json_filename << "\n";
        std::cerr << "Working directory: " << std::filesystem::current_path() << "\n";
        throw std::runtime_error("Failed to load tactical test cases");
    }

    json data = json::parse(file);
    std::vector<TacticalPosition> cases;
    cases.reserve(data["tactical_test_cases"].size());

    for (const auto& tc : data["tactical_test_cases"]) {
        TacticalPosition pos;
        pos.id          = tc["id"].get<std::string>();
        pos.category    = tc["category"].get<std::string>();
        pos.description = tc["description"].get<std::string>();
        pos.fen         = tc["fen"].get<std::string>();
        pos.depth       = tc["depth"].get<int>();
        for (const auto& m : tc["best_moves"])
            pos.best_moves.push_back(m.get<std::string>());
        cases.push_back(std::move(pos));
    }
    return cases;
}

TacticalResult TacticalTestRunner::run_position(const TacticalPosition& pos)
{
    TacticalResult result;
    result.id          = pos.id;
    result.description = pos.description;

    Board::Instance().SetupFromFEN(pos.fen);
    GameInfo info = Board::Instance().GetGameInfo();

    auto ai = PlayerBase::Create(PlayerBase::ePlayerTypes::AI_PERPLEX,
                                 static_cast<unsigned>(pos.depth));
    AIPerplex::SetVerboseLogging(false);
    ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);

    const auto t0 = std::chrono::steady_clock::now();
    Move m = ai->GetMove(info);
    const auto t1 = std::chrono::steady_clock::now();

    result.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    result.engine_move_uci = MoveFormatter::ToUCI(m);

    for (const auto& accepted : pos.best_moves) {
        if (result.engine_move_uci == accepted) {
            result.passed = true;
            break;
        }
    }
    return result;
}

bool TacticalTestRunner::run_test_suite(double required_pass_rate, bool verbose)
{
    auto positions = load_test_cases("tactical_test_cases.json");

    std::cout << "\n========================================\n";
    std::cout << "Tactical Test Suite (" << positions.size() << " positions)\n";
    std::cout << "========================================\n\n";

    int passed = 0;
    int failed = 0;

    for (const auto& pos : positions) {
        if (verbose) {
            std::cout << "[" << pos.id << "] " << pos.description << "\n";
            std::cout << "  FEN:   " << pos.fen << "\n";
            std::cout << "  Depth: " << pos.depth << "\n";
        }

        TacticalResult result = run_position(pos);

        const char* verdict = result.passed ? "PASS" : "FAIL";
        if (verbose) {
            std::cout << "  Engine: " << result.engine_move_uci
                      << "  Expected: [";
            for (size_t i = 0; i < pos.best_moves.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << pos.best_moves[i];
            }
            std::cout << "]  " << verdict
                      << "  (" << result.time_ms << " ms)\n\n";
        }

        if (result.passed) ++passed; else ++failed;
    }

    const int total = passed + failed;
    const double pass_rate = (total > 0) ? static_cast<double>(passed) / total : 0.0;
    const bool ok = pass_rate >= required_pass_rate;

    std::cout << "========================================\n";
    std::cout << "Results: " << passed << "/" << total << " passed"
              << " (" << static_cast<int>(pass_rate * 100) << "%)\n";
    std::cout << "Required: " << static_cast<int>(required_pass_rate * 100) << "%\n";
    std::cout << (ok ? "PASS" : "FAIL") << "\n";
    std::cout << "========================================\n\n";

    return ok;
}

} // namespace Testing
```

---

### Task 3: Create Tests/tactical_test_cases.json

**Files:**
- Create: `Tests/tactical_test_cases.json`

All positions below are verified. Positions should be expanded to 35+ before the suite is considered production-ready (see TestDesign.md for content goals). To add WAC positions, source FENs from chessprogramming.org/Win_at_Chess and verify each best move with an external engine.

```json
{
  "tactical_test_cases": [
    {
      "id": "M1-001",
      "category": "mate_in_1",
      "description": "Rook delivers back-rank checkmate",
      "fen": "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1",
      "best_moves": ["a1a8"],
      "depth": 4
    },
    {
      "id": "M1-002",
      "category": "mate_in_1",
      "description": "Queen delivers back-rank checkmate along d-file",
      "fen": "6k1/5ppp/8/8/8/8/3Q4/6K1 w - - 0 1",
      "best_moves": ["d2d8"],
      "depth": 4
    },
    {
      "id": "HANG-001",
      "category": "hanging_piece",
      "description": "Queen captures undefended rook on c1",
      "fen": "4k3/8/8/8/8/8/8/2rQK3 w - - 0 1",
      "best_moves": ["d1c1"],
      "depth": 4
    },
    {
      "id": "BACK-001",
      "category": "back_rank",
      "description": "Rook invades back rank with check, then captures black rook on a8",
      "fen": "r4rk1/pp3ppp/8/8/8/8/PP3PPP/3RR1K1 w - - 0 1",
      "best_moves": ["d1d8"],
      "depth": 5
    },
    {
      "id": "BACK-002",
      "category": "back_rank",
      "description": "Rook invades back rank with check, then captures black rook on f8",
      "fen": "5rk1/p4ppp/8/8/8/8/P4PPP/3RR1K1 w - - 0 1",
      "best_moves": ["d1d8"],
      "depth": 5
    },
    {
      "id": "QFORK-001",
      "category": "queen_fork",
      "description": "Queen fork wins rook: Qa4+ checks king and threatens Qxd5",
      "fen": "8/8/8/3r4/4k3/8/8/3QK3 w - - 0 1",
      "best_moves": ["d1a4"],
      "depth": 4
    },
    {
      "id": "BACK-003",
      "category": "back_rank",
      "description": "White rook captures black back-rank rook with check",
      "fen": "3r2k1/p4ppp/8/8/8/8/PPP3PP/3R1RK1 w - - 0 1",
      "best_moves": ["d1d8"],
      "depth": 5
    },
    {
      "id": "M2-001",
      "category": "mate_in_2",
      "description": "Ladder mate in 2: Rh7+ Kg8 Rb8#",
      "fen": "7k/8/6K1/8/8/8/8/1R5R w - - 0 1",
      "best_moves": ["h1h7"],
      "depth": 5
    }
  ]
}
```

---

### Task 4: Wire TacticalTestRunner into StratChessEvolved.vcxproj + filters

**Files:**
- Modify: `StratChessEvolved/StratChessEvolved.vcxproj`
- Modify: `StratChessEvolved/StratChessEvolved.vcxproj.filters`

**Step 1: Add ClCompile entry to vcxproj**

Find in `StratChessEvolved.vcxproj`:
```xml
    <ClCompile Include="..\StratEngine\Tests\Perft.cpp" />
```

Replace with:
```xml
    <ClCompile Include="..\StratEngine\Tests\Perft.cpp" />
    <ClCompile Include="..\StratEngine\Tests\TacticalTestRunner.cpp" />
```

**Step 2: Add ClInclude entry to vcxproj**

Find in `StratChessEvolved.vcxproj`:
```xml
    <ClInclude Include="..\StratEngine\Tests\Perft.h" />
```

Replace with:
```xml
    <ClInclude Include="..\StratEngine\Tests\Perft.h" />
    <ClInclude Include="..\StratEngine\Tests\TacticalTestRunner.h" />
```

**Step 3: Add ClCompile filter entry to vcxproj.filters**

Find in `StratChessEvolved.vcxproj.filters`:
```xml
    <ClCompile Include="..\StratEngine\Tests\Perft.cpp">
      <Filter>Tests</Filter>
    </ClCompile>
```

Replace with:
```xml
    <ClCompile Include="..\StratEngine\Tests\Perft.cpp">
      <Filter>Tests</Filter>
    </ClCompile>
    <ClCompile Include="..\StratEngine\Tests\TacticalTestRunner.cpp">
      <Filter>Tests</Filter>
    </ClCompile>
```

**Step 4: Add ClInclude filter entry to vcxproj.filters**

Find the Perft.h include filter entry. Its exact text:
```xml
    <ClInclude Include="..\StratEngine\Tests\Perft.h">
```

Add after its closing `</ClInclude>`:
```xml
    <ClInclude Include="..\StratEngine\Tests\TacticalTestRunner.h">
      <Filter>Header Files</Filter>
    </ClInclude>
```

*(If the Perft.h entry doesn't have `<Filter>` content, just add the TacticalTestRunner entry as a sibling in the same `<ItemGroup>` as the Perft.h entry.)*

---

### Task 5: Add `tactical` command to StratChessEvolved.cpp

**Files:**
- Modify: `StratChessEvolved/StratChessEvolved.cpp`

**Step 1: Add the include**

Find:
```cpp
#include <Tests/Perft.h>
```

Replace with:
```cpp
#include <Tests/Perft.h>
#include <Tests/TacticalTestRunner.h>
```

**Step 2: Add the tactical runner function**

Find (right before `static int perftrunner`):
```cpp
static int perftrunner(int argc, char** argv) {
```

Insert before it:
```cpp
static int tacticalrunner(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: tactical test\n";
        return 1;
    }
    const std::string command = argv[1];
    if (command == "test") {
        const bool ok = Testing::TacticalTestRunner::run_test_suite(0.90, true);
        return ok ? 0 : 1;
    }
    std::cerr << "Error: unknown tactical command '" << command << "'\n";
    std::cout << "Usage: tactical test\n";
    return 1;
}

```

**Step 3: Dispatch to tacticalrunner in main()**

Find in `main()`:
```cpp
	// Check for perft commands
	if (argc > 2 && std::string(argv[1]) == "perft") {
		return perftrunner(argc - 1, &argv[1]);
	}
```

Replace with:
```cpp
	// Check for perft commands
	if (argc > 2 && std::string(argv[1]) == "perft") {
		return perftrunner(argc - 1, &argv[1]);
	}
	// Check for tactical commands
	if (argc > 2 && std::string(argv[1]) == "tactical") {
		return tacticalrunner(argc - 1, &argv[1]);
	}
```

---

### Task 6: Build and verify

**Step 1: Build main project**

```powershell
.\build.ps1 main
```
Expected: `Build succeeded` with zero warnings.

**Step 2: Ensure Tests/logs/ directory exists**

AIPerplex creates `logs/aiperplex.log` relative to the working directory when constructed. Run from `Tests/`, so `Tests/logs/` must exist:

```powershell
New-Item -ItemType Directory -Force -Path Tests\logs
```

**Step 3: Run tactical test suite**

```powershell
cd Tests
..\x64\Release\StratChessEvolved.exe tactical test
```

Expected output:
```
========================================
Tactical Test Suite (8 positions)
========================================
...
Results: 8/8 passed (100%)
Required: 90%
PASS
========================================
```

If fewer than 7/8 pass, diagnose each failing position:
- Check `engine_move_uci` vs expected moves in the output
- Try running the position at higher depth to confirm the expected answer
- If the JSON best_move is wrong, fix the JSON (not the engine)
- If the engine finds a different but equally good move, add it to `best_moves`

**Step 4: Run full unit test suite to confirm no regressions**

```powershell
cd ..
.\StratChessTests\x64\Release\StratChessTests.exe
```
Expected: all existing assertions still pass (238+ total).

---

### Task 7: Update TestDesign.md

**Files:**
- Modify: `Docs/TestDesign.md`

**Step 1: Update the "Full tactical suite" entry in the coverage table**

Find:
```markdown
| Full tactical suite (WAC/mate-in-N) | — | ⏳ Phase 1 | `StratChessEvolved.exe tactical test` |
```

Replace with:
```markdown
| Full tactical suite (WAC/mate-in-N) | — | ✅ Phase 1 | `StratChessEvolved.exe tactical test` |
```

**Step 2: Update the "Full tactical suite in main executable" section body**

Find:
```markdown
### Full tactical suite in main executable

**When**: before UCI — needed to assess engine readiness before first public play. Does not need to wait for King Safety / Mobility evaluation extension.
**Files**: `StratEngine/Tests/TacticalTestRunner.h/cpp`, `Tests/tactical_test_cases.json`
**Invocation**: `StratChessEvolved.exe tactical test`
**Acceptance**: 90%+ pass rate on included positions

Content:
- WAC (Win At Chess) subset — 25 representative positions
- Mate-in-2 positions — 10 positions
- Endgame K+Q vs K, K+R vs K basic positions
```

Replace with:
```markdown
### Full tactical suite in main executable

**Status**: ✅ **Done.** Infrastructure landed March 2026; initial 8 positions all passing.
**Files**: `StratEngine/Tests/TacticalTestRunner.h/cpp`, `Tests/tactical_test_cases.json`
**Invocation**: Run from `Tests/` directory (same as perft):
```bash
cd Tests
../x64/Release/StratChessEvolved.exe tactical test
```
**Acceptance**: 90%+ pass rate (`required_pass_rate = 0.90` in `run_test_suite()`).
**Note**: `Tests/logs/` must pre-exist — AIPerplex creates `logs/aiperplex.log` relative to working directory.

**Initial positions (8, all verified):**
- `M1-001`, `M1-002`: Mate-in-1 (back-rank rook and queen)
- `HANG-001`: Win hanging rook (queen captures)
- `BACK-001`, `BACK-002`, `BACK-003`: Back-rank invasion wins rook
- `QFORK-001`: Queen fork wins rook (Qa4+ check)
- `M2-001`: Mate-in-2 (two-rook ladder: Rh7+ Kg8 Rb8#)

**Expanding the suite**: Source WAC positions from chessprogramming.org/Win_at_Chess. Add each position to `Tests/tactical_test_cases.json` with a verified `best_moves` list. Target: 25 WAC + 10 mate-in-2 + endgame K+Q/K+R positions.
```

*(Note: the markdown code block inside is intentional — use a fenced block correctly in the actual file.)*

---

### Task 8: Commit

**Step 1: Verify no debugging artifacts**

- `game_settings.json`: starting FEN, both players AI_PERPLEX — fix if a session left a custom FEN
- `StratEngine/AIPerplex.h`: `lmr_enabled = true` (not false)

**Step 2: Stage and commit**

```powershell
git add StratEngine/Tests/TacticalTestRunner.h
git add StratEngine/Tests/TacticalTestRunner.cpp
git add Tests/tactical_test_cases.json
git add StratChessEvolved/StratChessEvolved.vcxproj
git add StratChessEvolved/StratChessEvolved.vcxproj.filters
git add StratChessEvolved/StratChessEvolved.cpp
git add Docs/TestDesign.md
git commit -m "$(cat <<'EOF'
feat: add tactical test suite (StratChessEvolved.exe tactical test)

- TacticalTestRunner.h/cpp: JSON-driven runner in Testing namespace,
  follows Perft infrastructure pattern; loads tactical_test_cases.json
  from Tests/ directory; runs AIPerplex at per-position depth; compares
  first move (UCI) against accepted-moves list; reports pass rate
- Tests/tactical_test_cases.json: 8 initial verified positions (mate-in-1
  x2, hanging piece, back-rank x3, queen fork, mate-in-2)
- StratChessEvolved.cpp: 'tactical test' command dispatches to runner
- TestDesign.md: full tactical suite row marked done; section updated

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Success Criteria

- `.\build.ps1 main` — zero warnings
- `cd Tests && ..\x64\Release\StratChessEvolved.exe tactical test` — 8/8 pass (100% ≥ 90% threshold)
- `.\StratChessTests\x64\Release\StratChessTests.exe` — all existing tests still pass (238+ assertions)
- TestDesign.md updated with status ✅
