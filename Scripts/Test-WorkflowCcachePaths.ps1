<#
.SYNOPSIS
    Fail when a GitHub Actions workflow configures ccache's path-rewriting options.

.DESCRIPTION
    CI builds every job at a stable path and restores an entry produced at that same
    path, so absolute paths in cached depfiles and in embedded CodeView records match
    the consuming tree by construction. Two ccache options break that construction:

      base_dir  -- rewrites the command handed to the compiler, so the compiler emits
                   relative paths into the `.d` file. On the clang-cl presets CMake
                   drives dependencies through `deps = gcc` + `depfile`, and Ninja
                   compares depfile paths textually: an unmatched path is not an error,
                   it is a dependency edge that is silently never hooked up. The failure
                   mode is a stale artifact, not a red build (#510).
      hash_dir  -- when false, the build directory stops contributing to the hash, so a
                   hit can deliver another tree's absolute paths inside `/Z7` CodeView
                   records (#511).

    Both are local-workflow questions. Neither has any measured benefit in CI, where
    every job already builds at a stable path, and both would be adopted for a
    cross-job hit rate that CI's cache scoping does not offer. This guard exists
    because a prose note would not fail: the whole hazard class is one that leaves the
    build green.

    Matching is textual and deliberately narrow -- the ccache spellings only. The
    unrelated `FETCHCONTENT_BASE_DIR` in build-and-test.yml must not trip it, which is
    why the bare-key patterns reject a leading underscore.

    Scope is `.github/` rather than `.github/workflows/` alone, because the composite
    action that installs and configures ccache lives in `.github/actions/` and is
    where such a setting would most naturally be written.

.PARAMETER Root
    Directory to scan. Defaults to the repository's .github, resolved from this
    script's own location.

.PARAMETER SelfTest
    Run synthetic detector tests and exit. Verifies that the detector actually
    detects, which a green run over a compliant tree does not.

.HOW TO INVOKE
    pwsh -File Scripts/Test-WorkflowCcachePaths.ps1
    pwsh -File Scripts/Test-WorkflowCcachePaths.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run')]
    [string]$Root,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The environment spellings are exact and unambiguous. The bare configuration keys
# need a boundary that excludes '_' on both sides, so FETCHCONTENT_BASE_DIR and any
# other *_base_dir identifier is not a match. Case-insensitive throughout: YAML env
# names are conventionally upper case but nothing enforces it.
$script:BannedPattern = @(
    'CCACHE_BASEDIR'
    'CCACHE_HASHDIR'
    'CCACHE_NOHASHDIR'
    '(?<![A-Za-z0-9_])base_dir(?![A-Za-z0-9_])'
    '(?<![A-Za-z0-9_])hash_dir(?![A-Za-z0-9_])'
)

function Get-CcachePathSetting {
    <#
      .SYNOPSIS
        One record per offending line: the 1-based line number and the matched text.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Line
    )

    $hits = [System.Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $Line.Count; $i++) {
        foreach ($pattern in $script:BannedPattern) {
            $match = [regex]::Match($Line[$i], $pattern, 'IgnoreCase')
            if ($match.Success) {
                $hits.Add([pscustomobject]@{ Number = $i + 1; Text = $match.Value })
            }
        }
    }

    return $hits
}

function Invoke-SelfTest {
    function Assert-Case {
        param(
            [Parameter(Mandatory)][string]$Name,
            [Parameter(Mandatory)][AllowEmptyString()][string]$Content,
            [string[]]$ExpectedHit
        )

        $lines = $Content -split "`r?`n"
        $actual = @(Get-CcachePathSetting -Line $lines | ForEach-Object { $_.Text })
        $expected = @($ExpectedHit)

        if (($actual -join '|') -eq ($expected -join '|')) {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        }
        else {
            Write-Host "  FAIL  $Name (expected [$($expected -join ', ')], got [$($actual -join ', ')])" -ForegroundColor Red
            $script:selfTestFailures++
        }
    }

    $script:selfTestFailures = 0
    Write-Host "==> Self-test" -ForegroundColor Cyan

    Assert-Case -Name 'clean workflow passes' -Content @'
jobs:
  build:
    env:
      CCACHE_MAXSIZE: 400M
    steps:
      - run: build.ps1
'@ -ExpectedHit @()

    Assert-Case -Name 'CCACHE_BASEDIR env is caught' -Content @'
    env:
      CCACHE_BASEDIR: ${{ github.workspace }}
'@ -ExpectedHit @('CCACHE_BASEDIR')

    Assert-Case -Name 'CCACHE_NOHASHDIR env is caught' -Content @'
    env:
      CCACHE_NOHASHDIR: "true"
'@ -ExpectedHit @('CCACHE_NOHASHDIR')

    Assert-Case -Name 'ccache --set-config base_dir is caught' -Content @'
      - run: ccache --set-config base_dir=D:/a/StratChess
'@ -ExpectedHit @('base_dir')

    Assert-Case -Name 'ccache.conf hash_dir key is caught' -Content @'
      - run: echo "hash_dir = false" >> ccache.conf
'@ -ExpectedHit @('hash_dir')

    Assert-Case -Name 'lower-case env spelling is caught' -Content @'
      - run: $env:ccache_basedir = $PWD
'@ -ExpectedHit @('ccache_basedir')

    # The false positive this guard must not have: an unrelated CMake variable that
    # ends in _BASE_DIR. It is live in build-and-test.yml today.
    Assert-Case -Name 'FETCHCONTENT_BASE_DIR is not a match' -Content @'
      # Every Windows preset points FETCHCONTENT_BASE_DIR at build/_deps
      - run: cmake -D FETCHCONTENT_BASE_DIR=$deps ..
'@ -ExpectedHit @()

    Assert-Case -Name 'a similarly-named identifier is not a match' -Content @'
      - run: python tool.py --output-base-dir out --my_hash_dirs x
'@ -ExpectedHit @()

    Assert-Case -Name 'an empty file passes' -Content '' -ExpectedHit @()

    $failures = $script:selfTestFailures
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        return $false
    }
    Write-Host "Self-test PASSED." -ForegroundColor Green
    return $true
}

if ($SelfTest) {
    if (Invoke-SelfTest) { exit 0 }
    exit 1
}

if (-not $Root) {
    # Scripts/ -> repository root. Resolving from the script's own location keeps
    # this correct in every worktree.
    $repoRoot = Split-Path $PSScriptRoot -Parent
    $Root = Join-Path $repoRoot '.github'
}

if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
    Write-Host "FAIL: no directory at $Root" -ForegroundColor Red
    exit 1
}

# -Include is not used: with -LiteralPath it silently matches nothing unless the path
# itself carries a wildcard, which is exactly the "guard that quietly passes" shape
# this script exists to rule out.
$files = @(Get-ChildItem -LiteralPath $Root -Recurse -File |
    Where-Object { $_.Extension -in '.yml', '.yaml' } |
    Sort-Object FullName)
if ($files.Count -eq 0) {
    Write-Host "FAIL: no workflow or action files under $Root" -ForegroundColor Red
    exit 1
}

Write-Host "==> ccache path settings ($($files.Count) file(s))" -ForegroundColor Cyan

$violations = 0
foreach ($file in $files) {
    $lines = @(Get-Content -LiteralPath $file.FullName)
    $hits = @(Get-CcachePathSetting -Line $lines)
    $label = [System.IO.Path]::GetRelativePath($Root, $file.FullName)

    if ($hits.Count -eq 0) {
        Write-Host "  PASS  $label" -ForegroundColor Green
        continue
    }

    foreach ($hit in $hits) {
        Write-Host "  FAIL  ${label}:$($hit.Number): '$($hit.Text)'" -ForegroundColor Red
        $violations++
    }
}

if ($violations -gt 0) {
    Write-Host ""
    Write-Host "$violations ccache path-rewriting setting(s) in CI configuration." -ForegroundColor Red
    Write-Host "base_dir makes Ninja silently drop dependency edges; hash_dir=false lets a hit" -ForegroundColor Yellow
    Write-Host "carry another tree's paths into CodeView records. Neither buys anything in CI," -ForegroundColor Yellow
    Write-Host "where every job builds at a stable path. See issues #510, #511 and #514." -ForegroundColor Yellow
    exit 1
}

Write-Host "PASS: no ccache path-rewriting settings in CI configuration." -ForegroundColor Green
exit 0
