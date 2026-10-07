<#
.SYNOPSIS
    Compare-Bench on the strength lab's toolchain: builds two refs with GCC Release in WSL
    Ubuntu-26.04 and runs a paired nps comparison of them there, with an A/A control.

.DESCRIPTION
    The lab plays GCC builds on Linux, while Compare-Bench.ps1 measures the shipping clang-cl build
    on Windows. A speed change can cost a very different share of the time on the two, so read
    this next to a lab result.

    Each ref is built as strength.yml builds it (Release, no extra flags) on WSL's ext4, see
    WslBuild.ps1. Compare-Bench.ps1 then runs inside WSL under pwsh with -Control and
    -TrendOnly, with every binary and output on ext4, so the control copy sits where the arms do.
    The results are copied to -OutDir and the WSL tree is deleted.

    The result is a trend, not a verdict: there is no GCC equivalent of New-OrderedBuildPair.ps1.
    The interval, the A/A control and per-arm nps are all reported.

    Needs pwsh, cmake, ninja, g++ and tar in WSL Ubuntu-26.04. Run on a quiet machine.

.PARAMETER Baseline
    A worktree path, built with its uncommitted changes, or a commit of this repository.

.PARAMETER Candidate
    Likewise. It may equal -Baseline, for an A/A check.

.PARAMETER Rounds
    Measured rounds, passed to Compare-Bench. Default 12.

.PARAMETER Depth
    Fixed search depth, passed to Compare-Bench. Default 13.

.PARAMETER Positions
    Optional FEN file (Windows path), passed to Compare-Bench.

.PARAMETER Affinity
    Optional processor affinity mask. Applied with taskset to pwsh inside WSL, so every engine
    inherits it; .NET's ProcessorAffinity does not reach a child there. Default 0, unpinned.

.PARAMETER MinTimeMs
    Shortest search per position that may be timed, passed to Compare-Bench. Default 200.

.PARAMETER OutDir
    Where the build logs and Compare-Bench's CSVs, metadata.json and report.txt land. Defaults to
    build\compare-bench-linux\<timestamp>. Must be empty.

.PARAMETER SelfTest
    Assert the build script, the flags check and the Compare-Bench command line, and exit. Runs
    no build and no engine. Exits 1 on any failure.

.EXAMPLE
    .\Compare-BenchLinux.ps1 -Baseline origin/main -Candidate .
#>
[CmdletBinding()]
param(
    [string]$Baseline = '',

    [string]$Candidate = '',

    [ValidateRange(1, 1000)]
    [int]$Rounds = 12,

    [ValidateRange(1, 30)]
    [int]$Depth = 13,

    [string]$Positions = '',

    [int64]$Affinity = 0,

    [int]$MinTimeMs = 200,

    [string]$OutDir = '',

    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[System.Threading.Thread]::CurrentThread.CurrentCulture = [cultureinfo]::InvariantCulture

. (Join-Path $PSScriptRoot 'QuietWindow.ps1')
. (Join-Path $PSScriptRoot 'WslBuild.ps1')

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# Phase estimates; the first build's measured time replaces the build estimate.
$RoughBuildSeconds = 90
$RoughSuiteSeconds = 20

function Get-CompareBenchArgument {
    <# The command that runs Compare-Bench inside WSL, as an argument list for wsl --exec. #>
    param(
        [Parameter(Mandatory)][string]$Script,
        [Parameter(Mandatory)][string]$BaselineExe,
        [Parameter(Mandatory)][string]$CandidateExe,
        [Parameter(Mandatory)][string]$Out,
        [Parameter(Mandatory)][int]$RoundCount,
        [Parameter(Mandatory)][int]$SearchDepth,
        [Parameter(Mandatory)][int]$FloorMs,
        [Parameter(Mandatory)][AllowEmptyString()][string]$PositionFile,
        [Parameter(Mandatory)][int64]$Mask,
        [Parameter(Mandatory)][AllowEmptyString()][string]$BaselineLabel,
        [Parameter(Mandatory)][AllowEmptyString()][string]$CandidateLabel
    )

    $argList = [System.Collections.Generic.List[string]]::new()
    if ($Mask -ne 0) { $argList.AddRange([string[]]@('taskset', ('0x{0:x}' -f $Mask))) }
    $argList.AddRange([string[]]@(
        'pwsh', '-NoProfile', '-File', $Script,
        '-Baseline', $BaselineExe, '-Candidate', $CandidateExe, '-Control', '-TrendOnly',
        '-Rounds', [string]$RoundCount, '-Depth', [string]$SearchDepth, '-MinTimeMs', [string]$FloorMs,
        '-BaselineCommit', $BaselineLabel, '-CandidateCommit', $CandidateLabel, '-OutDir', $Out))
    if ($PositionFile) { $argList.AddRange([string[]]@('-Positions', $PositionFile)) }
    return $argList.ToArray()
}

function Set-AffinityMetadata {
    <# Compare-Bench records its own -Affinity, which is 0 here; record the taskset mask instead. #>
    param(
        [Parameter(Mandatory)][string]$Json,
        [Parameter(Mandatory)][int64]$Mask
    )
    $metadata = $Json | ConvertFrom-Json
    $metadata.Affinity = $Mask
    $mechanism = if ($Mask -ne 0) { 'taskset' } else { 'none' }
    $metadata | Add-Member -NotePropertyName AffinityMechanism -NotePropertyValue $mechanism -Force
    return $metadata | ConvertTo-Json -Depth 4
}

function Get-RefLabel {
    param([Parameter(Mandatory)][object]$Ref)
    $suffix = if ($Ref.Dirty) { '+uncommitted' } else { '' }
    return "$($Ref.Commit.Substring(0, 9))$suffix"
}

if ($SelfTest) {
    $failures = 0
    function Assert-Case {
        param([string]$Name, [bool]$Condition, [string]$Detail = '')
        if ($Condition) { Write-Host "  PASS  $Name" -ForegroundColor Green }
        else { Write-Host "  FAIL  $Name $Detail" -ForegroundColor Red; $script:failures++ }
    }

    Assert-Case 'FALSIFY: the WSL build script carries no CR' (-not $WslBuildScript.Contains("`r"))
    Assert-Case 'the WSL build script passes CMAKE_CXX_FLAGS only when flags are given' (
        $WslBuildScript.Contains('${flags:+"-DCMAKE_CXX_FLAGS=$flags"}') -and -not ($WslBuildScript -match 'DCMAKE_CXX_FLAGS=-g'))

    $flagCases = @(
        @{ Name = 'no flags: an empty CMAKE_CXX_FLAGS';        Log = @('CMAKE_BUILD_TYPE:STRING=Release', 'CMAKE_CXX_FLAGS:STRING='); Flags = '';   Expect = $true }
        @{ Name = 'FALSIFY: no flags rejects a -g build';     Log = @('CMAKE_CXX_FLAGS:STRING=-g');                                   Flags = '';   Expect = $false }
        @{ Name = '-g: matches';                               Log = @('CMAKE_CXX_FLAGS:STRING=-g');                                   Flags = '-g'; Expect = $true }
        @{ Name = 'FALSIFY: -g rejects an unflagged build';   Log = @('CMAKE_CXX_FLAGS:STRING=');                                     Flags = '-g'; Expect = $false }
        @{ Name = 'FALSIFY: _RELEASE is not CMAKE_CXX_FLAGS'; Log = @('CMAKE_CXX_FLAGS_RELEASE:STRING=');                             Flags = '';   Expect = $false }
        @{ Name = 'FALSIFY: an empty log';                    Log = @();                                                              Flags = '';   Expect = $false }
    )
    foreach ($case in $flagCases) {
        $got = Test-WslBuildFlags -Log $case.Log -Flags $case.Flags
        Assert-Case "flags check: $($case.Name)" ($got -eq $case.Expect) "got $got"
    }

    $common = @{ Script = '/s/Compare-Bench.ps1'; BaselineExe = '/b/x'; CandidateExe = '/c/x'; Out = '/o'; RoundCount = 12; SearchDepth = 13
                 FloorMs = 200; BaselineLabel = 'aaa'; CandidateLabel = 'bbb' }
    $plain = @(Get-CompareBenchArgument @common -PositionFile '' -Mask 0)
    Assert-Case 'command: pwsh first, A/A control and trend-only always on' ($plain[0] -eq 'pwsh' -and $plain -contains '-Control' -and $plain -contains '-TrendOnly') ($plain -join ' ')
    Assert-Case 'command: no -Positions without a file' ($plain -notcontains '-Positions') ($plain -join ' ')
    Assert-Case 'command: FALSIFY: -Affinity never reaches Compare-Bench' ($plain -notcontains '-Affinity') ($plain -join ' ')
    $pinned = @(Get-CompareBenchArgument @common -PositionFile '/p.fen' -Mask 12)
    Assert-Case 'command: an affinity mask runs pwsh under taskset' ($pinned[0] -eq 'taskset' -and $pinned[1] -eq '0xc' -and $pinned[2] -eq 'pwsh') ($pinned -join ' ')
    Assert-Case 'command: a positions file is passed' (($pinned -join ' ') -match '-Positions /p\.fen$') ($pinned -join ' ')

    $saved = '{"Affinity":0,"Rounds":12}'
    $pinnedMeta = Set-AffinityMetadata -Json $saved -Mask 12 | ConvertFrom-Json
    Assert-Case 'metadata: a taskset mask survives into metadata.json' ($pinnedMeta.Affinity -eq 12 -and $pinnedMeta.AffinityMechanism -eq 'taskset' -and $pinnedMeta.Rounds -eq 12) ($pinnedMeta | ConvertTo-Json -Compress)
    $plainMeta = Set-AffinityMetadata -Json $saved -Mask 0 | ConvertFrom-Json
    Assert-Case 'metadata: FALSIFY: an unpinned run records no mechanism' ($plainMeta.Affinity -eq 0 -and $plainMeta.AffinityMechanism -eq 'none') ($plainMeta | ConvertTo-Json -Compress)

    $label = Get-RefLabel -Ref ([pscustomobject]@{ Commit = '0123456789abcdef'; Dirty = $true })
    Assert-Case 'label: short commit plus uncommitted marker' ($label -eq '012345678+uncommitted') "got $label"

    Write-Host ''
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host 'All self-test cases passed.' -ForegroundColor Green
    exit 0
}

# --- main ------------------------------------------------------------------

if (-not $Baseline -or -not $Candidate) { throw '-Baseline and -Candidate are both required.' }
$toolCheck = Invoke-Wsl -Argument 'which', 'pwsh', 'cmake', 'ninja', 'g++', 'tar' 2>&1
if ($LASTEXITCODE -ne 0) { throw "WSL distro $WslDistro lacks a tool (pwsh, cmake, ninja, g++, tar) or is not installed: $toolCheck" }

$refs = [ordered]@{
    baseline  = Resolve-BuildRef -Ref $Baseline -Arm 'Baseline' -Repo $RepoRoot
    candidate = Resolve-BuildRef -Ref $Candidate -Arm 'Candidate' -Repo $RepoRoot
}
$positionFile = if ($Positions) { ConvertTo-WslPath (Resolve-Path -LiteralPath $Positions).Path } else { '' }

if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\compare-bench-linux\$(Get-Date -Format 'yyyyMMdd-HHmmss')" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outPath = (Resolve-Path -LiteralPath $OutDir).Path
if (@(Get-ChildItem -LiteralPath $outPath -Force).Count -gt 0) { throw "$outPath is not empty; use a fresh -OutDir." }

# Binaries, CSVs and the control copy all live under one ext4 directory, deleted at the end.
$wslRoot = "$(Invoke-Wsl -Argument 'printenv', 'HOME')/strat-compare-bench"
$depsDir = Get-WslDepsDir
$wslRun = "$wslRoot/$(Split-Path $outPath -Leaf)"

$suites = ($Rounds + 1) * 3
Write-Host "Output: $outPath" -ForegroundColor Cyan
Write-PhasePlan -Phase @(
    @{ Name = 'build baseline'; Quiet = $false; Seconds = $RoughBuildSeconds; Rough = $true }
    @{ Name = 'build candidate'; Quiet = $false; Seconds = $RoughBuildSeconds; Rough = $true }
    @{ Name = "Compare-Bench ($suites suite runs)"; Quiet = $true; Seconds = $RoughSuiteSeconds * $suites; Rough = $true }
)

try {
    $exes = @{}
    $buildSeconds = $null
    foreach ($arm in $refs.Keys) {
        $ref = $refs[$arm]
        $estimate = if ($null -ne $buildSeconds) { $buildSeconds } else { $RoughBuildSeconds }
        Write-PhaseBanner -Name "build $arm ($($ref.Ref) @ $(Get-RefLabel -Ref $ref))" -Quiet $false -Seconds $estimate -Rough ($null -eq $buildSeconds)
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        $exes[$arm] = Build-WslVariant -Ref $ref -Repo $RepoRoot -StageDir (Join-Path $outPath $arm) -WslBinDir "$wslRun/$arm" `
            -WslWorkDir "$wslRun/$arm-build" -WslDepsDir $depsDir -CxxFlags ''
        if ($null -eq $buildSeconds) { $buildSeconds = $timer.Elapsed.TotalSeconds }
        Write-Host ("  built in {0:N0} s" -f $timer.Elapsed.TotalSeconds) -ForegroundColor DarkGray
    }

    Write-PhaseBanner -Name "Compare-Bench ($suites suite runs)" -Quiet $true -Seconds ($RoughSuiteSeconds * $suites) -Rough $true
    $command = @(Get-CompareBenchArgument -Script (ConvertTo-WslPath (Join-Path $PSScriptRoot 'Compare-Bench.ps1')) `
        -BaselineExe $exes.baseline -CandidateExe $exes.candidate -Out "$wslRun/compare" -RoundCount $Rounds -SearchDepth $Depth `
        -FloorMs $MinTimeMs -PositionFile $positionFile -Mask $Affinity `
        -BaselineLabel (Get-RefLabel -Ref $refs.baseline) -CandidateLabel (Get-RefLabel -Ref $refs.candidate))
    Invoke-Wsl -Argument $command | Out-Host
    $compareExit = $LASTEXITCODE
    Write-PhaseBanner -Name 'copy results' -Quiet $false -Seconds 5 -Rough $true

    $resultDir = Join-Path $outPath 'compare'
    New-Item -ItemType Directory -Force -Path $resultDir | Out-Null
    Invoke-Wsl -Argument 'test', '-d', "$wslRun/compare"
    if ($LASTEXITCODE -eq 0) { Invoke-Wsl -Argument 'cp', '-r', "$wslRun/compare/.", (ConvertTo-WslPath $resultDir) | Out-Host }
    if ($compareExit -ne 0) { throw "Compare-Bench failed in WSL (exit $compareExit); whatever it wrote is in $resultDir." }
}
finally {
    Invoke-Wsl -Argument 'rm', '-rf', $wslRun | Out-Host
}

$metadataPath = Join-Path $outPath 'compare\metadata.json'
Set-AffinityMetadata -Json (Get-Content -LiteralPath $metadataPath -Raw) -Mask $Affinity | Set-Content -LiteralPath $metadataPath

$compiler = (Get-Content -LiteralPath (Join-Path $outPath 'baseline\build.log') -Tail 1).Trim()
$note = "GCC Release in WSL $WslDistro ($compiler)."
$reportPath = Join-Path $outPath 'compare\report.txt'
@($note) + @(Get-Content -LiteralPath $reportPath) | Set-Content -LiteralPath $reportPath
Write-Host ''
Write-Host $note
Write-Host "Build logs, CSVs, metadata.json and report.txt: $outPath"
