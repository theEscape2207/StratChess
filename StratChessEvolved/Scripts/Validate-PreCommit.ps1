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
