<#
.SYNOPSIS
    Build the test project and run the test suite.

.DESCRIPTION
    Thin wrapper around build.ps1 run-tests. Resolves paths relative to $PSScriptRoot
    so it can be called from any working directory.

.WHEN TO USE
    Any time you want to build and run tests — with or without a tag filter.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    .\StratChessEvolved\Scripts\Run-Tests.ps1
    .\StratChessEvolved\Scripts\Run-Tests.ps1 [tactical]

.PARAMETER Tag
    Optional Catch2 tag filter, e.g. "[tactical]" or "[eval]". Omit for full fast suite (~[slow]).
#>
param([string]$Tag = '')

Set-StrictMode -Version Latest

$GameDir     = Split-Path $PSScriptRoot -Parent
$RepoRoot    = Split-Path $GameDir -Parent
$buildScript = Join-Path $RepoRoot 'build.ps1'

if ($Tag) {
    & $buildScript run-tests $Tag
} else {
    & $buildScript run-tests
}
exit $LASTEXITCODE
