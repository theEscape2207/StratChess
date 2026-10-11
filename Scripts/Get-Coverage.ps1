<#
.SYNOPSIS
    Measure how much of the engine the fast test tier executes.

.DESCRIPTION
    Builds the test binary with the windows-clang-cl-coverage preset (LLVM source-based
    coverage), runs it, merges the raw profile and prints llvm-cov's per-file table and a
    one-line total. Only engine sources are reported: Catch2, the other dependencies,
    the tests themselves and system headers are filtered out.

    The instrumentation is in the frontend, so a Release build maps lines faithfully.
    Per-file figures for constexpr-heavy headers (defines.h, Magic.h) read near zero
    because their tables are built at compile time - that is not a gap.

    The build lives in build\windows-clang-cl-coverage, apart from every measured preset.

.PARAMETER Tag
    Catch2 filter for the run. Defaults to the fast tier, '~[slow]', which is what a
    pull request runs.

.PARAMETER Html
    Also write llvm-cov's annotated-source HTML report into this directory.

.PARAMETER SelfTest
    Run the summary-parsing tests and exit. Builds nothing.

.HOW TO INVOKE
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Get-Coverage.ps1
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Get-Coverage.ps1 -Html build\coverage-html
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Get-Coverage.ps1 "[eval]"
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Get-Coverage.ps1 -SelfTest
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Tag = '~[slow]',

    [string]$Html = '',

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path $PSScriptRoot -Parent

# Engine sources only. Paths reach llvm-cov with either separator, so both are matched.
$IgnoreFilenameRegex = '[\\/](_deps|StratChessTests)[\\/]|Program Files|Windows Kits'

# The totals of an `llvm-cov export -summary-only` document, one object per metric.
function Get-CoverageTotals {
    param([Parameter(Mandatory)][string]$ExportJson)

    $doc = $ExportJson | ConvertFrom-Json
    $docTotals = $doc.data[0].totals
    foreach ($metricName in 'lines', 'regions', 'branches', 'functions') {
        if (-not $docTotals.PSObject.Properties[$metricName]) {
            throw "llvm-cov export has no '$metricName' total"
        }
    }
    [ordered]@{
        Lines     = $docTotals.lines
        Regions   = $docTotals.regions
        Branches  = $docTotals.branches
        Functions = $docTotals.functions
    }
}

function Format-CoverageMetric {
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][object]$Metric)

    $inv = [cultureinfo]::InvariantCulture
    if ($Metric.count -eq 0) { return "$Name -" }
    $percent = ([double]$Metric.covered / [double]$Metric.count * 100).ToString('0.00', $inv)
    '{0} {1}% ({2}/{3})' -f $Name, $percent, $Metric.covered.ToString($inv), $Metric.count.ToString($inv)
}

function Format-CoverageTotals {
    param([Parameter(Mandatory)][System.Collections.IDictionary]$Totals)

    $parts = foreach ($key in $Totals.Keys) {
        Format-CoverageMetric -Name $key -Metric $Totals[$key]
    }
    $parts -join ' | '
}

if ($SelfTest) {
    function New-ExportJson {
        param([int[]]$Lines, [int[]]$Branches)
        @{ data = @(@{ totals = @{
            lines     = @{ covered = $Lines[0];    count = $Lines[1] }
            regions   = @{ covered = 3956;         count = 4509 }
            branches  = @{ covered = $Branches[0]; count = $Branches[1] }
            functions = @{ covered = 516;          count = 595 }
        } }) } | ConvertTo-Json -Depth 6
    }

    $cases = @(
        @{ Name   = 'probe baseline formats invariantly'
           Json   = New-ExportJson -Lines 5010, 6321 -Branches 2463, 2958
           Expect = 'Lines 79.26% (5010/6321) | Regions 87.74% (3956/4509) | Branches 83.27% (2463/2958) | Functions 86.72% (516/595)' }
        @{ Name   = 'no branches reads as a dash, not a division by zero'
           Json   = New-ExportJson -Lines 1, 2 -Branches 0, 0
           Expect = 'Lines 50.00% (1/2) | Regions 87.74% (3956/4509) | Branches - | Functions 86.72% (516/595)' }
        @{ Name   = 'a missing total throws rather than printing a partial line'
           Json   = '{"data":[{"totals":{"lines":{"covered":1,"count":2}}}]}'
           Expect = 'THROWS' }
    )

    # A comma-decimal culture is what this machine runs; the output must not follow it.
    $savedCulture = [cultureinfo]::CurrentCulture
    [cultureinfo]::CurrentCulture = [cultureinfo]::GetCultureInfo('da-DK')
    $failed = 0
    try {
        foreach ($case in $cases) {
            try {
                $actual = Format-CoverageTotals -Totals (Get-CoverageTotals -ExportJson $case.Json)
            } catch {
                $actual = 'THROWS'
            }
            if ($actual -ceq $case.Expect) {
                Write-Host "PASS  $($case.Name)" -ForegroundColor Green
            } else {
                Write-Host "FAIL  $($case.Name)`n      expected: $($case.Expect)`n      actual:   $actual" -ForegroundColor Red
                $failed++
            }
        }
    } finally {
        [cultureinfo]::CurrentCulture = $savedCulture
    }
    exit ($failed -eq 0 ? 0 : 1)
}

$buildScript = Join-Path $RepoRoot 'build.ps1'
$buildDir    = Join-Path $RepoRoot 'build\windows-clang-cl-coverage'
$testExe     = Join-Path $buildDir 'StratChessTests.exe'
$profileDir  = Join-Path $buildDir 'profile'

# In-process, so the VS environment build.ps1 imports stays on this process's PATH.
& $buildScript tests -Coverage
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# llvm-profdata and llvm-cov ship beside clang-cl in the VS LLVM component.
$llvmBin = Split-Path (Get-Command clang-cl).Source -Parent
$profdata = Join-Path $llvmBin 'llvm-profdata.exe'
$llvmCov  = Join-Path $llvmBin 'llvm-cov.exe'
foreach ($tool in $profdata, $llvmCov) {
    if (-not (Test-Path $tool)) { Write-Error "$tool not found beside clang-cl."; exit 1 }
}

# A leftover .profraw from an earlier run would be merged into this one.
if (Test-Path $profileDir) { Remove-Item -Recurse -Force $profileDir }
New-Item -ItemType Directory -Path $profileDir | Out-Null

Write-Host "`n==> Running instrumented tests ($Tag)" -ForegroundColor Cyan
$callerProfileFile = $env:LLVM_PROFILE_FILE
$env:LLVM_PROFILE_FILE = Join-Path $profileDir 'tests-%p.profraw'
try {
    & $testExe $Tag
    $testExit = $LASTEXITCODE
} finally {
    $env:LLVM_PROFILE_FILE = $callerProfileFile
}
if ($testExit -ne 0) {
    # Write-Host, not Write-Error: under 'Stop' the latter would exit 1, not the test's code.
    Write-Host "Tests failed (exit $testExit); no coverage report from a failing run." -ForegroundColor Red
    exit $testExit
}

$rawProfiles = @(Get-ChildItem -Path $profileDir -Filter '*.profraw')
if ($rawProfiles.Count -eq 0) {
    Write-Error "The test run wrote no .profraw - the binary is not instrumented."
    exit 1
}
$merged = Join-Path $profileDir 'tests.profdata'
& $profdata merge -sparse @($rawProfiles.FullName) -o $merged
if ($LASTEXITCODE -ne 0) { Write-Error "llvm-profdata merge failed (exit $LASTEXITCODE)."; exit 1 }

$covArgs = @($testExe, "-instr-profile=$merged", "-ignore-filename-regex=$IgnoreFilenameRegex")

Write-Host "`n==> Coverage by file" -ForegroundColor Cyan
& $llvmCov report @covArgs
if ($LASTEXITCODE -ne 0) { Write-Error "llvm-cov report failed (exit $LASTEXITCODE)."; exit 1 }

$exportText = (& $llvmCov export -format=text -summary-only @covArgs) -join "`n"
if ($LASTEXITCODE -ne 0) { Write-Error "llvm-cov export failed (exit $LASTEXITCODE)."; exit 1 }
$coverageTotals = Get-CoverageTotals -ExportJson $exportText

if ($Html) {
    & $llvmCov show -format=html "-output-dir=$Html" @covArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "llvm-cov show failed (exit $LASTEXITCODE)."; exit 1 }
    Write-Host "HTML report: $(Join-Path $Html 'index.html')"
}

Write-Host "`n==> Total ($Tag): $(Format-CoverageTotals -Totals $coverageTotals)" -ForegroundColor Cyan
exit 0
