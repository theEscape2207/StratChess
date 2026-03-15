<#
.SYNOPSIS
    Build wrapper for StratChessEvolved.

.DESCRIPTION
    Discovers MSBuild via vswhere and exposes simple build verbs.
    Defaults to Release|x64.

.PARAMETER Verb
    main            Build the main solution (StratChessEvolved.sln)
    tests           Build the test project (StratChessTests.vcxproj)
    all             Build main then tests (default)
    run-tests       Build tests then run fast tier only (excludes [slow])
    extended-tests  Build tests then run all tiers including [slow]

.PARAMETER Tag
    Optional Catch2 tag filter for run-tests, e.g. "[formatter]" or "[perft]".

.PARAMETER Config
    Build configuration: Release (default) or Debug.

.EXAMPLE
    .\build.ps1
    .\build.ps1 tests
    .\build.ps1 run-tests
    .\build.ps1 run-tests "[formatter]"
    .\build.ps1 extended-tests
    .\build.ps1 all -Config Debug
#>
param(
    [Parameter(Position=0)]
    [ValidateSet('main', 'tests', 'all', 'run-tests', 'extended-tests')]
    [string]$Verb = 'all',

    [Parameter(Position=1)]
    [string]$Tag = '',

    [string]$Config = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Platform = 'x64'
$RepoRoot  = $PSScriptRoot

# ---------------------------------------------------------------------------
# Discover MSBuild via vswhere (works regardless of VS version / install path)
# ---------------------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere.exe not found at: $vswhere`nIs Visual Studio installed?"
    exit 1
}

$MSBuild = (& $vswhere -latest -requires Microsoft.Component.MSBuild `
    -find 'MSBuild\**\Bin\MSBuild.exe') | Select-Object -First 1

if (-not $MSBuild -or -not (Test-Path $MSBuild)) {
    Write-Error "MSBuild.exe not found via vswhere. Is the MSBuild component installed?"
    exit 1
}

Write-Host "Using MSBuild: $MSBuild" -ForegroundColor DarkGray
Write-Host "Config: $Config | Platform: $Platform" -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------
function Invoke-MSBuild {
    param([string]$Target, [switch]$Parallel)
    $extraArgs = if ($Parallel) { @('/m') } else { @() }
    $allArgs = @(
        $Target,
        "/p:Configuration=$Config",
        "/p:Platform=$Platform",
        '/v:minimal'
    ) + $extraArgs
    Write-Host ""
    Write-Host "==> $Target" -ForegroundColor Cyan
    & $MSBuild @allArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed (exit $LASTEXITCODE): $Target"
        exit $LASTEXITCODE
    }
}

$Sln      = Join-Path $RepoRoot 'StratChessEvolved.sln'
$TestProj = Join-Path $RepoRoot 'StratChessTests\StratChessTests.vcxproj'
$TestExe  = Join-Path $RepoRoot "StratChessTests\$Platform\$Config\StratChessTests.exe"

# ---------------------------------------------------------------------------
# Verbs
# ---------------------------------------------------------------------------
switch ($Verb) {
    'main' {
        Invoke-MSBuild $Sln -Parallel
    }
    'tests' {
        Invoke-MSBuild $TestProj -Parallel
    }
    'all' {
        Invoke-MSBuild $Sln -Parallel
        Invoke-MSBuild $TestProj -Parallel
    }
    'run-tests' {
        Invoke-MSBuild $TestProj -Parallel
        Write-Host ""
        # Default: exclude [slow] tests; pass an explicit tag to override.
        $effectiveTag = if ($Tag) { $Tag } else { '~[slow]' }
        Write-Host "==> Running tests ($effectiveTag)" -ForegroundColor Cyan
        & $TestExe $effectiveTag
        exit $LASTEXITCODE
    }
    'extended-tests' {
        Invoke-MSBuild $TestProj -Parallel
        Write-Host ""
        Write-Host "==> Running all tests including [slow]" -ForegroundColor Cyan
        & $TestExe
        exit $LASTEXITCODE
    }
}
