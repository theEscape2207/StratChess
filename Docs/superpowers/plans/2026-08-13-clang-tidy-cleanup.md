# Clang-Tidy Source and Configuration Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver PR 1 of the approved clang-tidy design by removing PR #293's parser churn, correcting and shortening lint-driven comments, and centralizing accepted enum-size exclusions without changing behavior or CI enforcement.

**Architecture:** Keep `FENParser::ParseFEN()` as the public `noexcept` error boundary and move its existing parsing body into a private `ParseFENImpl()` function. Keep the root `.clang-tidy` broad/advisory for this PR, but replace per-declaration enum-size suppressions with the check's semicolon-separated `EnumIgnoreList`. CI behavior and fast/deep profiles remain PR 2 work.

**Tech Stack:** C++20, CMake/Ninja, Catch2 v3, PowerShell 7, clang-format/clang-tidy 22, existing `Run-Bench.ps1` equivalence harness.

## Global Constraints

- Base commit is merged PR #293 at `75d6835`, including blocker fix `aab72dc`.
- Do not change CI workflows, lint failure semantics, or check-family placement in this PR.
- Preserve `FENParser::ParseFEN()`'s signature, outputs, error strings, and `noexcept` guarantee.
- `log_warning_noexcept()` catches logging failures only; metadata correction always continues.
- Source comments state current invariants and omit review/tool history.
- Retain all nine enum-size exclusions centrally; do not narrow an enum in this PR.
- Preserve the user's unrelated main-worktree edits by working only in the isolated cleanup worktree.

---

### Task 1: Capture the behavioral baseline

**Files:**
- No repository files modified.
- Create outside the repository: `$env:TEMP/stratchess-clang-tidy-cleanup-before.csv`

**Interfaces:**
- Consumes: current Release engine at `build/windows-clang-cl/StratChessEvolved.exe`.
- Produces: eight-position baseline containing `Position`, `Nodes`, and `Best` for the post-change equivalence comparison.

- [ ] **Step 1: Build the Release engine and tests**

Run from the worktree root:

```powershell
./build.ps1 all
```

Expected: CMake/Ninja exits 0 and produces `build/windows-clang-cl/StratChessEvolved.exe` and `StratChessTests.exe`.

- [ ] **Step 2: Run the existing FEN regression tests before refactoring**

```powershell
./build/windows-clang-cl/StratChessTests.exe '[uci]'
```

Expected: all `[uci]` cases pass, including the throwing-sink metadata-correction case.

- [ ] **Step 3: Record the fixed-depth single-thread equivalence baseline**

```powershell
$beforeCsv = Join-Path $env:TEMP 'stratchess-clang-tidy-cleanup-before.csv'
./StratChessEvolved/Scripts/Run-Bench.ps1 `
  -Exe ./build/windows-clang-cl/StratChessEvolved.exe `
  -Threads 1 `
  -Csv $beforeCsv
```

Expected: all eight positions complete and the CSV is written. Timing/NPS are informational; only `Nodes` and `Best` are equivalence fields.

---

### Task 2: Extract the FEN parser implementation and tighten logging comments

**Files:**
- Modify: `StratEngine/Utils/FENParser.h:31-58`
- Modify: `StratEngine/Utils/FENParser.cpp:27-160,331-421`
- Modify: `StratChessTests/UCITests.cpp:867-928`

**Interfaces:**
- Consumes: public `ParseFEN(const std::string&, FENGameState&, std::vector<std::tuple<ePiece,eSquare>>&) noexcept`.
- Produces: private `ParseFENImpl(...)` with the same parameters and return type but no `noexcept`; anonymous-namespace `log_warning_noexcept(std::string_view, const Args&...) noexcept`.

- [ ] **Step 1: Pin the existing public error behavior in the focused test set**

No new observable behavior is required; the existing tests already pin too-few-fields, invalid formats, successful FENs, and throwing warning sinks. Run them as the red/green guard before editing:

```powershell
./build/windows-clang-cl/StratChessTests.exe '[uci]'
```

Expected before edit: PASS. Any later failure is introduced by the refactor.

- [ ] **Step 2: Declare the private implementation boundary**

Add to `FENParser`'s private section:

```cpp
static std::optional<std::string> ParseFENImpl(
    const std::string& fen, FENGameState& outState,
    std::vector<std::tuple<ePiece, eSquare>>& outPieces);
```

Remove the two helper comments that explain their exception specification through the old outer `try` indentation. Their private placement and the implementation boundary now express that relationship.

- [ ] **Step 3: Replace the indented parser with a thin public wrapper**

Keep the public function as:

```cpp
std::optional<std::string> FENParser::ParseFEN(
    const std::string& fen, FENGameState& outState,
    std::vector<std::tuple<ePiece, eSquare>>& outPieces) noexcept
{
    try {
        return ParseFENImpl(fen, outState, outPieces);
    } catch (...) {
        return std::string("internal error parsing FEN");
    }
}
```

Move the former `try` body verbatim into `ParseFENImpl()`, removing one indentation level and its old catch. Keep the `bugprone-exception-escape` suppression on the public boundary while PR 1 still uses the broad profile.

- [ ] **Step 4: Rename and simplify the warning boundary**

Rename every `safe_warn(...)` call and the helper itself to `log_warning_noexcept(...)`. Replace the history-heavy helper comment with the current invariant:

```cpp
// Metadata correction must continue even when best-effort warning logging fails.
```

Keep the local empty-catch suppression because the empty body is intentional and the root profile remains broad in PR 1.

- [ ] **Step 5: Shorten the throwing-sink test comments**

Retain `ThrowingSink`, `ScopedThrowingSink`, and the test assertions. Remove references to PR #293, earlier line numbers, and past failure mechanics. Keep only:

```cpp
// Throws on every log call to verify metadata correction does not depend on logging.
```

and a one-line RAII cleanup explanation where useful.

- [ ] **Step 6: Format and run the focused tests**

```powershell
./StratChessEvolved/Scripts/Run-Lint.ps1 -Check Format -Fix
./build.ps1 tests
./build/windows-clang-cl/StratChessTests.exe '[uci]'
```

Expected: format reaches a fixpoint, the test target builds, and every `[uci]` case passes.

- [ ] **Step 7: Review the FEN diff for churn**

```powershell
git diff --stat -- StratEngine/Utils/FENParser.cpp
git diff --word-diff=porcelain -- StratEngine/Utils/FENParser.cpp
```

Expected: the substantive changes are the wrapper/implementation split and helper rename; the former whole-body indentation no longer obscures review.

- [ ] **Step 8: Commit the FEN cleanup**

```powershell
git add StratEngine/Utils/FENParser.h StratEngine/Utils/FENParser.cpp StratChessTests/UCITests.cpp
git commit -m "refactor: isolate FEN parser exception boundary"
```

---

### Task 3: Centralize enum-size exclusions and correct lint-driven comments

**Files:**
- Modify: `.clang-tidy`
- Modify: `StratEngine/defines.h:72-108`
- Modify: `StratEngine/AIPerplex.h:109-119`
- Modify: `StratEngine/Eval.h:70-78`
- Modify: `StratEngine/GameState.h:20-30`
- Modify: `StratEngine/PlayerBase.h:12-24`
- Modify: `StratEngine/Utils/FenBatch.h:27-35`
- Modify: `StratEngine/PieceHelper.h:73-80`
- Modify: `StratEngine/TranspositionTable.h:135-156`
- Modify: `StratChessEvolved/StratChessEvolved.cpp:325-335`

**Interfaces:**
- Consumes: clang-tidy 22 option `performance-enum-size.EnumIgnoreList`.
- Produces: one central semicolon-separated exclusion value for all nine accepted enum types.

- [ ] **Step 1: Add the exact central ignore list**

Append this root configuration block:

```yaml
CheckOptions:
  performance-enum-size.EnumIgnoreList: >-
    eRowNames;eSquare;GameValues;AIPerplex::IterationDecision;AIPerplex::RejectionReason;EvalManager::EvalTypes;GameStates;PlayerBase::ePlayerTypes;FenBatch::LineKind
```

The option syntax is verified by clang-tidy 22's `--dump-config`; Clang documents fully qualified names/regexes separated by semicolons.

- [ ] **Step 2: Remove all nine source-level enum suppressions**

Delete each `// NOLINTNEXTLINE(performance-enum-size)` from the listed headers. Also delete lint-only prose:

- `AIPerplex`: “Local var…” and “Same as…” comments;
- `EvalManager` / `PlayerBase`: “Never a stored field” comments;
- `FenBatch`: the `LineResult` size comparison comment;
- `GameStates`: the #292 padding comment;
- `GameValues`: retain the domain statement only if it helps explain why it is an enum of constants.

- [ ] **Step 3: Replace inaccurate enum history with present constraints**

For `eRowNames`, remove the unrecorded-revert statement and the commented-out base type. Use:

```cpp
// NO_ROW requires a signed representation.
enum eRowNames {
```

For `eSquare`, remove the unrecorded-revert statement and leave #292 as the external investigation record rather than a source comment. Preserve the readable board layout and `clang-format` guards.

- [ ] **Step 4: Correct the range proofs**

Use this concise `AsPiece()` explanation:

```cpp
// Valid piece types and colors produce named ePiece values 0 through 13.
```

Keep the analyzer suppression because analyzer placement is PR 2 work.

Use this transposition-table proof:

```cpp
// Storage: normalize mate scores. Mate +/- MAX_PLY fits in int16_t.
```

The negative branch is covered by the symmetric bound and the casts remain explicit.

- [ ] **Step 5: Shorten `main()`'s exception-boundary comment**

Replace the crash/history/tool discussion with:

```cpp
// Convert uncaught startup/runtime exceptions into a process-level failure code.
```

Keep the `bugprone-exception-escape` suppression for PR 1's broad profile.

- [ ] **Step 6: Prove the central enum list covers exactly the removed suppressions**

```powershell
$tidy = (Get-Command clang-tidy -ErrorAction SilentlyContinue).Source
if (-not $tidy) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
  $vs = & $vswhere -latest -prerelease -products * -property installationPath | Select-Object -First 1
  $tidy = Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-tidy.exe'
}
& $tidy --config-file=.clang-tidy --checks=-*,performance-enum-size `
  -p build/windows-clang-cl --quiet `
  @(git ls-files '*.cpp' | Where-Object { $_ -notmatch '(^|/)Archived/' })
```

Expected: no `performance-enum-size` finding for any of the nine names and no `clang-diagnostic-error` across the whole tracked tree.

- [ ] **Step 7: Confirm source suppressions are gone and format is clean**

```powershell
rg 'NOLINTNEXTLINE\(performance-enum-size\)|prior attempt reverted|0xC0000409|safe_warn' `
  StratEngine StratChessEvolved StratChessTests
./StratChessEvolved/Scripts/Run-Lint.ps1 -Check Format -Fix
```

Expected: `rg` returns no matches; clang-format converges.

- [ ] **Step 8: Commit the centralized configuration and comments**

```powershell
git add .clang-tidy StratEngine StratChessEvolved/StratChessEvolved.cpp
git commit -m "chore: centralize accepted clang-tidy exclusions"
```

---

### Task 4: Verify behavior, lint cleanliness, and PR scope

**Files:**
- Modify only if verification finds a PR 1 defect.
- Create outside repository: `$env:TEMP/stratchess-clang-tidy-cleanup-after.csv`

**Interfaces:**
- Consumes: Task 1 baseline CSV and completed PR 1 source/config changes.
- Produces: build/test/lint/equivalence evidence suitable for the pull-request body.

- [ ] **Step 1: Run Release and Debug builds/tests using the repository procedure**

```powershell
./build.ps1 tests
./build/windows-clang-cl/StratChessTests.exe
./build.ps1 tests -Config Debug
./build/windows-clang-cl-debug/StratChessTests.exe
```

Expected: both configurations exit 0. Report test case and assertion totals for each.

- [ ] **Step 2: Run changed-source format and whole-tree Windows tidy**

```powershell
./StratChessEvolved/Scripts/Run-Lint.ps1 -Check Format
./StratChessEvolved/Scripts/Run-Lint.ps1 -Check Tidy -All
```

Expected: format passes and broad Windows/clang-cl tidy reports zero findings. Tidy remains advisory in PR 1, so inspect the complete output rather than relying only on the exit code.

- [ ] **Step 3: Record the post-change benchmark**

```powershell
$afterCsv = Join-Path $env:TEMP 'stratchess-clang-tidy-cleanup-after.csv'
./StratChessEvolved/Scripts/Run-Bench.ps1 `
  -Exe ./build/windows-clang-cl/StratChessEvolved.exe `
  -Threads 1 `
  -Csv $afterCsv
```

Expected: all eight positions complete.

- [ ] **Step 4: Compare only deterministic equivalence fields**

```powershell
$before = Import-Csv (Join-Path $env:TEMP 'stratchess-clang-tidy-cleanup-before.csv') |
  Select-Object Position, Nodes, Best
$after = Import-Csv (Join-Path $env:TEMP 'stratchess-clang-tidy-cleanup-after.csv') |
  Select-Object Position, Nodes, Best
$difference = Compare-Object $before $after -Property Position, Nodes, Best
if ($difference) { $difference | Format-Table; throw 'Search equivalence failed.' }
```

Expected: no output and exit 0. Do not compare `Ms` or `Nps`.

- [ ] **Step 5: Run Linux whole-tree tidy and fast tests**

Use the repository's existing WSL/Linux clang-22 path, configure a plain Release Clang database, run the broad root profile over all non-archived `.cpp` files, and run the fast Catch2 tier:

```bash
cmake -S . -B build-linux-tidy -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-22 \
  -DCMAKE_CXX_COMPILER=clang++-22
cmake --build build-linux-tidy --target StratChessTests --parallel
git ls-files '*.cpp' | grep -v '/Archived/' | \
  xargs clang-tidy-22 -p build-linux-tidy --quiet
./build-linux-tidy/StratChessTests '~[slow]'
```

Expected: zero tidy findings/diagnostic errors and all fast tests pass. If WSL lacks clang-22 or Ninja, report the environmental blocker rather than substituting another LLVM major.

- [ ] **Step 6: Audit scope and comments**

```powershell
git diff --check origin/main...HEAD
git diff --stat origin/main...HEAD
git diff --name-only origin/main...HEAD
git status --short
```

Expected: only the design/plan documents and PR 1 files are changed; no CI workflow or PR 2 implementation appears; worktree is clean after commits.

- [ ] **Step 7: Prepare the PR evidence**

The PR body must state:

- this is source/config hygiene only and CI remains advisory;
- `ParseFENImpl()` removes indentation churn without changing the public contract;
- `log_warning_noexcept()` retains the `aab72dc` correctness behavior;
- the exact nine enum names moved to `EnumIgnoreList`;
- Release/Debug case and assertion totals;
- Windows and Linux whole-tree tidy results;
- eight of eight benchmark positions have identical nodes and best moves;
- no approved design decisions changed during implementation, or list each change explicitly.
