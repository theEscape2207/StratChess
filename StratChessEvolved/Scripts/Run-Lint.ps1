<#
.SYNOPSIS
    Run clang-format and clang-tidy over the files a change touches.

.DESCRIPTION
    The local counterpart to the lint-linux CI job. clang-format is a gate: CI fails
    a pull request whose sources are not formatted, so the same check has to be
    reachable before pushing. clang-tidy is advisory here exactly as it is in CI --
    it prints findings and never fails this script -- until issue #284 clears the
    backlog and flips both.

    Neither tool is on PATH on a normal Windows box. They ship inside Visual Studio
    next to clang-cl, so this script resolves them the same way build.ps1 resolves
    the compiler: through vswhere, never a hard-coded VS path.

.WHEN TO USE
    Before pushing anything that touches C++ sources, and whenever CI's lint job
    fails and you want the same answer locally. Validate-PrePR.ps1 calls the format
    half automatically.

.HOW TO INVOKE
    pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Run-Lint.ps1
    ... -Check Format            # formatting only, what CI blocks on
    ... -Check Tidy              # static analysis only
    ... -All                     # whole tree instead of the changed files
    ... -Fix                     # rewrite files in place to satisfy clang-format

.PARAMETER Check
    Format, Tidy, or Both (default).

.PARAMETER All
    Lint every non-archived source instead of only what changed against -BaseRef.

.PARAMETER BaseRef
    Ref to diff against for the changed-file list. Default 'origin/main'.

.PARAMETER Fix
    Apply clang-format in place. Never applies clang-tidy fixes -- those change
    semantics often enough that they want reviewing individually.

.PARAMETER ClangFormat / ClangTidy
    Explicit paths, overriding discovery. Use when testing a specific toolchain.

.NOTES
    Must be invoked with -File, not dot-sourced. $PSScriptRoot is $null under dot-source.
#>
[CmdletBinding()]
param(
    [ValidateSet('Format', 'Tidy', 'Both')]
    [string]$Check = 'Both',

    [switch]$All,
    [string]$BaseRef = 'origin/main',
    [switch]$Fix,

    [string]$ClangFormat,
    [string]$ClangTidy,

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [ValidateSet('clang-cl', 'msvc')]
    [string]$Compiler = 'clang-cl'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The LLVM major the CI lint job pins. Local output must match CI's or the gate
# becomes "clean on my machine": clang-format's output can change between majors,
# and clang-tidy's check inventory certainly does.
$PinnedMajor = 22

$GameDir  = Split-Path $PSScriptRoot -Parent
$RepoRoot = Split-Path $GameDir -Parent

# ---------------------------------------------------------------------------
# Tool resolution: explicit parameter, then PATH, then Visual Studio.
# ---------------------------------------------------------------------------
function Resolve-LlvmTool {
    param(
        [Parameter(Mandatory)][string]$Name,     # 'clang-format' / 'clang-tidy'
        [string]$Explicit
    )

    if ($Explicit) {
        if (-not (Test-Path $Explicit)) { throw "$Name not found at the path given: $Explicit" }
        return $Explicit
    }

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    # Visual Studio ships LLVM alongside clang-cl. Locate the installation the same
    # way build.ps1 does rather than assuming an edition or a year.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -prerelease -products * -property installationPath 2>$null | Select-Object -First 1
        if ($vsRoot) {
            $candidate = Join-Path $vsRoot "VC\Tools\Llvm\x64\bin\$Name.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    throw @"
$Name could not be located. Looked in:
  1. the -$($Name -replace '-','') parameter (not supplied)
  2. PATH
  3. <VS install>\VC\Tools\Llvm\x64\bin\$Name.exe, via vswhere

Install the 'C++ Clang tools for Windows' component in the Visual Studio Installer,
or pass an explicit path.
"@
}

function Get-ToolMajor {
    param([Parameter(Mandatory)][string]$Exe)
    $line = & $Exe --version 2>&1 | Select-Object -First 1
    if ($line -match '(?:version\s+)(\d+)\.') { return [int]$Matches[1] }
    return 0
}

function Assert-PinnedMajor {
    param([string]$Exe, [string]$Name)
    $major = Get-ToolMajor -Exe $Exe
    $label = "  {0,-13} {1} (LLVM {2})" -f "${Name}:", $Exe, $major
    if ($major -eq $PinnedMajor) {
        Write-Host $label -ForegroundColor DarkGray
    } else {
        Write-Host $label -ForegroundColor Yellow
        Write-Warning "$Name is LLVM $major but CI pins LLVM $PinnedMajor. Results may not match CI."
    }
}

# ---------------------------------------------------------------------------
# File selection
# ---------------------------------------------------------------------------
function Get-TargetFiles {
    $all = @(git -C $RepoRoot ls-files '*.cpp' '*.h' 2>$null |
             Where-Object { $_ -and $_ -notmatch '(^|/)Archived/' })

    if ($All) { return $all }

    $committed = @(git -C $RepoRoot diff --name-only "$BaseRef...HEAD" 2>$null | Where-Object { $_ })
    if ($LASTEXITCODE -ne 0) {
        # Fail closed. Linting nothing must never look like linting cleanly -- that is
        # the same green-because-it-checked-nothing failure the CI job guards against.
        throw "Cannot diff against '$BaseRef'. Fetch it, or pass -All to lint the whole tree."
    }

    # Uncommitted work counts: this script is reached precisely when someone has not
    # committed yet. Mirrors Get-ChangeTier.ps1's porcelain handling, renames included.
    $working = @(git -C $RepoRoot status --porcelain 2>$null | Where-Object { $_ } | ForEach-Object {
        $p = $_.Substring(3).Trim().Trim('"')
        if ($p -match '\s->\s') { $p = ($p -split '\s->\s')[-1].Trim().Trim('"') }
        $p
    })

    $changed = @($committed + $working | Where-Object { $_ } | Select-Object -Unique)
    return @($all | Where-Object { $changed -contains $_ })
}

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
function Invoke-FormatCheck {
    param([string[]]$Files, [string]$Exe)

    if ($Fix) {
        Write-Host "`n==> clang-format (applying to $($Files.Count) file(s))" -ForegroundColor Cyan

        # Iterate to a fixpoint. clang-format is NOT idempotent in one pass: reflowing
        # a long comment can emit continuation lines that the next pass then corrects
        # ('//text' gaining its space). A single pass therefore leaves files that still
        # fail --dry-run -Werror, i.e. still fail CI. Observed on Eval.cpp during the
        # initial reformat, which converged on the second pass.
        $maxPasses = 5
        for ($pass = 1; $pass -le $maxPasses; $pass++) {
            $dirty = @($Files | Where-Object {
                & $Exe --style=file --dry-run -Werror (Join-Path $RepoRoot $_) 2>&1 | Out-Null
                $LASTEXITCODE -ne 0
            })
            if ($dirty.Count -eq 0) {
                Write-Host "Formatting applied; converged after $($pass - 1) pass(es)." -ForegroundColor Green
                return $true
            }
            Write-Host "  pass ${pass}: reformatting $($dirty.Count) file(s)" -ForegroundColor DarkGray
            foreach ($f in $dirty) { & $Exe -i --style=file (Join-Path $RepoRoot $f) }
        }

        Write-Host "FAIL: clang-format did not reach a fixpoint in $maxPasses passes." -ForegroundColor Red
        Write-Host 'That is a clang-format bug or a pathological construct, not your change.' -ForegroundColor Red
        return $false
    }

    Write-Host "`n==> clang-format (checking $($Files.Count) file(s))" -ForegroundColor Cyan
    $bad = @()
    foreach ($f in $Files) {
        & $Exe --style=file --dry-run -Werror (Join-Path $RepoRoot $f) 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { $bad += $f }
    }

    if ($bad.Count -gt 0) {
        Write-Host "FAIL: $($bad.Count) file(s) are not formatted:" -ForegroundColor Red
        $bad | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        Write-Host "`nFix them with: pwsh -File StratChessEvolved\Scripts\Run-Lint.ps1 -Check Format -Fix" -ForegroundColor Yellow
        return $false
    }
    Write-Host 'PASS: all checked files are correctly formatted.' -ForegroundColor Green
    return $true
}

function Invoke-TidyCheck {
    param([string[]]$Files, [string]$Exe)

    # Only translation units have compile-database entries. Headers are reached
    # through HeaderFilterRegex from the TUs that include them, and comprehensively
    # by the nightly whole-tree pass.
    $tus = @($Files | Where-Object { $_ -like '*.cpp' })

    $preset  = "windows-$Compiler"
    if ($Config -eq 'Debug') { $preset += '-debug' }
    $buildDir = Join-Path $RepoRoot "build\$preset"

    if (-not (Test-Path (Join-Path $buildDir 'compile_commands.json'))) {
        Write-Host "SKIP: no compile_commands.json in build\$preset." -ForegroundColor Yellow
        Write-Host "      Build once first: .\build.ps1 main -Config $Config" -ForegroundColor Yellow
        return
    }

    if ($tus.Count -eq 0) {
        Write-Host "`n==> clang-tidy: no .cpp files in scope, nothing to analyse." -ForegroundColor DarkGray
        return
    }

    Write-Host "`n==> clang-tidy (advisory -- analysing $($tus.Count) translation unit(s))" -ForegroundColor Cyan
    foreach ($t in $tus) {
        & $Exe -p $buildDir --quiet (Join-Path $RepoRoot $t)
    }
    Write-Host "`nclang-tidy findings above are ADVISORY and do not fail this script (see #284)." -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
Write-Host '==> Toolchain' -ForegroundColor Cyan
$fmtExe = $null; $tidyExe = $null
if ($Check -in @('Format', 'Both')) {
    $fmtExe = Resolve-LlvmTool -Name 'clang-format' -Explicit $ClangFormat
    Assert-PinnedMajor -Exe $fmtExe -Name 'clang-format'
}
if ($Check -in @('Tidy', 'Both')) {
    $tidyExe = Resolve-LlvmTool -Name 'clang-tidy' -Explicit $ClangTidy
    Assert-PinnedMajor -Exe $tidyExe -Name 'clang-tidy'
}

$files = Get-TargetFiles
if ($files.Count -eq 0) {
    Write-Host "`nNo C++ sources in scope; nothing to lint." -ForegroundColor Green
    exit 0
}

$formatOk = $true
if ($Check -in @('Format', 'Both')) { $formatOk = Invoke-FormatCheck -Files $files -Exe $fmtExe }
if ($Check -in @('Tidy',   'Both')) { Invoke-TidyCheck  -Files $files -Exe $tidyExe }

Write-Host ''
if (-not $formatOk) { Write-Host 'Lint FAILED (formatting).' -ForegroundColor Red; exit 1 }
Write-Host 'Lint PASSED.' -ForegroundColor Green
exit 0
