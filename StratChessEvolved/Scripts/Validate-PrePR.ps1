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
    .\StratChessEvolved\Scripts\Validate-PrePR.ps1

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
$aiLogFile   = Join-Path $logsDir 'aiperplex.log'
$checkResults = [ordered]@{}

# --- Step 1: Full parallel build ---
Write-Host "`n==> Full build (main + tests in parallel)" -ForegroundColor Cyan
# build.ps1 sets $ErrorActionPreference='Stop' internally and calls Write-Error on failure,
# which propagates a terminating error to this script via &. Wrap in try/catch so all three
# checks always run. Track failure via $buildFailed rather than $LASTEXITCODE — when the
# terminating error is caught, $LASTEXITCODE reflects the last native process (MSBuild), not
# build.ps1's exit code, so it can't be relied on for the PASS/FAIL decision.
$buildFailed = $false
try   { & $buildScript all }
catch { $buildFailed = $true; Write-Host "Build threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $buildFailed = $true }
$checkResults['Full build'] = if ($buildFailed) { 'FAIL' } else { 'PASS' }

# --- Step 2: Extended test suite ---
Write-Host "`n==> Extended test suite (including [slow])" -ForegroundColor Cyan
$extFailed = $false
try   { & $buildScript extended-tests }
catch { $extFailed = $true; Write-Host "Extended-tests threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $extFailed = $true }
$checkResults['Extended tests'] = if ($extFailed) { 'FAIL' } else { 'PASS' }

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
    # Truncate aiperplex.log before run so we only count moves from this game
    if (Test-Path $aiLogFile) { Clear-Content $aiLogFile }
    $proc   = Start-Process $gameExe -ArgumentList 'game' -PassThru -NoNewWindow -RedirectStandardOutput $outFile
    $exited = $proc.WaitForExit(60000)
    if (-not $exited) { $proc.Kill() }

    # GetMove complete: is written to logs/aiperplex.log by s_logger (file sink).
    # RedirectStandardOutput does not capture spdlog's stdout_color_sink on Windows
    # (spdlog uses WriteConsoleW which bypasses the C-runtime stdout redirect).
    $logOutput = if (Test-Path $aiLogFile) { [string](Get-Content $aiLogFile -Raw) } else { '' }
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
