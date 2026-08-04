# Tactical Suite Stability Mode + Deferred-Suite Scope Decision

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the second half of Near-Term Sequence step 3 (the deferred automated-suite
scope from PR #72): add a *stability mode* to the exe tactical suite (run the gated
31-position suite N consecutive times, fail on any per-run gate failure or any pass/fail
flip between runs), gate it in `Validate-PrePR.ps1`, and record the BT2630/ECM-GCP and
endgame-tablebase deferral decisions in the Roadmap. After this, nothing blocks Lazy SMP.

**Architecture:** Follow the existing pattern exactly — a pure, unit-testable verdict
policy function (`evaluate_stability()`, sibling of `evaluate_results()`) plus an
orchestrating runner (`run_stability_suite()`) and a new `tactical stability [N] [filename]`
exe subcommand. No new files; no search/eval code touched. The single-run `tactical test`
command stays (used by the staging workflow).

**Tech Stack:** C++20 (MSVC v145, x64 only, `/W4 /WX`), Catch2 v3 (`[suite_policy]` tag),
PowerShell 7 validation scripts.

**Why (analysis summary, from the pre-plan discussion in
`Test-suite-extension.md` scratch notes):**
- Once Lazy SMP lands, byte-identical node counts die as a validation signal. The ELO
  harness (PR #75) covers *statistical* strength regression; what suites uniquely add is
  fast, deterministic, *attributable* failure — a flipped position names the bug in
  minutes, an ELO match says "something broke" after hours.
- The cheapest detector for intermittent SMP race bugs is repeat-running the *existing*
  suite and requiring stable results — no new test content needed. Pre-SMP the engine is
  deterministic at fixed depth, so the gate is trivially green; the point is to have it
  wired in *before* threading starts (the issue #66 lesson: ungated suites drift silently).
- The flip check (not just "31/31 every run") keeps the detector working even if the suite
  later carries a tolerated non-mate failure inside the 90% threshold: a position that
  fails *consistently* passes stability (matches the gate policy); a position that fails
  *sometimes* is nondeterminism and fails it.
- BT2630/ECM-GCP: deferred until deeper search (post SEE/futility pruning). At average
  depth ~10.5 the engine would fail a large fraction, and a gate expected to fail is no gate.
- Endgame tablebase positions: scheduled with the eval progress work (issues #70 KQvKR
  no-progress, #76 shuffle play / premature 0.00) — built as the regression suite for those
  fixes, pinning each improvement with positions that failed before.

## Global Constraints

- Compiler: Level4 + `/WX` on x64 Debug and Release — any new warning is a build error.
  Approved narrowing only via `static_cast<>`. Never `#pragma warning(disable)`.
- C++20; `constexpr`/RAII/strong types preferred; English naming.
- Only x64 builds work; use `.\build.ps1` (never hard-code MSBuild paths).
- Pre-commit hook (`.githooks/pre-commit`) runs FEN check + fast tests on every commit —
  do not bypass it.
- Work on the worktree branch `worktree-lively-orbiting-lantern` (already level with
  `origin/main` at fd8b665); PR targets `main`.
- The diff touches no `Eval.cpp` and no `AIPerplex` search internals → no specialized
  reviewer dispatch required by the pre-PR checklist.
- Plan file lives at `.claude/plans/tactical-suite-stability-mode.md` and is committed
  with Task 1.

---

### Task 1: `evaluate_stability()` pure verdict policy (TDD)

**Files:**
- Modify: `StratEngine/Tests/TacticalTestRunner.h` (add `StabilityVerdict` +
  `evaluate_stability` declaration after `evaluate_results`, line ~40)
- Modify: `StratEngine/Tests/TacticalTestRunner.cpp` (implementation after
  `evaluate_results`, line ~93)
- Test: `StratChessTests/SuitePolicyTests.cpp` (append new `[suite_policy]` cases)
- Add: `.claude/plans/tactical-suite-stability-mode.md` (this file, committed here)

**Interfaces:**
- Consumes: existing `Testing::TacticalResult` struct and
  `TacticalTestRunner::evaluate_results(const std::vector<TacticalResult>&, double)`.
- Produces (Task 2 relies on these exact names):
  ```cpp
  struct StabilityVerdict {
      bool ok = false;
      bool comparable = true;              // false if runs differ in size (structural error)
      int runs = 0;
      std::vector<std::string> flipped_ids;    // positions whose pass flag differed across runs
      std::vector<int> failed_run_indices;     // 1-based indices of runs failing evaluate_results
  };
  [[nodiscard]] static StabilityVerdict evaluate_stability(
      const std::vector<std::vector<TacticalResult>>& runs, double required_pass_rate);
  ```

- [ ] **Step 1: Write the failing tests**

Append to `StratChessTests/SuitePolicyTests.cpp` (reuses the existing `make_result` helper
in that file):

```cpp
// --- evaluate_stability() -------------------------------------------------
// Stability policy: ok iff at least one run, all runs the same size, every
// run individually satisfies evaluate_results(), and no position's pass flag
// differs between runs (a "flip" = nondeterminism).

static std::vector<TacticalResult> make_run(std::initializer_list<std::pair<const char*, bool>> entries)
{
    std::vector<TacticalResult> run;
    for (const auto& [id, passed] : entries)
        run.push_back(make_result(id, "tactical_win", passed));
    return run;
}

TEST_CASE("identical passing runs are stable", "[suite_policy]")
{
    const auto run = make_run({{"A", true}, {"B", true}, {"C", true}});
    const auto v = TacticalTestRunner::evaluate_stability({run, run, run}, 0.90);
    CHECK(v.ok);
    CHECK(v.runs == 3);
    CHECK(v.flipped_ids.empty());
    CHECK(v.failed_run_indices.empty());
}

TEST_CASE("a single run degenerates to the per-run gate verdict", "[suite_policy]")
{
    const auto run = make_run({{"A", true}, {"B", true}});
    const auto v = TacticalTestRunner::evaluate_stability({run}, 0.90);
    CHECK(v.ok);
    CHECK(v.runs == 1);
}

TEST_CASE("a position flipping between runs fails stability", "[suite_policy]")
{
    // 10 positions per run so one failure (9/10 = 90%) still passes the
    // per-run gate — the flip alone must fail stability.
    std::vector<TacticalResult> run_pass, run_flip;
    for (int i = 0; i < 9; ++i) {
        run_pass.push_back(make_result("T", "tactical_win", true));
        run_flip.push_back(make_result("T", "tactical_win", true));
    }
    run_pass.push_back(make_result("FLIP", "tactical_win", true));
    run_flip.push_back(make_result("FLIP", "tactical_win", false));

    const auto v = TacticalTestRunner::evaluate_stability({run_pass, run_flip}, 0.90);
    CHECK_FALSE(v.ok);
    CHECK(v.failed_run_indices.empty());   // both runs pass the per-run gate
    REQUIRE(v.flipped_ids.size() == 1);
    CHECK(v.flipped_ids[0] == "FLIP");
}

TEST_CASE("a consistent mate failure fails every run but is not a flip", "[suite_policy]")
{
    std::vector<TacticalResult> run;
    for (int i = 0; i < 19; ++i)
        run.push_back(make_result("T", "tactical_win", true));
    run.push_back(make_result("WAC-004", "mate_in_2", false));   // fails per-run gate

    const auto v = TacticalTestRunner::evaluate_stability({run, run}, 0.90);
    CHECK_FALSE(v.ok);
    CHECK(v.flipped_ids.empty());                     // deterministic — no flip
    REQUIRE(v.failed_run_indices.size() == 2);
    CHECK(v.failed_run_indices[0] == 1);
    CHECK(v.failed_run_indices[1] == 2);
}

TEST_CASE("a consistent tolerated non-mate failure stays stable", "[suite_policy]")
{
    // 9/10 = 90% with a non-mate failure passes the gate; failing identically
    // in every run is deterministic, so stability passes too.
    std::vector<TacticalResult> run;
    for (int i = 0; i < 9; ++i)
        run.push_back(make_result("T", "tactical_win", true));
    run.push_back(make_result("T-FAIL", "tactical_win", false));

    const auto v = TacticalTestRunner::evaluate_stability({run, run, run}, 0.90);
    CHECK(v.ok);
}

TEST_CASE("empty run set fails stability safe", "[suite_policy]")
{
    const auto v = TacticalTestRunner::evaluate_stability({}, 0.90);
    CHECK_FALSE(v.ok);
    CHECK(v.runs == 0);
}

TEST_CASE("mismatched run sizes fail stability safe", "[suite_policy]")
{
    const auto a = make_run({{"A", true}, {"B", true}});
    const auto b = make_run({{"A", true}});
    const auto v = TacticalTestRunner::evaluate_stability({a, b}, 0.90);
    CHECK_FALSE(v.ok);
    CHECK_FALSE(v.comparable);
}
```

Note: `make_run` needs `#include <initializer_list>` and `<utility>` — both are already
transitively available via Catch2/vector; if `/W4 /WX` complains about missing includes it
won't (headers, not warnings), but add them explicitly at the top of the file next to the
existing includes if the build errors.

- [ ] **Step 2: Declare the interface so the tests compile, then verify they fail**

Add to `StratEngine/Tests/TacticalTestRunner.h`, inside `class TacticalTestRunner`,
directly after the `evaluate_results` declaration:

```cpp
    struct StabilityVerdict {
        bool ok = false;
        bool comparable = true;              // false if runs differ in size (structural error)
        int runs = 0;
        std::vector<std::string> flipped_ids;    // positions whose pass flag differed across runs
        std::vector<int> failed_run_indices;     // 1-based indices of runs failing evaluate_results
    };

    // Pure stability policy (unit-tested in SuitePolicyTests.cpp):
    // ok iff at least one run, all runs the same size, every run individually
    // satisfies evaluate_results(), and no position's pass flag differs
    // between runs. A flip = nondeterminism (the SMP race-bug signal).
    [[nodiscard]] static StabilityVerdict evaluate_stability(
        const std::vector<std::vector<TacticalResult>>& runs, double required_pass_rate);
```

Add a stub to `StratEngine/Tests/TacticalTestRunner.cpp` after `evaluate_results` so it
links (returns default → all new tests except the two fail-safe ones fail):

```cpp
TacticalTestRunner::StabilityVerdict TacticalTestRunner::evaluate_stability(
    const std::vector<std::vector<TacticalResult>>& runs, double required_pass_rate)
{
    (void)runs; (void)required_pass_rate;
    return {};
}
```

Run: `.\build.ps1 run-tests "[suite_policy]"`
Expected: FAIL — "identical passing runs are stable", "single run degenerates",
"tolerated non-mate failure stays stable" fail (`v.ok` false from the stub); fail-safe
cases pass incidentally.

- [ ] **Step 3: Implement `evaluate_stability`**

Replace the stub in `StratEngine/Tests/TacticalTestRunner.cpp`:

```cpp
TacticalTestRunner::StabilityVerdict TacticalTestRunner::evaluate_stability(
    const std::vector<std::vector<TacticalResult>>& runs, double required_pass_rate)
{
    StabilityVerdict v;
    v.runs = static_cast<int>(runs.size());
    if (runs.empty())
        return v;   // ok=false: fail safe on empty input

    for (size_t r = 0; r < runs.size(); ++r) {
        if (!evaluate_results(runs[r], required_pass_rate).ok)
            v.failed_run_indices.push_back(static_cast<int>(r) + 1);
    }

    const auto& first = runs.front();
    for (const auto& run : runs) {
        if (run.size() != first.size()) {
            v.comparable = false;
            return v;   // ok=false: runs are not position-by-position comparable
        }
    }

    for (size_t i = 0; i < first.size(); ++i) {
        for (size_t r = 1; r < runs.size(); ++r) {
            if (runs[r][i].passed != first[i].passed) {
                v.flipped_ids.push_back(first[i].id);
                break;
            }
        }
    }

    v.ok = v.failed_run_indices.empty() && v.flipped_ids.empty();
    return v;
}
```

- [ ] **Step 4: Run the tests and make sure they pass**

Run: `.\build.ps1 run-tests "[suite_policy]"`
Expected: PASS — all `[suite_policy]` cases (5 existing + 7 new) green.

- [ ] **Step 5: Commit**

```powershell
git add StratEngine/Tests/TacticalTestRunner.h StratEngine/Tests/TacticalTestRunner.cpp StratChessTests/SuitePolicyTests.cpp .claude/plans/tactical-suite-stability-mode.md
git commit -m @'
Tactical suite: evaluate_stability() verdict policy + unit tests

Pure policy for the upcoming stability mode: N suite runs must each pass
the existing gate AND no position may flip pass/fail between runs.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

### Task 2: `run_stability_suite()` runner + `tactical stability` CLI

**Files:**
- Modify: `StratEngine/Tests/TacticalTestRunner.h` (declaration after `run_test_suite`,
  line ~46)
- Modify: `StratEngine/Tests/TacticalTestRunner.cpp` (implementation after
  `run_test_suite`, line ~148)
- Modify: `StratChessEvolved/StratChessEvolved.cpp` (`tacticalrunner()`, line ~105)

**Interfaces:**
- Consumes: `evaluate_stability` / `StabilityVerdict` from Task 1; existing
  `load_test_cases`, `run_position`, `evaluate_results`.
- Produces: exe subcommand `tactical stability [N] [filename]` (N default 10, filename
  default `tactical_test_cases.json`), exit 0 on stable PASS / 1 otherwise — Task 3's
  gate relies on this exact CLI and exit-code contract.

- [ ] **Step 1: Declare and implement the runner**

Add to `StratEngine/Tests/TacticalTestRunner.h` after the `run_test_suite` declaration:

```cpp
    // Run the whole suite n_runs consecutive times. Prints one summary line
    // per run (plus any failing positions) and a final stability verdict.
    // Returns true iff every run passes the gate and no position flips.
    [[nodiscard]] static bool run_stability_suite(int n_runs,
                                                  double required_pass_rate = 0.90,
                                                  const std::string& json_filename = "tactical_test_cases.json");
```

Add to `StratEngine/Tests/TacticalTestRunner.cpp` after `run_test_suite`:

```cpp
bool TacticalTestRunner::run_stability_suite(int n_runs, double required_pass_rate,
                                             const std::string& json_filename)
{
    auto positions = load_test_cases(json_filename);

    std::cout << "\n========================================\n";
    std::cout << "Tactical Stability Suite (" << positions.size() << " positions x "
              << n_runs << " runs, " << json_filename << ")\n";
    std::cout << "========================================\n\n";

    std::vector<std::vector<TacticalResult>> runs;
    runs.reserve(static_cast<size_t>(n_runs));

    for (int r = 1; r <= n_runs; ++r) {
        std::vector<TacticalResult> results;
        results.reserve(positions.size());
        int64_t run_ms = 0;

        for (const auto& pos : positions) {
            TacticalResult result = run_position(pos);
            run_ms += result.time_ms;
            if (!result.passed) {
                std::cout << "  [" << result.id << "] engine " << result.engine_move_uci
                          << "  FAIL\n";
            }
            results.push_back(std::move(result));
        }

        const SuiteVerdict rv = evaluate_results(results, required_pass_rate);
        std::cout << "Run " << r << "/" << n_runs << ": " << rv.passed << "/" << rv.total
                  << " passed (" << run_ms << " ms)  " << (rv.ok ? "PASS" : "FAIL") << "\n";
        std::cout.flush();
        runs.push_back(std::move(results));
    }

    const StabilityVerdict sv = evaluate_stability(runs, required_pass_rate);

    std::cout << "\n========================================\n";
    std::cout << "Stability: " << sv.runs << " runs, "
              << sv.failed_run_indices.size() << " failing run(s), "
              << sv.flipped_ids.size() << " flipped position(s)\n";
    if (!sv.flipped_ids.empty()) {
        std::cout << "Flipped (nondeterministic pass/fail):";
        for (const auto& id : sv.flipped_ids) std::cout << " " << id;
        std::cout << "\n";
    }
    std::cout << (sv.ok ? "PASS" : "FAIL") << "\n";
    std::cout << "========================================\n\n";

    return sv.ok;
}
```

- [ ] **Step 2: Add the CLI subcommand**

In `StratChessEvolved/StratChessEvolved.cpp`, `tacticalrunner()`: extend the command
dispatch (after the `command == "test"` block) and update both usage strings.

```cpp
static int tacticalrunner(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: tactical test [filename] | tactical stability [N] [filename]\n";
        return 1;
    }
    const std::string command = argv[1];
    if (command == "test") {
        const std::string filename = (argc >= 3) ? argv[2] : "tactical_test_cases.json";
        const bool ok = Testing::TacticalTestRunner::run_test_suite(0.90, true, filename);
        return ok ? 0 : 1;
    }
    if (command == "stability") {
        int n_runs = 10;
        if (argc >= 3) {
            try {
                n_runs = std::stoi(argv[2]);
            } catch (const std::exception&) {
                std::cerr << "Error: N must be a number, got '" << argv[2] << "'\n";
                return 1;
            }
            if (n_runs < 1) {
                std::cerr << "Error: N must be >= 1, got " << n_runs << "\n";
                return 1;
            }
        }
        const std::string filename = (argc >= 4) ? argv[3] : "tactical_test_cases.json";
        const bool ok = Testing::TacticalTestRunner::run_stability_suite(n_runs, 0.90, filename);
        return ok ? 0 : 1;
    }
    std::cerr << "Error: unknown tactical command '" << command << "'\n";
    std::cout << "Usage: tactical test [filename] | tactical stability [N] [filename]\n";
    return 1;
}
```

(`<string>` provides `std::stoi`; already included via StdAfx/PCH.)

- [ ] **Step 3: Build main and run the new mode end-to-end**

```powershell
.\build.ps1 main
cd Tests
..\x64\Release\StratChessEvolved.exe tactical stability 3
cd ..
```

Expected: three `Run r/3: 31/31 passed (~1100 ms)  PASS` lines, then
`Stability: 3 runs, 0 failing run(s), 0 flipped position(s)` and `PASS`; exit code 0
(check `$LASTEXITCODE`).

Also verify the error paths:

```powershell
cd Tests
..\x64\Release\StratChessEvolved.exe tactical stability abc   # exit 1, "N must be a number"
..\x64\Release\StratChessEvolved.exe tactical stability 0     # exit 1, "N must be >= 1"
cd ..
```

- [ ] **Step 4: Run the fast test tier to confirm nothing regressed**

Run: `.\build.ps1 run-tests`
Expected: all fast-tier tests PASS.

- [ ] **Step 5: Commit**

```powershell
git add StratEngine/Tests/TacticalTestRunner.h StratEngine/Tests/TacticalTestRunner.cpp StratChessEvolved/StratChessEvolved.cpp
git commit -m @'
Tactical suite: stability mode (tactical stability [N] [filename])

Runs the gated suite N consecutive times; fails on any per-run gate
failure or any position flipping pass/fail between runs. Pre-SMP this is
trivially green; post-Lazy-SMP it is the cheap detector for intermittent
TT-race nondeterminism.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

### Task 3: Gate stability mode in `Validate-PrePR.ps1` Step 3

**Files:**
- Modify: `StratChessEvolved/Scripts/Validate-PrePR.ps1` (Step 3 block, line ~58, and
  `.DESCRIPTION` line 8)

**Interfaces:**
- Consumes: `tactical stability 10` CLI + exit code from Task 2.
- Produces: the pre-PR gate every future PR (including this one, in Task 5) runs.

- [ ] **Step 1: Update the Step 3 invocation and comments**

In `StratChessEvolved/Scripts/Validate-PrePR.ps1` replace the `.DESCRIPTION` line:

```
    3. Runs the exe tactical suite (Tests/tactical_test_cases.json, 90% threshold).
```

with:

```
    3. Runs the exe tactical suite in stability mode (10 consecutive runs of
       Tests/tactical_test_cases.json, 90% threshold per run + no pass/fail flips).
```

and replace the Step 3 block:

```powershell
# --- Step 3: Exe tactical suite ---
# Guards the 90% pass threshold on Tests/tactical_test_cases.json. Issue #66
# (QFORK-001 silently regressed to 7/8) went unnoticed because no automated
# gate ran this suite; it takes < 1 s, so it now runs before every PR.
Write-Host "`n==> Tactical suite (StratChessEvolved.exe tactical test)" -ForegroundColor Cyan
$testsDir = Join-Path $RepoRoot 'Tests'
Push-Location $testsDir
try {
    $tacticalFailed = $false
    try   { & $gameExe tactical test }
    catch { $tacticalFailed = $true; Write-Host "Tactical suite threw: $_" -ForegroundColor DarkGray }
    if ($LASTEXITCODE -ne 0) { $tacticalFailed = $true }
    $checkResults['Tactical suite'] = if ($tacticalFailed) { 'FAIL' } else { 'PASS' }
} finally {
    Pop-Location
}
```

with:

```powershell
# --- Step 3: Exe tactical suite (stability mode) ---
# Guards the 90% pass threshold on Tests/tactical_test_cases.json. Issue #66
# (QFORK-001 silently regressed to 7/8) went unnoticed because no automated
# gate ran this suite. Stability mode (10 consecutive runs, no pass/fail
# flips allowed) additionally detects nondeterministic search results — the
# cheap intermittent-race-bug signal once Lazy SMP threads share the TT.
# ~11 s total; trivially deterministic (hence green) on single-threaded builds.
Write-Host "`n==> Tactical suite (StratChessEvolved.exe tactical stability 10)" -ForegroundColor Cyan
$testsDir = Join-Path $RepoRoot 'Tests'
Push-Location $testsDir
try {
    $tacticalFailed = $false
    try   { & $gameExe tactical stability 10 }
    catch { $tacticalFailed = $true; Write-Host "Tactical suite threw: $_" -ForegroundColor DarkGray }
    if ($LASTEXITCODE -ne 0) { $tacticalFailed = $true }
    $checkResults['Tactical suite'] = if ($tacticalFailed) { 'FAIL' } else { 'PASS' }
} finally {
    Pop-Location
}
```

- [ ] **Step 2: Verify the script step in isolation**

The full script runs in Task 5; here just confirm the edited invocation works from the
script's working-directory convention:

```powershell
cd Tests
..\x64\Release\StratChessEvolved.exe tactical stability 10
cd ..
```

Expected: 10 × `31/31 passed` + final `PASS`, exit 0, ~11 s wall time.

- [ ] **Step 3: Commit**

```powershell
git add StratChessEvolved/Scripts/Validate-PrePR.ps1
git commit -m @'
Validate-PrePR: run tactical suite in stability mode (10x)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

### Task 4: Record the scope decision in Roadmap + TestDesign

**Files:**
- Modify: `Docs/Roadmap.md` (Near-Term Sequence step 3, line ~48; "Create Automated Test
  Suite" item, line ~222)
- Modify: `Docs/TestDesign.md` (pipeline table row line ~26; "Full tactical suite in main
  executable" section line ~213)

**Interfaces:** documentation only; wording below is final copy.

- [ ] **Step 1: Roadmap — close out Near-Term Sequence step 3**

In `Docs/Roadmap.md`, replace:

```
   — ELO baseline half ✅ done (July 2026): `Run-EloMatch.ps1` + `Docs/EloLog.md`, sanity
   baseline at ±0 (identical builds, pooled 1000 games). The deferred-suite scope revisit
   (endgame tablebase positions, BT2630/ECMGCP) remains open.
```

with:

```
   — ELO baseline half ✅ done (July 2026): `Run-EloMatch.ps1` + `Docs/EloLog.md`, sanity
   baseline at ±0 (identical builds, pooled 1000 games).
   — Deferred-suite scope revisit ✅ decided (July 2026): the pre-SMP artifact is
   **stability mode** — `tactical stability N` runs the gated suite N consecutive times
   and fails on any per-run gate failure *or* any position flipping pass/fail between
   runs; gated at N=10 in `Validate-PrePR.ps1` Step 3. BT2630/ECM-GCP stays deferred
   until deeper search (post SEE/futility); the endgame tablebase set is scheduled with
   the eval progress work (#70/#76) as its regression suite. **Step 3 complete — nothing
   further blocks Lazy SMP.**
```

Also update the Critical Priority section (line ~62), replacing:

```
cleared; what remains before Lazy SMP is step 3 of the Near-Term Sequence above (ELO baseline
+ deferred-suite scope revisit).
```

with:

```
cleared; step 3 of the Near-Term Sequence above (ELO baseline + deferred-suite scope
revisit) completed July 2026 — Lazy SMP is unblocked.
```

- [ ] **Step 2: Roadmap — update the "Create Automated Test Suite" item**

Replace the **Deferred** bullet:

```
- **Deferred**: BT2630/ECMGCP tactical sets, endgame tablebase positions — revisit before
  Lazy SMP and/or after evaluation work (king safety, mobility) gives them something to catch
```

with:

```
- ✅ **Stability mode (July 2026)**: `tactical stability [N] [filename]` runs the gated
  suite N consecutive times; fails on any per-run gate failure or any position whose
  pass/fail flips between runs (`evaluate_stability()`, unit-tested in `[suite_policy]`).
  Gated at N=10 in `Validate-PrePR.ps1` Step 3 — the cheap detector for intermittent
  nondeterminism once Lazy SMP threads race on the shared TT.
- **Deferred (decided July 2026)**: BT2630/ECMGCP — until deeper search (SEE/futility
  pruning); at avg depth ~10.5 the engine would fail a large fraction, and a gate expected
  to fail is no gate. Endgame tablebase positions — build alongside the eval progress work
  (#70/#76) as its regression suite, pinning each fix with positions that failed before.
```

- [ ] **Step 3: TestDesign — document the mode**

In `Docs/TestDesign.md`:

(a) Update the pipeline table row:

```
| Full tactical suite | `StratChessEvolved.exe tactical test` | — | Seconds | Pre-PR (automated: `Validate-PrePR.ps1` Step 3) |
```

to:

```
| Full tactical suite | `StratChessEvolved.exe tactical stability 10` (single run: `tactical test`) | — | ~11 s | Pre-PR (automated: `Validate-PrePR.ps1` Step 3) |
```

(b) In the "Full tactical suite in main executable" section, after the **Invocation**
paragraph, insert:

```
**Stability mode**: `StratChessEvolved.exe tactical stability [N] [filename]` (N defaults
to 10) runs the whole suite N consecutive times and fails on either (a) any run failing
the normal gate policy or (b) any position whose pass/fail *flips* between runs. The flip
rule means a consistently-failing tolerated position (within the 90% threshold) does not
break stability, but a sometimes-failing one does — flips are the signal for
nondeterministic search results, the primary intermittent-race-bug symptom once Lazy SMP
threads share the TT. Policy is pure (`TacticalTestRunner::evaluate_stability()`) and
unit-tested in `SuitePolicyTests.cpp` (`[suite_policy]`). Gated at N=10 in
`Validate-PrePR.ps1` Step 3; on today's single-threaded fixed-depth search it is
deterministic and therefore trivially green.
```

(Read the section first for the exact anchor line; the Invocation paragraph ends with the
sentence referencing "Growing the suite" below.)

- [ ] **Step 4: Commit**

```powershell
git add Docs/Roadmap.md Docs/TestDesign.md
git commit -m @'
Docs: record deferred-suite scope decision + stability mode

Near-Term Sequence step 3 complete; BT2630/ECM-GCP deferred until deeper
search, endgame tablebase set scheduled with eval work (#70/#76).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
'@
```

---

### Task 5: Pre-PR validation and PR

**Files:** none new — validation + PR only.

- [ ] **Step 1: Sync with origin/main**

```powershell
git fetch origin main
git log HEAD..origin/main --oneline   # expect empty; merge if not
```

- [ ] **Step 2: Full pre-PR validation (code change → full gate required)**

```
cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1"
```

Expected: all four checks PASS — including the new Step 3 stability run (10 × 31/31).
This also dogfoods the gate change itself.

- [ ] **Step 3: Push and open the PR**

Per the memory note, `gh pr create --body` bypasses the PR template — format the body
manually as Why/Summary/Test plan/Notes. Base: `main`, head:
`worktree-lively-orbiting-lantern`.

```powershell
git push -u origin worktree-lively-orbiting-lantern
gh pr create --base main --title "Tactical suite stability mode + deferred-suite scope decision" --body @'
## Why

Closes the second half of Near-Term Sequence step 3 (the deferred automated-suite scope
from PR #72). Once Lazy SMP lands, node-count equivalence disappears as a validation
signal; this PR decides what the tactical-suite side of the safety net looks like before
threading starts — and what deliberately waits.

## Summary

- **Stability mode** (the one new pre-SMP artifact): `tactical stability [N] [filename]`
  runs the existing 31-position gated suite N consecutive times and fails on any per-run
  gate failure or any position flipping pass/fail between runs — the cheapest
  deterministic detector for intermittent SMP race bugs, no new test content needed.
  Pure verdict policy (`evaluate_stability()`) unit-tested in `[suite_policy]`.
- Gated at N=10 in `Validate-PrePR.ps1` Step 3 (~11 s; replaces the single run — run 1
  under the same per-run policy subsumes it). Single-run `tactical test` stays for the
  staging workflow.
- **Endgame tablebase set: scheduled with the eval work, not now** — the ELO baseline
  games showed endgame technique is the weakest area (#70/#76); the set gets built as the
  regression suite for those fixes.
- **BT2630/ECM-GCP: deferred until deeper search** — at avg depth ~10.5 the engine would
  fail a large fraction, and a gate expected to fail is no gate; revisit after SEE/futility
  pruning buys depth.
- Roadmap updated to record the decision and retire the open-ended "revisit" item —
  nothing further blocks Lazy SMP.

## Test plan

- [x] `[suite_policy]` unit tests: 7 new cases for `evaluate_stability()` (flip detection,
  per-run gate propagation, fail-safe on empty/mismatched input)
- [x] Stability mode end-to-end: `tactical stability 10` → 10 x 31/31, 0 flips, PASS
  (trivially green pre-SMP, as designed)
- [x] `Validate-PrePR.ps1` full gate green with the new Step 3

## Notes

With the ELO harness covering statistical strength regression, the suite's unique job is
fast, attributable failure — a flipped position names the bug in minutes; an ELO match
only says something broke after hours.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
'@
```

---

## Key Correctness Properties

1. `evaluate_stability` is pure and fail-safe: empty input, zero-size runs, and
   mismatched run sizes all yield `ok == false`.
2. Stability subsumes the old gate: run 1 is evaluated with the identical
   `evaluate_results` policy, so anything the single-run gate caught still fails.
3. A *consistently*-failing tolerated non-mate position does not fail stability
   (deterministic ≠ broken gate); a *sometimes*-failing position always does.
4. `tactical test` behavior is unchanged (staging workflow intact).
5. No search/eval code touched — node counts, ELO, and all existing test tiers are
   unaffected by construction.

## Validation Plan

- Per-task: `.\build.ps1 run-tests "[suite_policy]"` (Task 1), exe end-to-end run
  (Task 2/3), pre-commit hook on every commit.
- Final: `Validate-PrePR.ps1` full gate (build + extended `[slow]` tests + stability-mode
  tactical suite + self-play).
- Specialized reviewers: not required (no `Eval.cpp`, no `AIPerplex` search internals in
  the diff).
