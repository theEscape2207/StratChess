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

    Never pushes anywhere. Never touches uncommitted changes -- aborts cleanly if the
    working tree is dirty rather than risk losing anything. Restores whatever branch
    was checked out before the script ran, if it had to switch to master first.

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

Write-Host "`n==> Checking working tree" -ForegroundColor Cyan
$status = Invoke-Git @('status', '--porcelain')
if ($status.Output) {
    Write-Host "FAIL: working tree has uncommitted changes. Commit, stash, or switch away before syncing master." -ForegroundColor Red
    Write-GitOutput $status.Output
    exit 1
}
Write-Host "PASS: working tree is clean." -ForegroundColor Green

$originalBranch = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Output

Write-Host "`n==> Fetching origin/main" -ForegroundColor Cyan
$fetch = Invoke-Git @('fetch', 'origin', 'main')
if ($fetch.ExitCode -ne 0) {
    Write-Host "FAIL: git fetch origin main failed." -ForegroundColor Red
    Write-GitOutput $fetch.Output
    exit 1
}
Write-Host "PASS: fetched." -ForegroundColor Green

$switchedBranch = $false
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
        exit 1
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
        if ($switchedBranch) { Invoke-Git @('checkout', $originalBranch) | Out-Null }
        exit 1
    }
    $mergeSha = (Invoke-Git @('rev-parse', '--short', 'HEAD')).Output
    Write-Host "PASS: merged origin/main into master (new commit $mergeSha); local-only commits preserved." -ForegroundColor Green
}

if ($switchedBranch) {
    Write-Host "`n==> Switching back to '$originalBranch'" -ForegroundColor Cyan
    Invoke-Git @('checkout', $originalBranch) | Out-Null
}

Write-Host "`nSync complete." -ForegroundColor Green
exit 0
