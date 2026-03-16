# Build & Test Automation Improvements Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace fragile inline shell logic in skills/CLAUDE.md with purpose-built PowerShell scripts that handle MSBuild discovery and working-directory resolution internally, and add structured pre-commit and pre-PR validation scripts.

**Architecture:** `build.ps1` (existing) gains a true parallel `all` verb via PowerShell jobs. Three new scripts in `StratChessEvolved/Scripts/` wrap build.ps1 for test running and validation. Skills and CLAUDE.md become thin pointers to these scripts — subagents call one canonical `cmd.exe /c powershell -File ...` line and get a reliable result.

**Tech Stack:** PowerShell 5.1+, MSBuild (via vswhere), Catch2 v3, StratChessEvolved self-play exe

**Spec:** `Docs/superpowers/specs/2026-03-16-build-test-automation-improvements-design.md`

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `build.ps1` | Modify | Single build entry point — add true parallel `all` |
| `StratChessEvolved/Scripts/Run-Tests.ps1` | Create | Thin wrapper: build tests + run with optional tag |
| `StratChessEvolved/Scripts/Validate-PreCommit.ps1` | Create | FEN check + fast test suite; exit 1 on any failure |
| `StratChessEvolved/Scripts/Validate-PrePR.ps1` | Create | Full parallel build + extended tests + self-play |
| `.claude/skills/run-tests/SKILL.md` | Modify | Canonical invocation pointer — no inline shell logic |
| `.claude/skills/self-play-validate/SKILL.md` | Modify | Canonical invocation pointer — no inline shell logic |
| `CLAUDE.md` | Modify | Add `all`-parallel note + Validation scripts table |

---

## Path Conventions for Scripts

Scripts live at `StratChessEvolved/Scripts/<name>.ps1`. Path navigation from `$PSScriptRoot`:

```powershell
$GameDir  = Split-Path $PSScriptRoot -Parent               # <repo>/StratChessEvolved/
$RepoRoot = Split-Path $GameDir -Parent                    # <repo>/
$buildScript  = Join-Path $RepoRoot 'build.ps1'
$settingsFile = Join-Path $GameDir 'game_settings.json'
$gameExe      = Join-Path $RepoRoot 'x64\Release\StratChessEvolved.exe'
$logsDir      = Join-Path $GameDir 'logs'
```

**Canonical invocation from any shell (bash, cmd, PS):**
```
cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\<name>.ps1"
```

---

## Chunk 1: build.ps1 + Scripts/

### Task 1: Update `build.ps1` — parallel `all` verb

**Files:**
- Modify: `build.ps1:12` (`.SYNOPSIS` `all` line)
- Modify: `build.ps1:102-105` (`all` switch case)

- [ ] **Step 1: Verify current sequential behaviour**

  Run from repo root:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File build.ps1 all"
  ```
  Expected: builds main then tests sequentially; output shows two `==>` lines, one after the other.

- [ ] **Step 2: Replace the `all` switch case**

  In `build.ps1`, replace:
  ```powershell
  'all' {
      Invoke-MSBuild $Sln -Parallel
      Invoke-MSBuild $TestProj -Parallel
  }
  ```
  With:
  ```powershell
  'all' {
      Write-Host "`n==> Building main + tests in parallel" -ForegroundColor Cyan
      $jobMain  = Start-Job { param($msbuild,$sln,$cfg,$plat)
          & $msbuild $sln /p:Configuration=$cfg /p:Platform=$plat /m /v:minimal
          $LASTEXITCODE
      } -ArgumentList $MSBuild,$Sln,$Config,$Platform

      $jobTests = Start-Job { param($msbuild,$proj,$cfg,$plat)
          & $msbuild $proj /p:Configuration=$cfg /p:Platform=$plat /m /v:minimal
          $LASTEXITCODE
      } -ArgumentList $MSBuild,$TestProj,$Config,$Platform

      Wait-Job $jobMain,$jobTests | Out-Null
      $exitMain  = Receive-Job $jobMain  | Select-Object -Last 1
      $exitTests = Receive-Job $jobTests | Select-Object -Last 1
      Remove-Job $jobMain,$jobTests

      if ($exitMain -ne 0 -or $exitTests -ne 0) {
          Write-Error "Parallel build failed (main=$exitMain tests=$exitTests)."
          exit 1
      }
  }
  ```

  > **Note on exit-code capture:** `$LASTEXITCODE` is emitted as the last statement inside each job so it becomes the job's output. After `Wait-Job`, `Receive-Job` retrieves it. `Select-Object -Last 1` discards any MSBuild stdout that may also appear. Do NOT check `.State -eq 'Failed'` — that only fires on PS terminating exceptions, not on MSBuild exit 1.

- [ ] **Step 3: Update `.SYNOPSIS` docs**

  In `build.ps1`, change the `.PARAMETER Verb` `all` line from:
  ```
      all             Build main then tests (default)
  ```
  To:
  ```
      all             Build main and tests in parallel (default)
  ```

- [ ] **Step 4: Verify happy path**

  Run:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File build.ps1 all"
  ```
  Expected: single `==> Building main + tests in parallel` header, both builds complete, exit 0.

- [ ] **Step 5: Verify negative path (exit code propagation)**

  In any `.cpp` file (e.g. `StratEngine/Eval.cpp`), introduce a deliberate syntax error on line 1:
  ```cpp
  THIS_IS_NOT_CPP
  ```
  Run:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File build.ps1 all"
  ```
  Expected: non-zero exit code, error message contains `main=1` or `tests=1`.
  **Immediately revert the syntax error before continuing.**

- [ ] **Step 6: Commit**

  ```
  git add build.ps1
  git commit -m "feat(build): parallel all verb using PS jobs with correct exit-code propagation"
  ```

---

### Task 2: Create `StratChessEvolved/Scripts/Run-Tests.ps1`

**Files:**
- Create: `StratChessEvolved/Scripts/Run-Tests.ps1`

- [ ] **Step 1: Create the `Scripts/` directory**

  ```
  mkdir StratChessEvolved\Scripts
  ```

- [ ] **Step 2: Write the script**

  Create `StratChessEvolved/Scripts/Run-Tests.ps1`:
  ```powershell
  <#
  .SYNOPSIS
      Build the test project and run the test suite.

  .DESCRIPTION
      Thin wrapper around build.ps1 run-tests. Resolves paths relative to $PSScriptRoot
      so it can be called from any working directory.

  .WHEN TO USE
      Any time you want to build and run tests — with or without a tag filter.

  .HOW TO INVOKE (from bash, cmd, or PowerShell)
      cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"
      cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [tactical]"

  .PARAMETER Tag
      Optional Catch2 tag filter, e.g. "[tactical]" or "[eval]". Omit for full fast suite (~[slow]).
  #>
  param([string]$Tag = '')

  Set-StrictMode -Version Latest

  $GameDir     = Split-Path $PSScriptRoot -Parent
  $RepoRoot    = Split-Path $GameDir -Parent
  $buildScript = Join-Path $RepoRoot 'build.ps1'

  if ($Tag) {
      & $buildScript run-tests $Tag
  } else {
      & $buildScript run-tests
  }
  exit $LASTEXITCODE
  ```

- [ ] **Step 3: Verify — no tag**

  From repo root:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"
  ```
  Expected: tests build, all fast tests pass, exit 0. Output matches `.\build.ps1 run-tests`.

- [ ] **Step 4: Verify — with tag**

  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [tactical]"
  ```
  Expected: only `[tactical]` tests run, pass, exit 0.

- [ ] **Step 5: Commit**

  ```
  git add StratChessEvolved/Scripts/Run-Tests.ps1
  git commit -m "feat(scripts): add Run-Tests.ps1 wrapper with canonical single-line invocation"
  ```

---

### Task 3: Create `StratChessEvolved/Scripts/Validate-PreCommit.ps1`

**Files:**
- Create: `StratChessEvolved/Scripts/Validate-PreCommit.ps1`

- [ ] **Step 1: Write the script**

  Create `StratChessEvolved/Scripts/Validate-PreCommit.ps1`:
  ```powershell
  <#
  .SYNOPSIS
      Pre-commit validation: FEN check + fast test suite.

  .DESCRIPTION
      1. Verifies StratChessEvolved/game_settings.json contains the chess starting position FEN.
      2. Builds and runs the fast test suite (excludes [slow]).
      Both checks always run before exit so all failures are reported at once.
      Exits with code 1 if any check fails.

  .WHEN TO USE
      Before every git commit. Always run this before Validate-PrePR.ps1.

  .HOW TO INVOKE (from bash, cmd, or PowerShell)
      cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PreCommit.ps1"

  .NOTES
      Must be invoked with -File, not dot-sourced. $PSScriptRoot is $null under dot-source,
      which causes Split-Path to throw under Set-StrictMode -Version Latest.
  #>

  Set-StrictMode -Version Latest
  # Do NOT set $ErrorActionPreference = 'Stop' — this script deliberately accumulates
  # failures across both checks before exiting. Each step checks $LASTEXITCODE directly.

  $GameDir      = Split-Path $PSScriptRoot -Parent
  $RepoRoot     = Split-Path $GameDir -Parent
  $buildScript  = Join-Path $RepoRoot 'build.ps1'
  $settingsFile = Join-Path $GameDir 'game_settings.json'
  $startingFen  = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
  $failed       = $false

  # --- Step 1: FEN check ---
  Write-Host "`n==> Checking FEN in game_settings.json" -ForegroundColor Cyan
  $content = Get-Content $settingsFile -Raw
  if ($content -notmatch [regex]::Escape($startingFen)) {
      Write-Host "FAIL: game_settings.json does not contain the starting FEN." -ForegroundColor Red
      Write-Host "      Reset the FEN to: $startingFen" -ForegroundColor Yellow
      $failed = $true
  } else {
      Write-Host "PASS: FEN is at starting position." -ForegroundColor Green
  }

  # --- Step 2: Fast test suite ---
  Write-Host "`n==> Running fast test suite" -ForegroundColor Cyan
  & $buildScript run-tests
  if ($LASTEXITCODE -ne 0) {
      Write-Host "FAIL: Test suite reported failures." -ForegroundColor Red
      $failed = $true
  } else {
      Write-Host "PASS: All fast tests passed." -ForegroundColor Green
  }

  # --- Summary ---
  Write-Host ""
  if ($failed) {
      Write-Host "Pre-commit validation FAILED. Fix issues before committing." -ForegroundColor Red
      exit 1
  } else {
      Write-Host "Pre-commit validation PASSED." -ForegroundColor Green
      exit 0
  }
  ```

- [ ] **Step 2: Verify — happy path (correct FEN)**

  Confirm `StratChessEvolved/game_settings.json` has the starting FEN. Then run:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PreCommit.ps1"
  ```
  Expected: FEN check PASS, tests PASS, final message "Pre-commit validation PASSED.", exit 0.

- [ ] **Step 3: Verify — wrong FEN (failure path)**

  In `StratChessEvolved/game_settings.json`, temporarily change the FEN string to any other value.
  Run:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PreCommit.ps1"
  ```
  Expected: FEN check prints FAIL with reset instruction, tests still run (accumulate-failures), final message "Pre-commit validation FAILED.", exit 1.
  **Immediately revert the FEN change.**

- [ ] **Step 4: Commit**

  ```
  git add StratChessEvolved/Scripts/Validate-PreCommit.ps1
  git commit -m "feat(scripts): add Validate-PreCommit.ps1 — FEN check and fast test suite"
  ```

---

### Task 4: Create `StratChessEvolved/Scripts/Validate-PrePR.ps1`

**Files:**
- Create: `StratChessEvolved/Scripts/Validate-PrePR.ps1`

- [ ] **Step 1: Ensure `StratChessEvolved/logs/` exists**

  Self-play logging requires `logs/` to pre-exist — spdlog silently fails if it doesn't.
  The script creates it if absent. Verify `StratChessEvolved/logs/` exists on disk; if not, create it:
  ```
  mkdir StratChessEvolved\logs
  ```
  (The script will also create it automatically — this step is just a sanity check.)

- [ ] **Step 2: Verify `game_settings.json` has AIPerplex on both sides**

  Open `StratChessEvolved/game_settings.json` and confirm both player entries have `"type": 6`.
  If not, set both to `"type": 6` temporarily for self-play (restore original after Task 4 if you changed it).

- [ ] **Step 3: Write the script**

  Create `StratChessEvolved/Scripts/Validate-PrePR.ps1`:
  ```powershell
  <#
  .SYNOPSIS
      Pre-PR validation: full parallel build + extended test suite + self-play.

  .DESCRIPTION
      1. Builds main solution and test project in parallel.
      2. Runs the full extended test suite (including [slow]).
      3. Runs a headless AIPerplex vs AIPerplex self-play game (60s timeout).
      All three checks always run before exit so all failures are visible at once.
      Exits with code 1 if any check fails. Run Validate-PreCommit.ps1 first.

  .WHEN TO USE
      Before opening a pull request.

  .HOW TO INVOKE (from bash, cmd, or PowerShell)
      cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1"

  .NOTES
      Prerequisite: game_settings.json must have "type": 6 for both players (AIPerplex vs AIPerplex).
      Must be invoked with -File, not dot-sourced. $PSScriptRoot is $null under dot-source.
  #>

  Set-StrictMode -Version Latest
  # Do NOT set $ErrorActionPreference = 'Stop' — this script runs all three checks before
  # exiting so the summary table is always printed. Each step checks $LASTEXITCODE directly.

  $GameDir     = Split-Path $PSScriptRoot -Parent
  $RepoRoot    = Split-Path $GameDir -Parent
  $buildScript = Join-Path $RepoRoot 'build.ps1'
  $gameExe     = Join-Path $RepoRoot 'x64\Release\StratChessEvolved.exe'
  $logsDir     = Join-Path $GameDir 'logs'
  $outFile     = Join-Path $GameDir 'pre_pr_selfplay_out.txt'
  $checkResults = [ordered]@{}

  # --- Step 1: Full parallel build ---
  Write-Host "`n==> Full build (main + tests in parallel)" -ForegroundColor Cyan
  & $buildScript all
  $checkResults['Full build'] = if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' }

  # --- Step 2: Extended test suite ---
  Write-Host "`n==> Extended test suite (including [slow])" -ForegroundColor Cyan
  & $buildScript extended-tests
  $checkResults['Extended tests'] = if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' }

  # --- Step 3: Self-play ---
  Write-Host "`n==> Self-play (AIPerplex vs AIPerplex, 60s timeout)" -ForegroundColor Cyan

  # Ensure logs/ exists — spdlog silently fails without it
  if (-not (Test-Path $logsDir)) {
      New-Item -ItemType Directory -Path $logsDir | Out-Null
      Write-Host "  Created missing logs/ directory." -ForegroundColor DarkGray
  }

  Push-Location $GameDir
  try {
      if (Test-Path $outFile) { Remove-Item $outFile }
      $proc   = Start-Process $gameExe -PassThru -NoNewWindow -RedirectStandardOutput $outFile
      $exited = $proc.WaitForExit(60000)
      if (-not $exited) { $proc.Kill() }

      $output    = if (Test-Path $outFile) { Get-Content $outFile -Raw } else { '' }
      $moveCount = ([regex]::Matches($output, 'GetMove complete:')).Count

      # Require at least 2 moves (one per side). Explicit parentheses guard against
      # PowerShell's left-to-right -and/-or precedence. $exited alone is not sufficient
      # — a crash that exits cleanly also sets $exited = $true.
      $gameTerminated = $output -match 'checkmate|stalemate|draw'
      if (($moveCount -ge 2) -and ($gameTerminated -or $exited)) {
          Write-Host "PASS: $moveCount move(s) logged; game completed." -ForegroundColor Green
          $checkResults['Self-play'] = 'PASS'
      } else {
          Write-Host "FAIL: $moveCount move(s) logged. Expected at least 2 and game to terminate." -ForegroundColor Red
          Write-Host "Output tail:" -ForegroundColor Yellow
          $output -split "`n" | Select-Object -Last 10 | ForEach-Object { Write-Host "  $_" }
          $checkResults['Self-play'] = 'FAIL'
      }
  } finally {
      Pop-Location
      if (Test-Path $outFile) { Remove-Item $outFile }
  }

  # --- Summary table ---
  Write-Host "`n--- Pre-PR Validation Summary ---" -ForegroundColor Cyan
  $anyFailed = $false
  foreach ($check in $checkResults.Keys) {
      $status = $checkResults[$check]
      $color  = if ($status -eq 'PASS') { 'Green' } else { 'Red' }
      Write-Host ("  {0,-22} {1}" -f $check, $status) -ForegroundColor $color
      if ($status -ne 'PASS') { $anyFailed = $true }
  }
  Write-Host ""

  if ($anyFailed) {
      Write-Host "Pre-PR validation FAILED." -ForegroundColor Red
      exit 1
  } else {
      Write-Host "Pre-PR validation PASSED." -ForegroundColor Green
      exit 0
  }
  ```

- [ ] **Step 4: Verify — happy path**

  Run from repo root:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1"
  ```
  Expected:
  - Full build: PASS
  - Extended tests: PASS
  - Self-play: PASS (at least 2 `GetMove complete:` lines, game terminates)
  - Summary table printed, exit 0

- [ ] **Step 5: Verify — negative path (exit-code propagation)**

  Introduce a deliberate syntax error in `StratEngine/Eval.cpp` line 1:
  ```cpp
  THIS_IS_NOT_CPP
  ```
  Run:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1"
  ```
  Expected: summary table shows `Full build   FAIL`, remaining checks still run (accumulate-failures pattern), final message "Pre-PR validation FAILED.", exit 1.
  **Immediately revert the syntax error before continuing.**

- [ ] **Step 6: Commit**

  ```
  git add StratChessEvolved/Scripts/Validate-PrePR.ps1
  git commit -m "feat(scripts): add Validate-PrePR.ps1 — parallel build, extended tests, self-play"
  ```

---

## Chunk 2: Skills + CLAUDE.md

### Task 5: Update skills and CLAUDE.md

**Files:**
- Modify: `.claude/skills/run-tests/SKILL.md`
- Modify: `.claude/skills/self-play-validate/SKILL.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Replace `.claude/skills/run-tests/SKILL.md` body**

  Keep the frontmatter (`---` block) unchanged. Replace everything after the closing `---` with:

  > **Path note:** The canonical invocation below uses `StratChessEvolved\Scripts\Run-Tests.ps1` (fully-qualified from repo root). The spec used `Scripts\\Run-Tests.ps1` which is broken from repo root — the plan's path is correct.
  ```markdown
  Build the test project and run with an optional Catch2 tag filter.

  ## Canonical invocation (works from bash, cmd, or PowerShell)

  No tag — full fast suite (excludes [slow]):
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"
  ```

  With tag filter:
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [tactical]"
  ```

  ## Notes
  - Run from worktree root (not a subdirectory)
  - Available tags: [sort] [search] [tactical] [perft] [tt] [eval] [repetition] [formatter] [board] [time_mgr]
  - Binary is under `StratChessTests\x64\Release\` (not `x64\Release\`) — the script handles this
  - Fallback if Scripts/ unavailable: `cmd.exe /c "powershell -ExecutionPolicy Bypass -File build.ps1 run-tests"`
  ```

- [ ] **Step 2: Replace `.claude/skills/self-play-validate/SKILL.md` body**

  Keep the frontmatter unchanged. Replace everything after the closing `---` with:

  > **Path note:** Same as Step 1 — use `StratChessEvolved\Scripts\Validate-PrePR.ps1` (fully-qualified), not `Scripts\\Validate-PrePR.ps1`.
  ```markdown
  Run full pre-PR validation: parallel build, extended tests, and AIPerplex vs AIPerplex self-play.

  ## Canonical invocation (works from bash, cmd, or PowerShell)

  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1"
  ```

  ## What it checks
  1. Full build — main solution + test project in parallel
  2. Extended test suite — all tiers including [slow]
  3. Self-play — AIPerplex vs AIPerplex, 60s timeout; verifies `GetMove complete:` per move (≥2) and natural game termination

  ## Notes
  - Run `Validate-PreCommit.ps1` first (FEN check and fast tests live there)
  - `game_settings.json` must have `"type": 6` for both players before running
  - For PlayerAI/PlayerBase base class changes, also verify AIAgent self-play manually (`"type": 3` for both sides)
  - Summary table always printed even if earlier checks fail
  ```

- [ ] **Step 3: Update `CLAUDE.md` — build script table**

  In the `### Build script (preferred)` section, replace the entire code block:

  Before:
  ```powershell
  .\build.ps1               # build main solution + test project (Release|x64)
  .\build.ps1 main          # main solution only
  .\build.ps1 tests         # test project only
  .\build.ps1 run-tests     # build tests then run all of them
  .\build.ps1 run-tests "[formatter]"  # build tests then run a single tag
  .\build.ps1 all -Config Debug        # debug build of both
  ```
  After:
  ```powershell
  .\build.ps1               # build main + tests in parallel (Release|x64)
  .\build.ps1 main          # main solution only
  .\build.ps1 tests         # test project only
  .\build.ps1 run-tests     # build tests then run all of them
  .\build.ps1 run-tests "[formatter]"  # build tests then run a single tag
  .\build.ps1 all                      # build main + tests in parallel (Release|x64)
  .\build.ps1 all -Config Debug        # parallel debug build of both
  ```

- [ ] **Step 4: Update `CLAUDE.md` — add Validation scripts subsection**

  After the `### Build script (preferred)` block (before `### MSBuild invocation (fallback / raw)`), insert:

  ```markdown
  ### Validation scripts
  Scripts in `StratChessEvolved/Scripts/` handle working-directory and MSBuild path resolution
  internally. Invoke via the canonical pattern from any shell (bash, cmd, PowerShell):
  ```
  cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\<name>.ps1"
  ```

  | Script | When to use |
  |---|---|
  | `Scripts\Run-Tests.ps1 [tag]` | Any test verification — optional tag filter |
  | `Scripts\Validate-PreCommit.ps1` | Before every commit — FEN check + fast tests |
  | `Scripts\Validate-PrePR.ps1` | Before opening a PR — full build + extended tests + self-play |

  Scripts must be invoked with `-File`, not dot-sourced (`$PSScriptRoot` is `$null` under dot-source).

  ```

- [ ] **Step 5: Verify skills are updated correctly**

  Read back both skill files and confirm:
  - No multi-line shell logic blocks remain (single-line `cmd.exe` invocation fences are expected and correct)
  - Canonical `cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\..."` line is present
  - Notes section is present

- [ ] **Step 6: Commit**

  ```
  git add .claude/skills/run-tests/SKILL.md
  git add .claude/skills/self-play-validate/SKILL.md
  git add CLAUDE.md
  git commit -m "docs: update run-tests and self-play skills + CLAUDE.md to point to Scripts/"
  ```

---

## Validation Checklist (end-to-end)

Run these after all tasks complete to confirm the full chain works:

- [ ] `cmd.exe /c "powershell -ExecutionPolicy Bypass -File build.ps1 all"` — exits 0, parallel output
- [ ] `cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1"` — all fast tests pass
- [ ] `cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Tests.ps1 [tactical]"` — tag filter works
- [ ] `cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PreCommit.ps1"` — PASS with correct FEN
- [ ] Temporarily corrupt FEN, re-run PreCommit — FAIL with clear message, tests still run
- [ ] `cmd.exe /c "powershell -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Validate-PrePR.ps1"` — all three checks PASS, summary table printed
- [ ] Introduce deliberate build error in Eval.cpp, run Validate-PrePR.ps1 — `Full build` FAIL in summary, extended tests and self-play still run, exit 1; revert error
- [ ] Invoke `run-tests` skill — canonical invocation confirmed in skill body
- [ ] Invoke `self-play-validate` skill — canonical invocation confirmed in skill body
