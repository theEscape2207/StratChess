<#
.SYNOPSIS
    Pre-commit validation: clang-format + FEN check + fast test suite.

.DESCRIPTION
    1. Checks clang-format on the changed files and exits immediately on failure,
       naming `Run-Lint.ps1 -Check Format -Fix`. Short-circuits (issue #478) because
       the fix is already known and cannot be changed by the FEN check or the test
       suite -- running them first only defers a result that is already decided.
    2. Verifies StratChessEvolved/game_settings.json contains the chess starting position FEN.
    3. Builds and runs the fast test suite (excludes [slow]).
    Steps 2 and 3 always both run before exit so their failures are reported together;
    step 1 alone short-circuits.
    Exits with code 1 if any check fails.

.WHEN TO USE
    Before every git commit. Always run this before Validate-PrePR.ps1.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Validate-PreCommit.ps1

.PARAMETER SelfTest
    Run the FEN-check and fail-fast-classification cases and exit. Pure: no build,
    no test suite, no filesystem.

.NOTES
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>
[CmdletBinding()]
param(
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
# Do NOT set $ErrorActionPreference = 'Stop' — the FEN check and the test suite
# deliberately accumulate before exiting so both failures are reported at once.
# clang-format is the exception: it short-circuits below. Each step checks
# $LASTEXITCODE directly.

$RepoRoot     = Split-Path $PSScriptRoot -Parent
$GameDir      = Join-Path $RepoRoot 'StratChessEvolved'
$buildScript  = Join-Path $RepoRoot 'build.ps1'
$lintScript   = Join-Path $PSScriptRoot 'Run-Lint.ps1'
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

# Pure: which pre-commit checks short-circuit rather than accumulate. Reserved for a
# check that is cheap, deterministic, auto-fixable, and whose remedy cannot be changed
# by any later result -- clang-format is the only one that currently qualifies (issue
# #478). The FEN check and the test suite need judgement (what changed, which test)
# that later results could still add to, so they keep aggregating into $failed.
function Test-IsFastFailCheck {
    param([Parameter(Mandatory)][string]$CheckName)
    return $CheckName -eq 'clang-format'
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

    # FALSIFY: before this rule existed, every check aggregated -- a formatting-only
    # failure still paid for the full build and test suite before reporting it.
    $fastFailCases = @(
        @{ Name = 'clang-format is fail-fast'; CheckName = 'clang-format'; Expect = $true }
        @{ Name = 'FALSIFY: the FEN check is not fail-fast'; CheckName = 'FEN check'; Expect = $false }
        @{ Name = 'FALSIFY: the test suite is not fail-fast'; CheckName = 'fast test suite'; Expect = $false }
    )
    foreach ($case in $fastFailCases) {
        $actual = Test-IsFastFailCheck -CheckName $case.CheckName
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
    Write-Host "All $($cases.Count + $fastFailCases.Count) self-test cases passed." -ForegroundColor Green
    exit 0
}

# --- Step 1: clang-format (issue #478) ---
# Short-circuits: the fix (Run-Lint.ps1 -Check Format -Fix) is already known and
# nothing the FEN check or the test suite could find would change it, so there is
# no reason to pay for either before reporting it.
Write-Host "`n==> clang-format" -ForegroundColor Cyan
$lintFailed = $false
try   { & $lintScript -Check Format }
catch { $lintFailed = $true; Write-Host "Lint threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $lintFailed = $true }
if ($lintFailed -and (Test-IsFastFailCheck -CheckName 'clang-format')) {
    Write-Host ''
    Write-Host 'Pre-commit validation FAILED (clang-format).' -ForegroundColor Red
    Write-Host '      Fix with: pwsh -File Scripts\Run-Lint.ps1 -Check Format -Fix' -ForegroundColor Yellow
    exit 1
}

# --- Step 2: FEN check ---
Write-Host "`n==> Checking FEN in game_settings.json" -ForegroundColor Cyan
$content = Get-Content $settingsFile -Raw
if (-not (Test-ActiveStartingFen -Content $content -StartingFen $startingFen)) {
    Write-Host "FAIL: game_settings.json active FEN is not the starting position." -ForegroundColor Red
    Write-Host "      Reset the FEN to: $startingFen" -ForegroundColor Yellow
    $failed = $true
} else {
    Write-Host "PASS: FEN is at starting position." -ForegroundColor Green
}

# --- Step 3: Fast test suite ---
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
