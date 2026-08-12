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
    pwsh -ExecutionPolicy Bypass -File C:\...\StratChessEvolved\Scripts\Get-BuildArtifact.ps1
    ... -File C:\...\StratChessEvolved\Scripts\Get-BuildArtifact.ps1 -Target StratChessTests -Config Debug

.PARAMETER Target
    StratChessEvolved (default) or StratChessTests.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Compiler
    clang-cl (default, the shipping compiler) or msvc.

.PARAMETER AllowMissing
    Print the path even when nothing has been built there yet. Only for callers
    that resolve the path *before* the build that produces it -- Validate-PrePR
    and Run-EloMatch both do. Otherwise a missing binary is an error here, so it
    surfaces at resolution rather than as a confusing failure at first use, or
    worse, as a stale binary from another checkout.
#>
[CmdletBinding()]
param(
    [ValidateSet('StratChessEvolved', 'StratChessTests')]
    [string]$Target = 'StratChessEvolved',

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [ValidateSet('clang-cl', 'msvc')]
    [string]$Compiler = 'clang-cl',

    [switch]$AllowMissing
)

Set-StrictMode -Version Latest

$GameDir  = Split-Path $PSScriptRoot -Parent
$RepoRoot = Split-Path $GameDir -Parent

$preset = "windows-$Compiler"
if ($Config -eq 'Debug') { $preset += '-debug' }

$path = Join-Path $RepoRoot "build\$preset\$Target.exe"

if (-not $AllowMissing -and -not (Test-Path $path)) {
    Write-Error "Not built: $path`nBuild it with: .\build.ps1 main -Config $Config"
    exit 1
}

Write-Output $path
