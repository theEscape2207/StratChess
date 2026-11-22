<#
.SYNOPSIS
    Check out a baseline commit in a detached worktree under %TEMP% and build it the way the
    candidate is built, for Compare-Bench.ps1 and New-OrderedBuildPair.ps1.

.DESCRIPTION
    A paired bench needs a baseline exe from the same build path as the candidate: an exe from any
    other path is a different binary that benches differently. Doing that by hand meets three
    traps, which this removes:

    1. **The worktree guard refuses a baseline under .claude/worktrees.** The tree goes under
       %TEMP%, at a short path that stays clear of MAX_PATH.
    2. **build.ps1 builds the shell's worktree, not its own.** The build runs with the baseline
       tree as its working directory.
    3. **A stale or edited tree.** The path carries the commit, an existing worktree is reused
       only when it sits at that commit with no tracked changes, and any other directory there is
       refused rather than overwritten.

    Prints the tree, the exe and the commit. -Remove deletes the worktree again.

.PARAMETER Ref
    The baseline commit-ish. Default: the merge base of HEAD and origin/main, fetched first, so
    the baseline stays the commit the task forked from after origin/main moves.

.PARAMETER Remove
    Remove the worktree for -Ref instead of creating it.

.PARAMETER SelfTest
    Run the pure path and reuse-decision cases and exit. No git, no build.

.WHEN TO USE
    Before a Compare-Bench.ps1 series against the merge base (skill measure-strength,
    reference/regression-check.md), and for -BaselineTree of New-OrderedBuildPair.ps1.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\New-BaselineTree.ps1
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\New-BaselineTree.ps1 -Ref b04be0c2 -Remove

.NOTES
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>

[CmdletBinding()]
param(
    [string]$Ref = '',
    [switch]$Remove,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-BaselineTreePath {
    <# The tree's path for a commit: short, under the temp root, named by the commit. #>
    param([Parameter(Mandatory)][string]$TempRoot, [Parameter(Mandatory)][string]$Commit)
    return Join-Path $TempRoot ('sce-base-' + $Commit.Substring(0, 8))
}

function Get-TreeDecision {
    <#
      .SYNOPSIS
        Create, Reuse or Refuse for a path, given 'git worktree list --porcelain', whether the
        path exists and whether it has tracked changes. Reuse only a clean registered worktree
        whose HEAD is the commit.
    #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Commit,
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Porcelain,
        [Parameter(Mandatory)][bool]$PathExists,
        [bool]$Dirty = $false
    )
    $wanted = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $current = $null
    foreach ($line in $Porcelain) {
        if ($line.StartsWith('worktree ')) {
            $current = [System.IO.Path]::GetFullPath($line.Substring(9)).TrimEnd('\', '/')
            continue
        }
        if ($current -and $line.StartsWith('HEAD ') -and $current -ieq $wanted) {
            if ($line.Substring(5) -eq $Commit -and -not $Dirty) { return 'Reuse' }
            return 'Refuse'
        }
    }
    if ($PathExists) { return 'Refuse' }
    return 'Create'
}

if ($SelfTest) {
    $failures = 0
    $sha = 'b04be0c204b4e1a9ebfe010484a13f631895c37a'
    $other = '6787d5f6aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    $tree = Get-BaselineTreePath -TempRoot 'C:\Temp' -Commit $sha
    $listed = @("worktree C:/Repo", "HEAD $other", 'branch refs/heads/main', '', 'worktree C:/Temp/sce-base-b04be0c2', "HEAD $sha", 'detached', '')
    $stale = @("worktree C:/Repo", "HEAD $other", '', 'worktree C:/Temp/sce-base-b04be0c2', "HEAD $other", 'detached', '')
    $cases = @(
        @{ Name = 'path is the temp root plus the short commit'; Actual = $tree; Expect = 'C:\Temp\sce-base-b04be0c2' }
        @{ Name = 'absent path -> Create'; Actual = (Get-TreeDecision -Path $tree -Commit $sha -Porcelain @("worktree C:/Repo", "HEAD $other") -PathExists $false); Expect = 'Create' }
        @{ Name = 'registered at the commit -> Reuse'; Actual = (Get-TreeDecision -Path $tree -Commit $sha -Porcelain $listed -PathExists $true); Expect = 'Reuse' }
        @{ Name = 'FALSIFY: registered at the commit with tracked changes -> Refuse'; Actual = (Get-TreeDecision -Path $tree -Commit $sha -Porcelain $listed -PathExists $true -Dirty $true); Expect = 'Refuse' }
        @{ Name = 'registered at another commit -> Refuse'; Actual = (Get-TreeDecision -Path $tree -Commit $sha -Porcelain $stale -PathExists $true); Expect = 'Refuse' }
        @{ Name = 'unregistered directory in the way -> Refuse'; Actual = (Get-TreeDecision -Path $tree -Commit $sha -Porcelain @("worktree C:/Repo", "HEAD $sha") -PathExists $true); Expect = 'Refuse' }
        @{ Name = 'another worktree at the commit is not this one -> Create'; Actual = (Get-TreeDecision -Path $tree -Commit $sha -Porcelain @("worktree C:/Repo", "HEAD $sha") -PathExists $false); Expect = 'Create' }
    )
    foreach ($case in $cases) {
        if ($case.Actual -ceq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else { Write-Host "  FAIL  $($case.Name) (expected '$($case.Expect)', got '$($case.Actual)')" -ForegroundColor Red; $failures++ }
    }
    if ($failures -gt 0) { exit 1 }
    exit 0
}

& git fetch origin main 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: git fetch origin main failed.' -ForegroundColor Red; exit 1 }
if (-not $Ref) {
    $Ref = "$(& git merge-base HEAD origin/main)".Trim()
    if ($LASTEXITCODE -ne 0 -or -not $Ref) { Write-Host 'FAIL: HEAD and origin/main have no merge base.' -ForegroundColor Red; exit 1 }
}
$baseCommit = "$(& git rev-parse --verify --quiet "$Ref^{commit}")".Trim()
if ($LASTEXITCODE -ne 0 -or -not $baseCommit) { Write-Host "FAIL: '$Ref' does not name a commit." -ForegroundColor Red; exit 1 }
$shortCommit = $baseCommit.Substring(0, 8)

$treePath = Get-BaselineTreePath -TempRoot ([System.IO.Path]::GetTempPath()) -Commit $baseCommit
$worktreeLines = @(& git worktree list --porcelain)
$pathExists = Test-Path -LiteralPath $treePath
$hasTrackedChanges = $pathExists -and @(& git -C $treePath status --porcelain --untracked-files=no 2>$null).Count -gt 0
$decision = Get-TreeDecision -Path $treePath -Commit $baseCommit -Porcelain $worktreeLines -PathExists $pathExists -Dirty $hasTrackedChanges

if ($Remove) {
    if ($decision -ne 'Reuse') { Write-Host "FAIL: no baseline worktree for $shortCommit at $treePath." -ForegroundColor Red; exit 1 }
    & git worktree remove $treePath 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: git worktree remove $treePath failed." -ForegroundColor Red; exit 1 }
    Write-Host "PASS: removed $treePath." -ForegroundColor Green
    exit 0
}

if ($decision -eq 'Refuse') {
    Write-Host "FAIL: $treePath exists but is not a clean worktree at $shortCommit. Remove it, then re-run." -ForegroundColor Red
    exit 1
}
if ($decision -eq 'Create') {
    Write-Host "==> Adding a detached worktree at $treePath" -ForegroundColor Cyan
    & git worktree add --detach $treePath $baseCommit 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: git worktree add failed.' -ForegroundColor Red; exit 1 }
}
else {
    Write-Host "==> Reusing the worktree at $treePath" -ForegroundColor Cyan
}

# build.ps1 builds the tree it is run from, so the working directory is the baseline's.
Write-Host '==> Building the baseline (build.ps1 main)' -ForegroundColor Cyan
Push-Location -LiteralPath $treePath
try {
    & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $treePath 'build.ps1') main | Out-Host
    $buildExit = $LASTEXITCODE
    $exe = @(& pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $treePath 'Scripts\Get-BuildArtifact.ps1'))
    $artifactExit = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($buildExit -ne 0) { Write-Host "FAIL: the baseline build failed (exit $buildExit)." -ForegroundColor Red; exit 1 }
if ($artifactExit -ne 0 -or $exe.Count -eq 0) { Write-Host 'FAIL: no baseline exe after the build.' -ForegroundColor Red; exit 1 }
$exePath = "$($exe[-1])".Trim()

Write-Host ''
Write-Host "PASS: baseline $shortCommit built." -ForegroundColor Green
Write-Host "  Tree   : $treePath"
Write-Host "  Exe    : $exePath"
Write-Host "  Commit : $baseCommit"
Write-Host ''
Write-Host 'Next:' -ForegroundColor Cyan
Write-Host "  Compare-Bench.ps1 -Baseline $exePath -Candidate <exe> -BaselineCommit $shortCommit -CandidateCommit <sha>"
Write-Host "  New-BaselineTree.ps1 -Ref $shortCommit -Remove   # when done"
