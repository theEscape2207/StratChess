<#
.SYNOPSIS
    Sync local `master` up to date with `origin/main`.

.DESCRIPTION
    `origin/main` is the single canonical shared branch (`origin/master` was retired --
    it was a leftover from an incomplete master->main rename and was never actually
    relied on as a backup). Local `master` is a personal/scratch integration branch:
    safe to commit to directly, safe to let drift, and this script is how it gets
    reconciled with origin/main on demand -- fast-forwarded when possible, merged
    (preserving any local-only commits) when master has diverged.

    Never pushes anywhere and never opens a PR -- this script only pulls origin/main
    down into master. (Publishing master's own local-only commits into main is a
    separate, not-yet-automated step: branch off origin/main, bring the commits over,
    push, open a PR.)

    A dirty working tree does not block the sync: uncommitted changes (tracked and
    untracked) are stashed first and popped back at the end, on whichever branch they
    started on. If the pop hits a conflict, git leaves the stash entry in place rather
    than dropping it -- nothing is lost, you just resolve the conflict and `git stash
    drop` yourself once satisfied. Restores whatever branch was checked out before the
    script ran, if it had to switch to master first -- this always happens, even if the
    sync itself fails partway through.

.WHEN TO USE
    - At the start of a session that will work directly in the main checkout (not a
      worktree), to make sure `master` reflects everything merged into `origin/main`
      since it was last synced.
    - Any time a PR merges into main and local master needs to catch up.
    - Safe to run unconditionally / repeatedly -- it's a no-op if already up to date.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Sync-Master.ps1"

.NOTES
    Must be run from the main repository checkout, not a `.claude/worktrees/*` worktree
    -- git will not let this script check out `master` if it's already checked out in
    another worktree (which it always will be, since worktrees fork their own branch).
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$GameDir  = Split-Path $PSScriptRoot -Parent
$RepoRoot = Split-Path $GameDir -Parent

function Invoke-Git {
    param([string[]]$GitArgs)
    $output = & git -C $RepoRoot @GitArgs 2>&1
    return @{ Output = $output; ExitCode = $LASTEXITCODE }
}

function Write-GitOutput {
    param($Output)
    if ($Output) { Write-Host (($Output | Out-String).Trim()) }
}

# Restores whatever this run changed (branch, stash) and exits. Called from every exit
# point instead of a bare `exit N` so cleanup always runs, success or failure -- a raw
# `exit` does not reliably trigger try/finally in a plain script, so this is explicit.
function Restore-AndExit {
    param(
        [int]$Code,
        [string]$OriginalBranch,
        [bool]$SwitchedBranch,
        [bool]$Stashed
    )
    if ($SwitchedBranch) {
        $current = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Output
        if ($current -ne $OriginalBranch) {
            Write-Host "`n==> Restoring original branch '$OriginalBranch'" -ForegroundColor Cyan
            Invoke-Git @('checkout', $OriginalBranch) | Out-Null
        }
    }
    if ($Stashed) {
        Write-Host "`n==> Restoring stashed changes" -ForegroundColor Cyan
        $pop = Invoke-Git @('stash', 'pop')
        if ($pop.ExitCode -ne 0) {
            Write-Host "WARNING: stash pop hit a conflict -- your changes are safely preserved in the stash (not dropped)." -ForegroundColor Yellow
            Write-GitOutput $pop.Output
            Write-Host "Resolve the conflict, then run 'git stash drop' once you're satisfied nothing was lost." -ForegroundColor Yellow
        } else {
            Write-Host "PASS: stashed changes restored." -ForegroundColor Green
        }
    }
    exit $Code
}

Write-Host "`n==> Checking working tree" -ForegroundColor Cyan
$originalBranch = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Output
$switchedBranch = $false
$stashed = $false

$status = Invoke-Git @('status', '--porcelain')
if ($status.Output) {
    Write-Host "Working tree has uncommitted changes -- stashing before sync." -ForegroundColor Yellow
    $stash = Invoke-Git @('stash', 'push', '-u', '-m', 'Sync-Master.ps1 autostash')
    if ($stash.ExitCode -ne 0) {
        Write-Host "FAIL: could not stash uncommitted changes." -ForegroundColor Red
        Write-GitOutput $stash.Output
        Restore-AndExit -Code 1 -OriginalBranch $originalBranch -SwitchedBranch $switchedBranch -Stashed $stashed
    }
    $stashed = $true
    Write-Host "PASS: uncommitted changes stashed." -ForegroundColor Green
} else {
    Write-Host "PASS: working tree is clean." -ForegroundColor Green
}

Write-Host "`n==> Fetching origin/main" -ForegroundColor Cyan
$fetch = Invoke-Git @('fetch', 'origin', 'main')
if ($fetch.ExitCode -ne 0) {
    Write-Host "FAIL: git fetch origin main failed." -ForegroundColor Red
    Write-GitOutput $fetch.Output
    Restore-AndExit -Code 1 -OriginalBranch $originalBranch -SwitchedBranch $switchedBranch -Stashed $stashed
}
Write-Host "PASS: fetched." -ForegroundColor Green

if ($originalBranch -ne 'master') {
    Write-Host "`n==> Switching to master (was on '$originalBranch')" -ForegroundColor Cyan
    $checkout = Invoke-Git @('checkout', 'master')
    if ($checkout.ExitCode -ne 0) {
        Write-Host "FAIL: could not check out master." -ForegroundColor Red
        Write-GitOutput $checkout.Output
        $worktrees = (Invoke-Git @('worktree', 'list')).Output
        $masterLine = $worktrees | Where-Object { $_ -match '\[master\]' }
        if ($masterLine) {
            $masterPath = ($masterLine -split '\s+')[0]
            Write-Host "master is checked out at: $masterPath -- run this script from there instead." -ForegroundColor Yellow
        }
        Restore-AndExit -Code 1 -OriginalBranch $originalBranch -SwitchedBranch $switchedBranch -Stashed $stashed
    }
    $switchedBranch = $true
}

Write-Host "`n==> Syncing master with origin/main" -ForegroundColor Cyan
$before = (Invoke-Git @('rev-parse', 'master')).Output
$ff = Invoke-Git @('merge', 'origin/main', '--ff-only')
if ($ff.ExitCode -eq 0) {
    $after = (Invoke-Git @('rev-parse', 'master')).Output
    if ($before -eq $after) {
        Write-Host "PASS: master already up to date with origin/main." -ForegroundColor Green
    } else {
        $count = (Invoke-Git @('rev-list', '--count', "$before..$after")).Output
        Write-Host "PASS: fast-forwarded master by $count commit(s)." -ForegroundColor Green
    }
} else {
    Write-Host "master has diverged from origin/main -- merging (preserves local-only commits)" -ForegroundColor Yellow
    $merge = Invoke-Git @('merge', 'origin/main', '-m', 'Merge origin/main (Sync-Master.ps1)')
    if ($merge.ExitCode -ne 0) {
        Write-Host "FAIL: merge produced conflicts -- aborting merge, master left untouched." -ForegroundColor Red
        Invoke-Git @('merge', '--abort') | Out-Null
        Write-GitOutput $merge.Output
        Write-Host "Resolve manually: git checkout master; git merge origin/main" -ForegroundColor Yellow
        Restore-AndExit -Code 1 -OriginalBranch $originalBranch -SwitchedBranch $switchedBranch -Stashed $stashed
    }
    $mergeSha = (Invoke-Git @('rev-parse', '--short', 'HEAD')).Output
    Write-Host "PASS: merged origin/main into master (new commit $mergeSha); local-only commits preserved." -ForegroundColor Green
}

Write-Host "`nSync complete." -ForegroundColor Green
Restore-AndExit -Code 0 -OriginalBranch $originalBranch -SwitchedBranch $switchedBranch -Stashed $stashed
