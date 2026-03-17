# Build & Test Automation Improvements — Design Spec

**Date**: 2026-03-16
**Status**: Approved

---

## Goal

Eliminate the two most common subagent build/test failure modes — shell syntax confusion and MSBuild path discovery errors — and add structured pre-commit and pre-PR validation scripts. Outcome: subagents call a single canonical command and get a reliable result; validation is automated and consistent.

**Scope limits**: No changes to engine source, test logic, or evaluation. No new test cases. No changes to the CI pipeline.

---

## Problem Statement

Subagents fail to trigger builds consistently due to:
1. Shell syntax confusion (bash vs PowerShell vs cmd.exe) — most common
2. MSBuild path hard-coding or discovery failures — most common
3. Wrong working directory — less common

Additionally, pre-commit and pre-PR validation steps (FEN reset check, regression tests, self-play) are informal — they rely on Claude remembering to do them.

---

## Design Decisions

**Scripts as the fix layer, not instructions**: Instructions drift; scripts don't. Putting MSBuild discovery and working-directory resolution inside scripts means any shell can invoke them via one canonical `cmd.exe /c powershell...` pattern. Skills and CLAUDE.md become thin pointers.

**build.ps1 remains the single build entry point**: No new Build-Main.ps1 or Build-Tests.ps1. The `all` verb in build.ps1 is updated to run both projects in parallel (currently sequential). All new scripts delegate to build.ps1.

**FEN check only in pre-commit**: Validate-PrePR.ps1 skips the FEN check — Validate-PreCommit.ps1 always runs first in the workflow.

**Self-play lives in Validate-PrePR.ps1 only**: Too slow for pre-commit. Pre-commit runs the fast test suite only.

---

## Files Changed

| File | Change type |
|---|---|
| `build.ps1` | Modified — `all` verb made parallel |
| `Scripts/Run-Tests.ps1` | New — thin wrapper around `build.ps1 run-tests [tag]` |
| `Scripts/Validate-PreCommit.ps1` | New — FEN check + fast test suite |
| `Scripts/Validate-PrePR.ps1` | New — full build + extended tests + self-play |
| `.claude/skills/run-tests/SKILL.md` | Modified — body replaced with canonical invocation |
| `.claude/skills/self-play-validate/SKILL.md` | Modified — body replaced with canonical invocation |
| `CLAUDE.md` | Modified — build section updated; `all` verb documented |

---

## Step-by-Step Changes

### 1. Update `build.ps1` — parallel `all`

Replace the sequential `all` case:
```powershell
'all' {
    Invoke-MSBuild $Sln -Parallel
    Invoke-MSBuild $TestProj -Parallel
}
```
With parallel PowerShell jobs (exit codes captured inside each job and returned as output — checking `.State` after `Receive-Job` is unreliable, and `State -eq 'Failed'` does not fire when MSBuild exits non-zero):
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
Also update `.SYNOPSIS` docs: `all` line reads "Build main and tests in parallel".

### 2. Create `Scripts/Run-Tests.ps1`

```powershell
<#
.SYNOPSIS
    Build the test project and run the test suite.

.DESCRIPTION
    Thin wrapper around build.ps1 run-tests. Handles working-directory resolution
    so it can be called from any shell context.

.WHEN TO USE
    Any time you want to build and run tests — with or without a tag filter.

.HOW TO INVOKE (from bash/cmd)
    cmd.exe /c "powershell -ExecutionPolicy Bypass -File Scripts\\Run-Tests.ps1"
    cmd.exe /c "powershell -ExecutionPolicy Bypass -File Scripts\\Run-Tests.ps1 [tactical]"

.PARAMETER Tag
    Optional Catch2 tag filter, e.g. "[tactical]" or "[eval]". Omit for full fast suite.
#>
param([string]$Tag = '')

$RepoRoot = Split-Path $PSScriptRoot -Parent
$buildScript = Join-Path $RepoRoot 'build.ps1'

if ($Tag) {
    & $buildScript run-tests $Tag
} else {
    & $buildScript run-tests
}
exit $LASTEXITCODE
```

### 3. Create `Scripts/Validate-PreCommit.ps1`

```powershell
<#
.SYNOPSIS
    Pre-commit validation: FEN check + fast test suite.

.DESCRIPTION
    1. Verifies game_settings.json FEN is at the chess starting position.
    2. Builds and runs the fast test suite (excludes [slow]).
    Exits with code 1 on any failure.

.WHEN TO USE
    Before every git commit. Run this before Validate-PrePR.ps1.

.HOW TO INVOKE (from bash/cmd)
    cmd.exe /c "powershell -ExecutionPolicy Bypass -File Scripts\\Validate-PreCommit.ps1"
#>

Set-StrictMode -Version Latest
# Do NOT set $ErrorActionPreference = 'Stop' here — this script deliberately
# accumulates failures across both checks before exiting, so each step must
# continue even when the previous one fails. Exit codes are checked via $LASTEXITCODE.

$RepoRoot       = Split-Path $PSScriptRoot -Parent
$buildScript    = Join-Path $RepoRoot 'build.ps1'
$settingsFile   = Join-Path $RepoRoot 'StratChessEvolved\game_settings.json'
$startingFen    = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
$failed         = $false

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

### 4. Create `Scripts/Validate-PrePR.ps1`

```powershell
<#
.SYNOPSIS
    Pre-PR validation: full build + extended tests + self-play.

.DESCRIPTION
    1. Builds main solution and test project in parallel.
    2. Runs the full extended test suite (including [slow]).
    3. Runs a headless AIPerplex vs AIPerplex self-play game (60s timeout).
    Exits with code 1 on any failure. Run Validate-PreCommit.ps1 first.

.WHEN TO USE
    Before opening a pull request.

.HOW TO INVOKE (from bash/cmd)
    cmd.exe /c "powershell -ExecutionPolicy Bypass -File Scripts\\Validate-PrePR.ps1"
#>

Set-StrictMode -Version Latest
# Do NOT set $ErrorActionPreference = 'Stop' here — this script runs all three
# checks before exiting so the summary table is always printed. Each step checks
# $LASTEXITCODE directly rather than relying on terminating errors.

$RepoRoot    = Split-Path $PSScriptRoot -Parent
$buildScript = Join-Path $RepoRoot 'build.ps1'
$gameDir     = Join-Path $RepoRoot 'StratChessEvolved'
$gameExe     = Join-Path $RepoRoot 'x64\Release\StratChessEvolved.exe'
$outFile     = Join-Path $gameDir 'pre_pr_selfplay_out.txt'
$results     = [ordered]@{}

# --- Step 1: Full parallel build ---
Write-Host "`n==> Full build (main + tests in parallel)" -ForegroundColor Cyan
& $buildScript all
$results['Full build'] = if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' }

# --- Step 2: Extended test suite ---
Write-Host "`n==> Extended test suite (including [slow])" -ForegroundColor Cyan
& $buildScript extended-tests
$results['Extended tests'] = if ($LASTEXITCODE -eq 0) { 'PASS' } else { 'FAIL' }

# --- Step 3: Self-play ---
Write-Host "`n==> Self-play (AIPerplex vs AIPerplex, 60s timeout)" -ForegroundColor Cyan
Push-Location $gameDir
try {
    if (Test-Path $outFile) { Remove-Item $outFile }
    $proc = Start-Process $gameExe -PassThru -NoNewWindow -RedirectStandardOutput $outFile
    $exited = $proc.WaitForExit(60000)
    if (-not $exited) { $proc.Kill() }

    $output = if (Test-Path $outFile) { Get-Content $outFile -Raw } else { '' }
    $moveCount = ([regex]::Matches($output, 'GetMove complete:')).Count

    # Require at least 2 moves (one per side) — $exited alone is not sufficient
    # because a crash also sets $exited = $true. Explicit parentheses guard against
    # PowerShell's -and/-or precedence (left to right, no short-circuit grouping).
    $gameTerminated = $output -match 'checkmate|stalemate|draw'
    if (($moveCount -ge 2) -and ($gameTerminated -or $exited)) {
        Write-Host "PASS: $moveCount move(s) logged; game completed." -ForegroundColor Green
        $results['Self-play'] = 'PASS'
    } else {
        Write-Host "FAIL: $moveCount move(s) logged. Expected game to complete." -ForegroundColor Red
        Write-Host "Output tail:" -ForegroundColor Yellow
        $output -split "`n" | Select-Object -Last 10 | ForEach-Object { Write-Host "  $_" }
        $results['Self-play'] = 'FAIL'
    }
} finally {
    Pop-Location
    if (Test-Path $outFile) { Remove-Item $outFile }
}

# --- Summary table ---
Write-Host "`n--- Pre-PR Validation Summary ---" -ForegroundColor Cyan
$anyFailed = $false
foreach ($check in $results.Keys) {
    $status = $results[$check]
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

### 5. Update `.claude/skills/run-tests/SKILL.md`

Replace current body with:

```markdown
Build the test project and run with an optional Catch2 tag filter.

## Canonical invocation (works from bash, cmd, PowerShell)

No tag (full fast suite):
    cmd.exe /c "powershell -ExecutionPolicy Bypass -File Scripts\\Run-Tests.ps1"

With tag filter:
    cmd.exe /c "powershell -ExecutionPolicy Bypass -File Scripts\\Run-Tests.ps1 [tactical]"

## Notes
- Run from worktree root
- Binary under StratChessTests\x64\Release\ (not x64\Release\)
- Use build.ps1 run-tests directly only if Scripts\Run-Tests.ps1 is unavailable
- Available tags: [sort] [search] [tactical] [perft] [tt] [eval] [repetition] [formatter] [board] [time_mgr]
```

### 6. Update `.claude/skills/self-play-validate/SKILL.md`

Replace current body with:

```markdown
Run full pre-PR validation: parallel build, extended tests, and AIPerplex vs AIPerplex self-play.

## Canonical invocation (works from bash, cmd, PowerShell)

    cmd.exe /c "powershell -ExecutionPolicy Bypass -File Scripts\\Validate-PrePR.ps1"

## What it checks
1. Full build (main + tests in parallel)
2. Extended test suite (all tiers including [slow])
3. Self-play: AIPerplex vs AIPerplex, 60s timeout — verifies GetMove complete: per move and natural game termination

## Notes
- Run Validate-PreCommit.ps1 first (FEN check lives there)
- Script runs from Scripts/ — no working directory setup needed
- For PlayerAI/PlayerBase changes, also verify AIAgent self-play manually (type: 3)
```

### 7. Update `CLAUDE.md` — build section

- Change `all` description from "Build main then tests (default)" to "Build main and tests in parallel (default)"
- Add a "Validation scripts" subsection after the build table:

```markdown
### Validation scripts
Scripts live in `Scripts/` and handle working-directory and MSBuild path resolution internally.
Invoke via `cmd.exe /c "powershell -ExecutionPolicy Bypass -File Scripts\\<name>.ps1"` from any shell.

| Script | When to use |
|---|---|
| `Scripts\Run-Tests.ps1 [tag]` | Any test verification |
| `Scripts\Validate-PreCommit.ps1` | Before every commit — FEN check + fast tests |
| `Scripts\Validate-PrePR.ps1` | Before opening a PR — full build + extended tests + self-play |
```

---

## Validation Plan

1. `build.ps1 all` — verify both projects build and that output shows parallel job completion
1b. Introduce a deliberate syntax error in a source file, run `build.ps1 all`, verify exit code is 1 and the error is reported; revert the change. (Validates that parallel job exit codes propagate correctly.)
2. `Scripts\Run-Tests.ps1` — verify pass output matches current test run
3. `Scripts\Run-Tests.ps1 [tactical]` — verify tag filter passes through
4. `Scripts\Validate-PreCommit.ps1` with correct FEN — expect PASS
5. `Scripts\Validate-PreCommit.ps1` with wrong FEN — expect FAIL with clear message
6. `Scripts\Validate-PrePR.ps1` — expect all three checks PASS and summary table printed
7. Invoke `run-tests` skill — verify canonical invocation works end-to-end
8. Invoke `self-play-validate` skill — verify canonical invocation works end-to-end

---

## Key Correctness Properties

- `build.ps1 all` exit code must be 1 if either parallel job fails
- `Validate-PreCommit.ps1` must exit 1 (not just print FAIL) when FEN is wrong — downstream scripts check exit code
- `Validate-PrePR.ps1` must clean up `pre_pr_selfplay_out.txt` even on failure (finally block)
- Self-play check must not hang — 60s `WaitForExit` timeout + `Kill()` fallback
- Scripts must be runnable from any working directory — all paths resolved relative to `$PSScriptRoot`
- Scripts must be invoked with `-File`, not dot-sourced — `$PSScriptRoot` is `$null` under dot-source or interactive paste, which causes `Split-Path $null` to throw under `Set-StrictMode -Version Latest`
