<#
.SYNOPSIS
    Print the path to a built binary for a given target, configuration and compiler.

.DESCRIPTION
    CMake presets emit into build/<preset>/, so there is no longer one x64\Release
    path every script can assume. This is the single place that knows the mapping;
    scripts call it rather than hardcoding a layout that then drifts.

    The default compiler is clang-cl, which is what ships. That default is
    load-bearing for measurement: Run-Bench and Run-EloMatch numbers are only
    comparable between binaries built by the same compiler, so an MSVC candidate
    measured against a clang reference reads as a large regression that is really
    just the compiler difference (issue #84 measured it at roughly 25% nps).

.WHEN TO USE
    Any script or session that needs a built binary and should not care which
    preset directory it landed in.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Get-BuildArtifact.ps1
    ... -File C:\...\Scripts\Get-BuildArtifact.ps1 -Target StratChessTests -Config Debug

.PARAMETER Target
    StratChessEvolved (default) or StratChessTests.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Compiler
    clang-cl (default, the shipping compiler) or msvc.

.PARAMETER AllowMissing
    Print the path even when nothing has been built there yet, and implies
    -AllowStale. Only for callers that resolve the path *before* the build that
    produces it -- Validate-PrePR and Run-EloMatch both do, so a freshness error
    there would fire on a binary they are about to rebuild anyway. Otherwise a
    missing binary is an error here, so it surfaces at resolution rather than as
    a confusing failure at first use.

.PARAMETER AllowStale
    Skip the freshness check and print the path even if sources are newer than
    the binary. For a caller deliberately measuring an old build.

.PARAMETER SelfTest
    Run the preset-mapping cases and exit. Pure: no build, no filesystem.

.NOTES
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>
[CmdletBinding()]
param(
    [ValidateSet('StratChessEvolved', 'StratChessTests')]
    [string]$Target = 'StratChessEvolved',

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [ValidateSet('clang-cl', 'msvc')]
    [string]$Compiler = 'clang-cl',

    [switch]$AllowMissing,
    [switch]$AllowStale,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest

# The preset mapping this script exists to own, split out so it can be asserted without
# a build. Kept free of filesystem access for that reason.
function Get-ArtifactRelativePath {
    param(
        [Parameter(Mandatory)][string]$Target,
        [Parameter(Mandatory)][string]$Config,
        [Parameter(Mandatory)][string]$Compiler
    )

    $preset = "windows-$Compiler"
    if ($Config -eq 'Debug') { $preset += '-debug' }
    return "build\$preset\$Target.exe"
}

if ($SelfTest) {
    $cases = @(
        # The clang-cl default is load-bearing for measurement, not a preference: an MSVC
        # candidate measured against a clang reference reads as a ~25% nps regression that
        # is really the compiler (#84). Asserted from the parameter default, so a change to
        # it fails here rather than in a strength result nobody can explain.
        @{ Name = 'default is the shipping compiler'
           Args = @{}
           Expect = 'build\windows-clang-cl\StratChessEvolved.exe' }
        @{ Name = 'clang-cl Debug takes the -debug preset'
           Args = @{ Config = 'Debug' }
           Expect = 'build\windows-clang-cl-debug\StratChessEvolved.exe' }
        @{ Name = 'msvc Release'
           Args = @{ Compiler = 'msvc' }
           Expect = 'build\windows-msvc\StratChessEvolved.exe' }
        @{ Name = 'msvc Debug'
           Args = @{ Compiler = 'msvc'; Config = 'Debug' }
           Expect = 'build\windows-msvc-debug\StratChessEvolved.exe' }
        @{ Name = 'test binary keeps its own name'
           Args = @{ Target = 'StratChessTests' }
           Expect = 'build\windows-clang-cl\StratChessTests.exe' }
        @{ Name = 'test binary, Debug'
           Args = @{ Target = 'StratChessTests'; Config = 'Debug' }
           Expect = 'build\windows-clang-cl-debug\StratChessTests.exe' }
    )

    $failed = 0
    foreach ($case in $cases) {
        # Unspecified keys fall back to this script's own parameter defaults, so the
        # defaults themselves are under test and not restated here.
        $splat = @{
            Target   = $case.Args.ContainsKey('Target')   ? $case.Args.Target   : $Target
            Config   = $case.Args.ContainsKey('Config')   ? $case.Args.Config   : $Config
            Compiler = $case.Args.ContainsKey('Compiler') ? $case.Args.Compiler : $Compiler
        }
        $actual = Get-ArtifactRelativePath @splat
        if ($actual -eq $case.Expect) {
            Write-Host "  PASS  $($case.Name)" -ForegroundColor Green
        }
        else {
            $failed++
            Write-Host "  FAIL  $($case.Name): got '$actual', expected '$($case.Expect)'" -ForegroundColor Red
        }
    }

    Write-Host ''
    if ($failed -gt 0) {
        Write-Host "$failed self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host "All $($cases.Count) self-test cases passed." -ForegroundColor Green
    exit 0
}

$RepoRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildFreshness.ps1')

$path = Join-Path $RepoRoot (Get-ArtifactRelativePath -Target $Target -Config $Config -Compiler $Compiler)

if (-not $AllowMissing -and -not (Test-Path $path)) {
    Write-Error "Not built: $path`nBuild it with: .\build.ps1 main -Config $Config"
    exit 1
}

if (-not $AllowMissing -and -not $AllowStale -and (Test-Path $path)) {
    $artifact = ($Target -eq 'StratChessTests') ? 'Tests' : 'Main'
    $writeTime = (Get-Item $path).LastWriteTime
    $verdict = Test-ArtifactFreshness -ArtifactWriteTime $writeTime -Sources (Get-BuildRelevantSources -Root $RepoRoot -Artifact $artifact)
    if ($verdict.Reason -eq 'stale') {
        $newer = Split-Path $verdict.NewestSourcePath -Leaf
        Write-Error "Stale: $path`n$newer is newer. Rebuild it with: .\build.ps1 main -Config $Config, or pass -AllowStale to use it anyway."
        exit 1
    }
}

Write-Output $path
