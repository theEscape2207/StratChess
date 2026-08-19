<#
.SYNOPSIS
    Pre-PR validation: full parallel build + extended test suite + self-play.

.DESCRIPTION
    1. Builds main solution and test project in parallel.
    2. Runs the full extended test suite (including [slow]).
    3. Runs the exe tactical suite in stability mode (10 consecutive runs of
       Tests/tactical_test_cases.json, 90% threshold per run + no pass/fail flips).
    4. Runs a headless AIPerplex vs AIPerplex self-play game (60s timeout).
    All four checks always run before exit so all failures are visible at once.
    Exits with code 1 if any check fails. Run Validate-PreCommit.ps1 first.

.WHEN TO USE
    Before opening a pull request.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\StratChessEvolved\Scripts\Validate-PrePR.ps1

.NOTES
    Prerequisite: game_settings.json must have "type": 6 for both players (AIPerplex vs AIPerplex).
    Must be invoked with -File, not dot-sourced. $PSScriptRoot is $null under dot-source.
#>

param(
    # Run every gate regardless of what the diff contains. Use when your judgement
    # disagrees with the classifier.
    [switch]$Force,
    # Ref the diff is computed against when choosing a validation tier.
    [string]$BaseRef = 'origin/main'
)

Set-StrictMode -Version Latest
# Do NOT set $ErrorActionPreference = 'Stop' — this script runs all three checks before
# exiting so the summary table is always printed. Each step checks $LASTEXITCODE directly.

$GameDir     = Split-Path $PSScriptRoot -Parent
$RepoRoot    = Split-Path $GameDir -Parent
$buildScript = Join-Path $RepoRoot 'build.ps1'
# -AllowMissing: this resolves the path before Step 1 builds it, so on a fresh
# worktree nothing is there yet.
$gameExe     = & (Join-Path $PSScriptRoot 'Get-BuildArtifact.ps1') -AllowMissing
$logsDir     = Join-Path $GameDir 'logs'
$outFile     = Join-Path $GameDir 'pre_pr_selfplay_out.txt'
$aiLogFile   = Join-Path $logsDir 'aiperplex.log'
$checkResults = [ordered]@{}

# --- Scope the run to what actually changed (issue #124) ---------------------
# Full build + extended [slow] tests + a 10-run tactical suite + self-play cannot
# catch anything a documentation edit or a measurement script could break, and
# running them anyway burns minutes for a guaranteed pass. Get-ChangeTier.ps1 is
# the single source of truth for this decision, shared with CI
# (.github/workflows/build-and-test.yml) so the two definitions cannot drift.
# It fails closed: anything unrecognised classifies as Engine and gets the full run.
$tierScript = Join-Path $PSScriptRoot 'Get-ChangeTier.ps1'
$change = & $tierScript -BaseRef $BaseRef

if ($Force) {
    Write-Host "`n==> Change tier: $($change.Tier) -- overridden by -Force, running every gate." -ForegroundColor Yellow
} else {
    Write-Host "`n==> Change tier: $($change.Tier) (decided by: $($change.DecidingFile))" -ForegroundColor Cyan
}

if (-not $Force -and $change.Tier -eq 'Docs') {
    Write-Host 'Docs-only diff -- SKIPPING full build, extended tests, tactical suite and self-play.' -ForegroundColor Green
    Write-Host "The pre-commit hook's fast-test pass is sufficient for documentation changes." -ForegroundColor Green
    Write-Host 'Re-run with -Force to validate anyway.'
    Write-Host ''
    Write-Host 'Pre-PR validation PASSED (docs-only fast path).' -ForegroundColor Green
    exit 0
}

if (-not $Force -and $change.Tier -eq 'Tooling') {
    Write-Host 'Engine-inert tooling diff -- SKIPPING full build, extended tests, tactical suite and self-play.' -ForegroundColor Green
    Write-Host 'These scripts are never compiled and never invoked by the engine, so no' -ForegroundColor Green
    Write-Host 'build/test gate can observe the change. Syntax-checking them instead.' -ForegroundColor Green
    Write-Host 'Re-run with -Force to validate anyway.'
    Write-Host ''

    $syntaxFailed = $false
    foreach ($f in $change.ChangedFiles) {
        if ($f -notlike '*.ps1') { continue }
        $full = Join-Path $RepoRoot $f
        if (-not (Test-Path $full)) { continue }
        $tokens = $null; $errors = $null
        [System.Management.Automation.Language.Parser]::ParseInput(
            (Get-Content $full -Raw), [ref]$tokens, [ref]$errors) | Out-Null
        if ($errors.Count -gt 0) {
            Write-Host "  FAIL  $f" -ForegroundColor Red
            $errors | ForEach-Object { Write-Host "        $($_.Message)" -ForegroundColor Red }
            $syntaxFailed = $true
        } else {
            Write-Host "  PASS  $f (syntax)" -ForegroundColor Green
        }
    }
    Write-Host ''
    if ($syntaxFailed) { Write-Host 'Pre-PR validation FAILED (script syntax).' -ForegroundColor Red; exit 1 }
    Write-Host 'Pre-PR validation PASSED (tooling fast path).' -ForegroundColor Green
    exit 0
}

Write-Host 'Running the full gate set.' -ForegroundColor Cyan

# --- Step 0a: build wrapper self-test ---
Write-Host "`n==> Build wrapper self-test" -ForegroundColor Cyan
$buildSelfTestFailed = $false
try   { & $buildScript -SelfTest }
catch { $buildSelfTestFailed = $true; Write-Host "Build self-test threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $buildSelfTestFailed = $true }
$checkResults['Build wrapper self-test'] = if ($buildSelfTestFailed) { 'FAIL' } else { 'PASS' }

# --- Step 0b: clang-format ---
# CI blocks a pull request whose sources are not formatted, so the same answer has
# to be reachable before pushing -- otherwise this is the only gate in the repo that
# can only be discovered after a push. It runs first because it is by far the
# cheapest: seconds against several minutes for the build.
Write-Host "`n==> clang-format (issue #175)" -ForegroundColor Cyan
$lintScript = Join-Path $PSScriptRoot 'Run-Lint.ps1'
$lintFailed = $false
try   { & $lintScript -Check Format -BaseRef $BaseRef }
catch { $lintFailed = $true; Write-Host "Lint threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $lintFailed = $true }
$checkResults['clang-format'] = if ($lintFailed) { 'FAIL' } else { 'PASS' }

# --- Step 0c: blame-ignore coverage ---
# A clang-format configuration change re-runs the formatter over the whole tree,
# so it lands as a commit that rewrites most files without altering a line of
# code. Unless it is recorded in .git-blame-ignore-revs it buries the real
# history of every file it touched -- and nothing else in the pipeline notices.
# Pure git, so it costs nothing.
Write-Host "`n==> Blame-ignore coverage (issue #175)" -ForegroundColor Cyan
$blameFailed = $false
try   { & $lintScript -Check BlameIgnore -BaseRef $BaseRef }
catch { $blameFailed = $true; Write-Host "Blame-ignore check threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $blameFailed = $true }
$checkResults['Blame-ignore'] = if ($blameFailed) { 'FAIL' } else { 'PASS' }

# --- Step 0d: workflow job timeouts ---
# The classify job enforces this, so without it here a workflow edit that forgets
# timeout-minutes is only discoverable after a push -- the same asymmetry the
# clang-format step above exists to remove. Pure text, so it costs nothing.
Write-Host "`n==> Workflow job timeouts" -ForegroundColor Cyan
$timeoutScript = Join-Path $PSScriptRoot 'Test-WorkflowTimeouts.ps1'
$timeoutFailed = $false
try   { & $timeoutScript }
catch { $timeoutFailed = $true; Write-Host "Timeout guard threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $timeoutFailed = $true }
$checkResults['Workflow timeouts'] = if ($timeoutFailed) { 'FAIL' } else { 'PASS' }

# --- Step 1: Full parallel build ---
Write-Host "`n==> Full build (main + tests in parallel)" -ForegroundColor Cyan
# build.ps1 sets $ErrorActionPreference='Stop' internally and calls Write-Error on failure,
# which propagates a terminating error to this script via &. Wrap in try/catch so all three
# checks always run. Track failure via $buildFailed rather than $LASTEXITCODE — when the
# terminating error is caught, $LASTEXITCODE reflects the last native process (cmake/ninja), not
# build.ps1's exit code, so it can't be relied on for the PASS/FAIL decision.
$buildFailed = $false
try   { & $buildScript all }
catch { $buildFailed = $true; Write-Host "Build threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $buildFailed = $true }
$checkResults['Full build'] = if ($buildFailed) { 'FAIL' } else { 'PASS' }

# --- Step 1b: fast clang-tidy Gate ---
# Run after the build so a fresh worktree has the shipping clang-cl compilation
# database the shared local/CI runner requires.
Write-Host "`n==> clang-tidy Gate (issues #175/#284)" -ForegroundColor Cyan
$tidyFailed = $false
try   { & $lintScript -Check Tidy -Profile Gate -BaseRef $BaseRef }
catch { $tidyFailed = $true; Write-Host "clang-tidy Gate threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $tidyFailed = $true }
$checkResults['clang-tidy Gate'] = if ($tidyFailed) { 'FAIL' } else { 'PASS' }

# --- Step 2: Extended test suite ---
Write-Host "`n==> Extended test suite (including [slow])" -ForegroundColor Cyan
$extFailed = $false
try   { & $buildScript extended-tests }
catch { $extFailed = $true; Write-Host "Extended-tests threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $extFailed = $true }
$checkResults['Extended tests'] = if ($extFailed) { 'FAIL' } else { 'PASS' }

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

# --- Step 4: Self-play ---
Write-Host "`n==> Self-play (AIPerplex vs AIPerplex, 60s timeout)" -ForegroundColor Cyan

# Ensure logs/ exists — spdlog silently fails without it
if (-not (Test-Path $logsDir)) {
    New-Item -ItemType Directory -Path $logsDir | Out-Null
    Write-Host "  Created missing logs/ directory." -ForegroundColor DarkGray
}

Push-Location $GameDir
try {
    if (Test-Path $outFile) { Remove-Item $outFile }
    # Truncate aiperplex.log before run so we only count moves from this game
    if (Test-Path $aiLogFile) { Clear-Content $aiLogFile }
    $proc   = Start-Process $gameExe -ArgumentList 'game' -PassThru -NoNewWindow -RedirectStandardOutput $outFile
    $exited = $proc.WaitForExit(60000)
    if (-not $exited) { $proc.Kill() }

    # GetMove complete: is written to logs/aiperplex.log by s_logger (file sink).
    # RedirectStandardOutput does not capture spdlog's stdout_color_sink on Windows
    # (spdlog uses WriteConsoleW which bypasses the C-runtime stdout redirect).
    $logOutput = (Test-Path $aiLogFile) ? [string](Get-Content $aiLogFile -Raw) : ''
    $moveCount = ([regex]::Matches($logOutput, 'GetMove complete:')).Count

    # Require at least 2 completed moves (one per side) — confirms the search engine
    # is functioning. With time_limit:15000ms per move, a 60s timeout window yields
    # ~3-4 moves; the game will not complete naturally in that window, so we do not
    # require game-termination (checkmate/stalemate/draw). Explicit parentheses guard
    # against PowerShell's left-to-right -and/-or precedence.
    if ($moveCount -ge 2) {
        Write-Host "PASS: $moveCount move(s) logged; engine is functional." -ForegroundColor Green
        $checkResults['Self-play'] = 'PASS'
    } else {
        Write-Host "FAIL: $moveCount move(s) logged. Expected at least 2." -ForegroundColor Red
        Write-Host "Log tail (aiperplex.log):" -ForegroundColor Yellow
        $logOutput -split "`n" | Select-Object -Last 10 | ForEach-Object { Write-Host "  $_" }
        $checkResults['Self-play'] = 'FAIL'
    }
} finally {
    Pop-Location
    if (Test-Path $outFile) { Remove-Item $outFile -ErrorAction SilentlyContinue }
}

# --- Summary table ---
Write-Host "`n--- Pre-PR Validation Summary ---" -ForegroundColor Cyan
Write-Host ("  {0,-22} {1}" -f 'Change tier', $change.Tier) -ForegroundColor DarkGray
$anyFailed = $false
foreach ($check in $checkResults.Keys) {
    $status = $checkResults[$check]
    $color  = ($status -eq 'PASS') ? 'Green' : 'Red'
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
