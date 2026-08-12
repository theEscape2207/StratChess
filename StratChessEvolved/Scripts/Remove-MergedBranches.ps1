<#
.SYNOPSIS
    Delete local branches already merged into origin/main.

.DESCRIPTION
    The in-place counterpart to Remove-Worktree.ps1's branch cleanup, for the mode where
    tasks are branches in one worktree rather than directories. It asks one question per
    branch -- "is this contained in origin/main?" -- and answers it with
    `git merge-base --is-ancestor`, never by matching a name. A branch called
    `worktree-old-thing` may still hold unpushed work, and a branch whose name looks
    unfamiliar may be fully merged; only ancestry is evidence.

    `git branch -d` is not a substitute: it tests whether a branch merged into the CURRENT
    branch, which is a different question and the wrong one here. This verifies against
    origin/main first, then deletes with -D.

    Never deletes: master, main, the branch you are on, and any branch checked out in
    another worktree (git would refuse those anyway).

.PARAMETER SyncMaster
    Run Sync-Master.ps1 afterwards so local master reflects the merges.

.PARAMETER DryRun
    Report what would be deleted and delete nothing.

.WHEN TO USE
    After one or more PRs merge, when working in-place. The worktree flow uses
    Remove-Worktree.ps1 instead, which also removes the directory.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\StratChessEvolved\Scripts\Remove-MergedBranches.ps1 -SyncMaster

.NOTES
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
#>

[CmdletBinding()]
param(
    [switch]$SyncMaster,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

& git rev-parse --git-dir *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: not inside a git repository." -ForegroundColor Red
    exit 1
}

Write-Host "`n==> Fetching origin/main" -ForegroundColor Cyan
& git fetch origin main --prune
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: git fetch origin main failed." -ForegroundColor Red
    exit 1
}

# $null (not "HEAD") when detached, so the current-branch comparisons below never
# match a real branch name and the merged-branch NOTE is skipped -- there is nothing
# to move off of.
$currentBranch = & git symbolic-ref -q --short HEAD 2>$null
if ($LASTEXITCODE -ne 0) { $currentBranch = $null }

# A branch checked out in another worktree cannot be deleted, so it is skipped with a
# reason rather than attempted and reported as an error.
$checkedOutElsewhere = @()
foreach ($line in (& git worktree list --porcelain)) {
    if ($line -like 'branch *') {
        $b = ($line.Substring(7) -replace '^refs/heads/', '').Trim()
        if ($b -and $b -ne $currentBranch) { $checkedOutElsewhere += $b }
    }
}

$deleted = @()
$kept    = @()
# for-each-ref lists only real branches; `git branch` also emits a synthetic
# "(HEAD detached at ...)" line when detached, which is not a valid object name and
# would trip merge-base --is-ancestor below.
foreach ($branch in (& git for-each-ref refs/heads --format='%(refname:short)')) {
    $branch = $branch.Trim()
    if (-not $branch) { continue }
    if ($branch -in @('master', 'main')) { continue }
    if ($branch -eq $currentBranch)      { continue }
    if ($branch -in $checkedOutElsewhere) {
        $kept += "$branch (checked out in another worktree)"
        continue
    }

    & git merge-base --is-ancestor $branch origin/main
    if ($LASTEXITCODE -ne 0) {
        $kept += "$branch (not merged into origin/main)"
        continue
    }

    if ($DryRun) {
        $deleted += "$branch (dry run -- not deleted)"
        continue
    }

    & git branch -D $branch *> $null
    if ($LASTEXITCODE -eq 0) { $deleted += $branch }
    else                     { $kept += "$branch (delete failed)" }
}

Write-Host "`n--- Merged into origin/main ---" -ForegroundColor Cyan
if ($deleted.Count -eq 0) { Write-Host "  none" }
else { $deleted | ForEach-Object { Write-Host "  deleted: $_" -ForegroundColor Green } }

if ($kept.Count -gt 0) {
    Write-Host "`n--- Kept ---" -ForegroundColor Cyan
    $kept | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
}

# Reported rather than acted on: moving someone's HEAD unasked is surprising, and
# New-TaskBranch.ps1 is the thing that moves you off it.
if ($currentBranch -and $currentBranch -notin @('master', 'main')) {
    & git merge-base --is-ancestor $currentBranch origin/main
    if ($LASTEXITCODE -eq 0) {
        Write-Host "`nNOTE: the branch you are on ('$currentBranch') is also merged." -ForegroundColor Yellow
        Write-Host "      Start the next task with New-TaskBranch.ps1, which moves you off it," -ForegroundColor Yellow
        Write-Host "      then re-run this to clean it up." -ForegroundColor Yellow
    }
}

if ($SyncMaster) {
    Write-Host "`n==> Syncing master" -ForegroundColor Cyan
    & pwsh -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Sync-Master.ps1')
}

exit 0
