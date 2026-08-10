# TT New-Game Clear Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove redundant on-clock TT clearing while preserving explicit new-game resets in UCI and non-UCI game mode.

**Architecture:** `TranspositionTable::clear()` uses its existing `entry_count` as a known-empty fast path and reports whether entries were removed. `PlayerAiBase` exposes a default no-op `StartNewGame()` lifecycle hook, `AIPerplex` overrides it to clear its TT, and the UCI and game-mode lifecycle owners invoke it before search. `GetMove()` no longer derives TT lifetime from `GameInfo::fullMoveCount`.

**Tech Stack:** C++20, Catch2 v3, clang-cl Release, PowerShell 7 repository scripts, UCI, fastchess.

## Global Constraints

- Fork and work only from fresh `origin/main`; PR target is `main`.
- Only x64 builds are supported; clang-cl Release is the shipping and measurement configuration.
- Warnings are errors; do not add warning suppressions.
- No TT entry-layout, replacement-policy, probe, store, or search-tuning changes.
- No lazy generations and no UCI `Hash` option.
- No per-node work or new probe/store locking.
- `clear()` is called only when no search is storing entries; document that source-level precondition.
- Preserve deterministic best moves and node counts at `Threads=1`.
- Run the required `search-reviewer`, self-play, and authoritative `Validate-PrePR.ps1` gates.
- Do not include workflow-friction observations in this PR; retain them separately for post-PR discussion.

---

### Task 1: Capture a Pre-Change Binary and Add the TT Empty Fast Path

**Files:**
- Modify: `StratChessTests/TTTests.cpp:117-176`
- Modify: `StratEngine/TranspositionTable.h:301-330`
- Runtime artifact: `StratChessEvolved/logs/issue259-baseline.exe` (gitignored, not committed)

**Interfaces:**
- Consumes: existing `TranspositionTable::entry_count`, bucket locks, and whole-table `tt_mutex`.
- Produces: `bool TranspositionTable::clear()`, returning `true` only when stored entries were removed.

- [ ] **Step 1: Build and preserve the exact pre-change engine binary**

Run:

```powershell
pwsh -ExecutionPolicy Bypass -File .\build.ps1 main
Copy-Item -LiteralPath .\build\windows-clang-cl\StratChessEvolved.exe `
  -Destination .\StratChessEvolved\logs\issue259-baseline.exe -Force
```

Confirm the copied file hash equals the built executable hash with `Get-FileHash`.

- [ ] **Step 2: Write failing `[tt]` tests for the observable fast-path contract**

Change the populated clear tests to assert `REQUIRE(tt.clear())`, then add:

```cpp
TEST_CASE("TT - clear reports no work for a freshly constructed table", "[tt]")
{
    TranspositionTable tt(1);
    REQUIRE_FALSE(tt.clear());
}

TEST_CASE("TT - repeated clear reports no work after the table is empty", "[tt]")
{
    TranspositionTable tt(1);
    do_store(tt, KEY_A, 100);

    REQUIRE(tt.clear());
    REQUIRE_FALSE(tt.clear());
}
```

- [ ] **Step 3: Build to verify RED**

Run:

```powershell
pwsh -ExecutionPolicy Bypass -File .\build.ps1 tests
```

Expected: compilation fails because the current `void clear()` cannot be used as a boolean condition.

- [ ] **Step 4: Implement the minimal fast path**

Change the signature and body to this shape, retaining the existing populated reset loop verbatim:

```cpp
// Lifecycle operation: callers must ensure no search can store concurrently.
// tt_mutex serializes clear calls; store() intentionally takes only bucket locks.
bool clear()
{
    std::scoped_lock g(tt_mutex);
    if (entry_count.load(std::memory_order_relaxed) == 0) {
        return false;
    }

    // Existing bucket loop and current_age/entry_count/pv_count resets.
    return true;
}
```

- [ ] **Step 5: Verify GREEN**

Run:

```powershell
pwsh -ExecutionPolicy Bypass -File .\build.ps1 tests
& .\build\windows-clang-cl\StratChessTests.exe '[tt]'
```

Expected: all `[tt]` cases pass, including populated clear semantics and both empty reports.

- [ ] **Step 6: Commit the TT behavior**

```powershell
git add StratEngine/TranspositionTable.h StratChessTests/TTTests.cpp
git commit -m "fix: skip clearing an empty TT"
```

---

### Task 2: Add the Explicit AI New-Game Lifecycle

**Files:**
- Modify: `StratChessTests/SearchTests.cpp:32-83`
- Modify: `StratEngine/PlayerAI.h:26-36`
- Modify: `StratEngine/AIPerplex.h:22-38`
- Modify: `StratEngine/AIPerplex.cpp:103-110`
- Modify: `StratEngine/UCIHandler.cpp:84-93`
- Modify: `StratEngine/Game.cpp:215-229`

**Interfaces:**
- Consumes: `PlayerAiBase`, the AIPerplex-owned `_tt`, UCI's stopped-search lifecycle, and `Game::SetPlayerParams()`'s existing `dynamic_cast<PlayerAiBase*>` pattern.
- Produces: `virtual void PlayerAiBase::StartNewGame() noexcept`, overridden by `void AIPerplex::StartNewGame() noexcept`.

- [ ] **Step 1: Extend the search fixture and write the failing lifecycle test**

Add fixture helpers using a marker key that is not zero:

```cpp
static constexpr uint64_t TT_MARKER_KEY = 0x7fff'ffff'ffff'ffffULL;

void store_tt_marker() const
{
    ai->_tt->store(TT_MARKER_KEY, 123, 1, 0, Move::EmptyMove(),
                   BoundType::EXACT, NodeType::PV_NODE, SearchPhase::MAIN);
}

bool has_tt_marker() const
{
    return ai->_tt->probe(TT_MARKER_KEY, 0).has_value();
}

void start_new_game() const { ai->StartNewGame(); }
```

Add:

```cpp
TEST_CASE("Search - StartNewGame clears a populated AIPerplex TT", "[search][tt]")
{
    AIPerlexTestFixture fix;
    fix.store_tt_marker();
    REQUIRE(fix.has_tt_marker());

    fix.start_new_game();

    REQUIRE_FALSE(fix.has_tt_marker());
}
```

- [ ] **Step 2: Build to verify RED**

Run `pwsh -ExecutionPolicy Bypass -File .\build.ps1 tests`.

Expected: compilation fails because `AIPerplex::StartNewGame()` does not exist.

- [ ] **Step 3: Implement the lifecycle interface and AIPerplex override**

In `PlayerAiBase`:

```cpp
virtual void StartNewGame() noexcept {}
```

In `AIPerplex` public declarations:

```cpp
void StartNewGame() noexcept override;
```

In `AIPerplex.cpp`:

```cpp
void AIPerplex::StartNewGame() noexcept
{
    (void)_tt->clear();
}
```

- [ ] **Step 4: Wire both off-clock lifecycle owners**

In `UciHandler::cmd_ucinewgame()`, after `init_ai()` and before board reset:

```cpp
if (ai_) ai_->StartNewGame();
```

In `Game::SetPlayerParams()`, combine the existing thread-setting cast with the lifecycle call so it always runs for AI players:

```cpp
if (auto* ai = dynamic_cast<PlayerAiBase*>(player.get())) {
    if (config.threads.has_value()) {
        ai->SetThreads(*config.threads);
    }
    ai->StartNewGame();
}
```

Legacy AIs inherit the no-op; human players do not enter the block.

- [ ] **Step 5: Verify GREEN and existing UCI option persistence**

Run:

```powershell
pwsh -ExecutionPolicy Bypass -File .\build.ps1 tests
& .\build\windows-clang-cl\StratChessTests.exe '[search][tt]'
& .\build\windows-clang-cl\StratChessTests.exe '[uci][smp]'
```

Expected: lifecycle test and existing `Threads survives cmd_ucinewgame` tests pass.

- [ ] **Step 6: Commit the lifecycle wiring**

```powershell
git add StratEngine/PlayerAI.h StratEngine/AIPerplex.h StratEngine/AIPerplex.cpp `
  StratEngine/UCIHandler.cpp StratEngine/Game.cpp StratChessTests/SearchTests.cpp
git commit -m "fix: reset TT through new-game lifecycle"
```

---

### Task 3: Remove Position-Metadata-Driven TT Lifetime

**Files:**
- Modify: `StratChessTests/SearchTests.cpp:32-104`
- Modify: `StratEngine/AIPerplex.cpp:142-161`

**Interfaces:**
- Consumes: the Task 2 TT marker helpers, `SearchLimits::fixed_depth(1)`, and a legal default board whose FEN fullmove field is one.
- Produces: `GetMove()` behavior that preserves TT entries independent of `GameInfo::fullMoveCount`.

- [ ] **Step 1: Add a fixture helper that performs a real depth-one search**

```cpp
void search_depth_one()
{
    ai->SetEvalEngine(EvalManager::EvalTypes::COMPLEX);
    GameInfo info = board_.GetGameInfo();
    REQUIRE(info.fullMoveCount == 1);
    const Move move = ai->GetMove(info, SearchLimits::fixed_depth(1));
    REQUIRE_FALSE(move.is_null());
}
```

- [ ] **Step 2: Write the failing regression test**

```cpp
TEST_CASE("Search - fullmove-one position does not define TT lifetime", "[search][tt]")
{
    AIPerlexTestFixture fix;
    fix.store_tt_marker();

    fix.search_depth_one();

    REQUIRE(fix.has_tt_marker());
}
```

- [ ] **Step 3: Run to verify RED**

Run:

```powershell
pwsh -ExecutionPolicy Bypass -File .\build.ps1 tests
& .\build\windows-clang-cl\StratChessTests.exe "Search - fullmove-one position does not define TT lifetime"
```

Expected: the test runs and fails because the existing `fullMoveCount == 1` block clears the marker before search.

- [ ] **Step 4: Remove the timed clear from `GetMove()`**

Delete only:

```cpp
// Only clear TT if new game (preserve across moves for better performance)
if (info.fullMoveCount == 1) {
    _tt->clear();
}
```

- [ ] **Step 5: Verify GREEN and focused regressions**

Run:

```powershell
pwsh -ExecutionPolicy Bypass -File .\build.ps1 tests
& .\build\windows-clang-cl\StratChessTests.exe '[tt]'
& .\build\windows-clang-cl\StratChessTests.exe '[search]'
& .\build\windows-clang-cl\StratChessTests.exe '[uci]'
```

Expected: the new fullmove-one test and all focused suites pass.

- [ ] **Step 6: Commit the metadata-lifetime fix**

```powershell
git add StratEngine/AIPerplex.cpp StratChessTests/SearchTests.cpp
git commit -m "fix: decouple TT lifetime from move count"
```

---

### Task 4: Validate Behavior, Timing, Game Mode, and the Final Diff

**Files:**
- Verify: all files changed by Tasks 1-3
- Do not commit: `StratChessEvolved/logs/issue259-baseline.exe` and probe output

**Interfaces:**
- Consumes: preserved baseline binary, candidate binary, repository test scripts, self-play skill, and `search-reviewer`.
- Produces: exact test evidence, timing evidence, deterministic equivalence evidence, reviewer disposition, and PR-ready validation.

- [ ] **Step 1: Build the final candidate and run the complete fast tier**

```powershell
pwsh -ExecutionPolicy Bypass -File .\build.ps1 main
pwsh -ExecutionPolicy Bypass -File .\build.ps1 run-tests
```

Record the exact test-case and assertion totals.

- [ ] **Step 2: Measure first-search latency against a past-move-one control**

For both baseline and candidate, run at least ten fresh UCI processes for each sequence:

```text
uci
isready
ucinewgame
position startpos
go depth 1
quit
```

and:

```text
uci
isready
ucinewgame
position startpos moves e2e4 e7e5
go depth 1
quit
```

Measure wall time to the `bestmove` line, not the engine's internal search time. Report median and range; the candidate's start-position overhead should converge toward its past-move-one control.

- [ ] **Step 3: Verify deterministic equivalence at `Threads=1`**

Run the same fixed-depth UCI position set through baseline and candidate. Compare every `bestmove`, reported depth, and node count. Any difference blocks completion because this change only moves lifecycle work.

- [ ] **Step 4: Run non-UCI game-mode self-play validation**

Use the `self-play-validate` skill with the explicit candidate path:

```text
C:\Users\thees\source\repos\StratChessEvolved\build\windows-clang-cl\StratChessEvolved.exe
```

Both sides must use AIPerplex (`type: 6`). Restore `game_settings.json` to its starting FEN afterward.

- [ ] **Step 5: Dispatch the mandatory search review**

Send the final `origin/main...HEAD` diff to `search-reviewer`. Address or explicitly rebut every finding on its merits, then rerun affected focused tests.

- [ ] **Step 6: Run authoritative pre-PR validation**

```powershell
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1
```

Expected Engine tier: build, extended tests, tactical suite, and self-play all pass.

- [ ] **Step 7: Review final scope and commit any validation-only corrections**

Run exact checks:

```powershell
git status --short
git diff --check origin/main...HEAD
git diff --stat origin/main...HEAD
git diff --name-only origin/main...HEAD
```

Only issue #259 source, tests, and plan files may be tracked. `.agents/` and probe/log artifacts remain untracked or ignored.

- [ ] **Step 8: Submit through the repository PR script**

Prepare a body covering motivation, design, fastchess `ucinewgame` verification, tests, timing, equivalence, self-play, reviewer disposition, and no-Elo rationale. Then run:

```powershell
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\New-PullRequest.ps1 `
  -Title "Avoid redundant on-clock TT clear" `
  -BodyFile C:\Users\thees\source\repos\StratChessEvolved\StratChessEvolved\logs\issue-259-pr-body.md
```

After successful submission, report the PR as awaiting the user-routed cross-agent review before merge.
