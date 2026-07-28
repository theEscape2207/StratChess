<#
.SYNOPSIS
    Tear down a merged worktree: directory, local branch, and remote branch.

.DESCRIPTION
    Post-merge cleanup, which CLAUDE.md treats as part of finishing a task rather than an
    optional extra. Handles the traps that make doing it by hand unpleasant:

    1. **Never run from inside the worktree being removed.** `git worktree remove` cannot
       delete its own working directory: it deregisters the worktree but leaves an
       orphaned folder with no `.git`, and the shell's cwd is left pointing at it while
       git commands silently resolve against the *outer* repo instead. This script
       detects that case and refuses with instructions, rather than half-doing it.
    2. **Merge verification, not name-trust.** Before deleting anything it checks the
       branch is actually an ancestor of `origin/main` (`git merge-base --is-ancestor`).
       A branch whose commits were squash-merged is NOT an ancestor, so that case is
       reported explicitly with a content-diff hint rather than silently blocked or
       silently deleted.
    3. **Locked worktrees** are unlocked first, otherwise removal fails with a message
       that does not name the lock as the cause.

    Deletes the remote branch only if it exists. Optionally syncs `master` afterwards.

.PARAMETER Name
    Worktree directory name under `.claude/worktrees/`, e.g. 'eval-mobility-term'.

.PARAMETER Force
    Delete even if the branch is not merged into origin/main. Also passes --force to
    `git worktree remove` (discarding uncommitted changes in that worktree).

.PARAMETER KeepRemote
    Leave the remote branch alone.

.PARAMETER SyncMaster
    Run Sync-Master.ps1 afterwards so local master reflects the merge.

.WHEN TO USE
    After a PR merges. Equivalent to the `commit-commands:clean_gone` skill for a single
    known worktree.

.HOW TO INVOKE (from bash, cmd, or PowerShell) -- run from the MAIN checkout
    cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Remove-Worktree.ps1 -Name eval-mobility-term -SyncMaster"

.NOTES
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [switch]$Force,
    [switch]$KeepRemote,
    [switch]$SyncMaster
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonDir = & git rev-parse --path-format=absolute --git-common-dir 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: not inside a git repository." -ForegroundColor Red; exit 1 }
$MainCheckout = Split-Path $commonDir -Parent
$TargetPath   = Join-Path (Join-Path $MainCheckout '.claude\worktrees') $Name

# Trap 1: refuse to remove the worktree we are standing in.
$here = (Get-Location).Path
if ($here -eq $TargetPath -or $here.StartsWith($TargetPath + [IO.Path]::DirectorySeparatorChar)) {
    Write-Host "FAIL: you are inside the worktree being removed." -ForegroundColor Red
    Write-Host "      git cannot delete its own working directory -- it would deregister the" -ForegroundColor Yellow
    Write-Host "      worktree and leave an orphaned folder behind." -ForegroundColor Yellow
    Write-Host "      Run this from the main checkout instead:" -ForegroundColor Yellow
    Write-Host "        cd `"$MainCheckout`"" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $TargetPath)) {
    Write-Host "NOTE: no directory at $TargetPath -- will still try branch cleanup." -ForegroundColor Yellow
}

# Resolve the branch this worktree has checked out.
$branch = $null
$wtLines = & git -C $MainCheckout worktree list --porcelain
$currentPath = $null
foreach ($line in $wtLines) {
    if ($line -like 'worktree *')  { $currentPath = $line.Substring(9) }
    elseif ($line -like 'branch *' -and $currentPath) {
        $normalized = $currentPath -replace '/', '\'
        if ($normalized -eq $TargetPath) { $branch = $line.Substring(7) -replace '^refs/heads/', '' }
    }
}

if (-not $branch) {
    Write-Host "NOTE: could not resolve a branch for that worktree (detached HEAD, or not registered)." -ForegroundColor Yellow
} else {
    Write-Host "`n==> Worktree '$Name' -> branch '$branch'" -ForegroundColor Cyan
}

# Trap 2: verify merged, by ancestry -- not by branch name.
if ($branch -and -not $Force) {
    & git -C $MainCheckout fetch origin main | Out-Null
    & git -C $MainCheckout merge-base --is-ancestor $branch origin/main
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nFAIL: '$branch' is not an ancestor of origin/main -- refusing to delete." -ForegroundColor Red
        Write-Host "      If the PR was SQUASH-merged this is expected; confirm the content landed:" -ForegroundColor Yellow
        Write-Host "        git diff origin/main $branch --stat     # empty => safe to -Force" -ForegroundColor Yellow
        Write-Host "      Then re-run with -Force." -ForegroundColor Yellow
        exit 1
    }
    Write-Host "PASS: '$branch' is contained in origin/main." -ForegroundColor Green
}

# Trap 3: locked worktrees refuse removal with an unhelpful message.
if (Test-Path $TargetPath) {
    & git -C $MainCheckout worktree unlock $TargetPath 2>&1 | Out-Null

    Write-Host "`n==> Removing worktree" -ForegroundColor Cyan
    $rmArgs = @('worktree', 'remove', $TargetPath)
    if ($Force) { $rmArgs += '--force' }
    & git -C $MainCheckout @rmArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: git worktree remove failed." -ForegroundColor Red
        exit 1
    }
    Write-Host "PASS: worktree removed." -ForegroundColor Green
}
& git -C $MainCheckout worktree prune | Out-Null

if ($branch) {
    Write-Host "`n==> Deleting local branch" -ForegroundColor Cyan
    & git -C $MainCheckout branch -D $branch
    if ($LASTEXITCODE -eq 0) { Write-Host "PASS: local branch deleted." -ForegroundColor Green }

    if (-not $KeepRemote) {
        Write-Host "`n==> Deleting remote branch" -ForegroundColor Cyan
        $remoteRef = & git -C $MainCheckout ls-remote --heads origin $branch
        if ($remoteRef) {
            & git -C $MainCheckout push origin --delete $branch
            if ($LASTEXITCODE -eq 0) { Write-Host "PASS: remote branch deleted." -ForegroundColor Green }
        } else {
            Write-Host "Remote branch already gone." -ForegroundColor Green
        }
    }
}

if ($SyncMaster) {
    Write-Host "`n==> Syncing master" -ForegroundColor Cyan
    & pwsh -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Sync-Master.ps1')
}

Write-Host "`nCleanup complete." -ForegroundColor Green
