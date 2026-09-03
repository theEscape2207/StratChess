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
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Validate-PreCommit.ps1

.PARAMETER SelfTest
    Run the FEN-check cases and exit. Pure: no build, no test suite, no filesystem.

.NOTES
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>
[CmdletBinding()]
param(
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
# Do NOT set $ErrorActionPreference = 'Stop' — this script deliberately accumulates
# failures across both checks before exiting. Each step checks $LASTEXITCODE directly.

$RepoRoot     = Split-Path $PSScriptRoot -Parent
$GameDir      = Join-Path $RepoRoot 'StratChessEvolved'
$buildScript  = Join-Path $RepoRoot 'build.ps1'
$settingsFile = Join-Path $GameDir 'game_settings.json'
$startingFen  = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
$failed       = $false

# game_settings.json is JSONC, and its "Alternative FEN positions" block is a commented
# copy of the active "FEN": line. Stripping comments first is what makes this a check of
# the ACTIVE field: matching the raw text finds the commented copy and passes whatever the
# active FEN says, which is the one situation the check exists to catch.
function Test-ActiveStartingFen {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Content,
        [Parameter(Mandatory)][string]$StartingFen
    )

    $active = [regex]::Replace($Content, '/\*.*?\*/', '', 'Singleline')
    $active = [regex]::Replace($active, '(?m)//.*$', '')
    return $active -match ('"FEN"\s*:\s*"' + [regex]::Escape($StartingFen) + '"')
}

if ($SelfTest) {
    $commented = @'
    /* Default FEN board setup */
    "FEN": "{ACTIVE}"

    /* Alternative FEN positions:
    "FEN": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
    */
'@
    $cases = @(
        @{ Name = 'active starting FEN passes'
           Content = '"FEN": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"'
           Expect = $true }
        @{ Name = 'whitespace around the colon is tolerated'
           Content = '"FEN"  :   "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"'
           Expect = $true }
        @{ Name = 'active starting FEN beside a commented copy passes'
           Content = $commented.Replace('{ACTIVE}', 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1')
           Expect = $true }
        # The falsification. Before comments were stripped this returned true, so an edited
        # active FEN committed cleanly: the commented copy alone satisfied the match.
        @{ Name = 'FALSIFY: edited active FEN is caught despite the commented copy'
           Content = $commented.Replace('{ACTIVE}', '8/8/8/4k3/8/8/4K3/8 w - - 0 1')
           Expect = $false }
        @{ Name = 'starting FEN only in a line comment does not count'
           Content = '// "FEN": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"'
           Expect = $false }
        @{ Name = 'no FEN key at all fails'
           Content = '{ "setup": "FEN" }'
           Expect = $false }
        @{ Name = 'a different key ending in FEN does not count'
           Content = '"startFEN": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"'
           Expect = $false }
    )

    $failedCases = 0
    foreach ($case in $cases) {
        $actual = Test-ActiveStartingFen -Content $case.Content -StartingFen $startingFen
        if ($actual -eq $case.Expect) {
            Write-Host "  PASS  $($case.Name)" -ForegroundColor Green
        }
        else {
            $failedCases++
            Write-Host "  FAIL  $($case.Name): got $actual, expected $($case.Expect)" -ForegroundColor Red
        }
    }

    Write-Host ''
    if ($failedCases -gt 0) {
        Write-Host "$failedCases self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host "All $($cases.Count) self-test cases passed." -ForegroundColor Green
    exit 0
}

# --- Step 1: FEN check ---
Write-Host "`n==> Checking FEN in game_settings.json" -ForegroundColor Cyan
$content = Get-Content $settingsFile -Raw
if (-not (Test-ActiveStartingFen -Content $content -StartingFen $startingFen)) {
    Write-Host "FAIL: game_settings.json active FEN is not the starting position." -ForegroundColor Red
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
