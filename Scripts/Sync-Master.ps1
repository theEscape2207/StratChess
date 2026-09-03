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
    untracked) are stashed first and restored at the end, on whichever branch they
    started on. The entry is addressed by its commit SHA, never by its position on the
    stack -- the stash stack is shared by every worktree in the repository, so `stash@{0}`
    at restore time need not be the entry this script pushed. If the restore hits a
    conflict, the entry stays on the stack rather than being dropped -- nothing is lost,
    you just resolve the conflict and `git stash drop` yourself once satisfied. Restores
    whatever branch was checked out before the script ran, if it had to switch to master
    first -- this always happens, even if the sync itself fails partway through.

.WHEN TO USE
    - At the start of a session that will work directly in the main checkout (not a
      worktree), to make sure `master` reflects everything merged into `origin/main`
      since it was last synced.
    - Any time a PR merges into main and local master needs to catch up.
    - Safe to run unconditionally / repeatedly -- it's a no-op if already up to date.

.PARAMETER SelfTest
    Run the stash-restore assertions against a throwaway fixture repository and exit.
    Touches nothing outside the fixture. Exits 1 on any failure.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Sync-Master.ps1
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Sync-Master.ps1 -SelfTest

.NOTES
    Runs from anywhere in the repository. `master` can be checked out in only one
    worktree, so the script finds that worktree and syncs there. When that is not the
    tree it was invoked from, it refuses on a dirty target instead of stashing -- the
    stash stack is shared across worktrees, and those changes are not this script's to
    move.
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>

[CmdletBinding()]
param(
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$LocalRoot = Split-Path $PSScriptRoot -Parent
# A branch can be checked out in only one worktree, so `master` has exactly one home and
# the sync has to happen there. Refs and objects are shared, so nothing else cares where
# it runs from. Finding that home is what lets this script run from anywhere: git refuses
# even a ref-only `git fetch origin main:master` aimed at a branch checked out elsewhere,
# so working in the owning tree is the only option available.
function Get-MasterWorktree {
    param([string]$Root)
    $path = $null
    foreach ($line in (& git -C $Root worktree list --porcelain 2>$null)) {
        if ($line -like 'worktree *') { $path = $line.Substring(9) }
        elseif ($line -eq 'branch refs/heads/master' -and $path) { return $path }
    }
    return $null
}

# `worktree list` reports forward slashes on Windows while $PSScriptRoot-derived paths
# use backslashes, so the two need normalising before they can be compared.
function ConvertTo-ComparablePath {
    param([string]$Path)
    return ($Path -replace '/', '\').TrimEnd('\')
}

$masterHome = Get-MasterWorktree -Root $LocalRoot
$Delegated  = $false
$RepoRoot   = $LocalRoot
if ($masterHome -and
    (ConvertTo-ComparablePath $masterHome) -ne (ConvertTo-ComparablePath $LocalRoot)) {
    $Delegated = $true
    $RepoRoot  = $masterHome
}

function Invoke-Git {
    param([string[]]$GitArgs)
    $output = & git -C $RepoRoot @GitArgs 2>&1
    return @{ Output = $output; ExitCode = $LASTEXITCODE }
}

function Write-GitOutput {
    param($Output)
    if ($Output) { Write-Host (($Output | Out-String).Trim()) }
}

# Current position of a stash entry, identified by its commit SHA. -1 when the entry is
# no longer on the stack. Positions shift: an entry pushed by a concurrent session in
# another worktree lands on top and renumbers everything below it.
function Get-StashIndex {
    param([Parameter(Mandatory)][string]$Sha)
    $entries = @((Invoke-Git @('stash', 'list', '--format=%H')).Output | Where-Object { $_ })
    for ($i = 0; $i -lt $entries.Count; $i++) {
        if ("$($entries[$i])".Trim() -eq $Sha) { return $i }
    }
    return -1
}

# Restores one stash entry by SHA and drops it, rather than `git stash pop`. The stash
# stack is shared by every worktree in the repository, so `stash@{0}` is whatever sits on
# top at that moment -- pop would apply a concurrent session's entry into this tree and
# leave that session's changes gone. Apply-then-drop is two steps precisely so a conflict
# leaves the entry on the stack.
function Restore-StashBySha {
    param([Parameter(Mandatory)][string]$Sha)

    $apply = Invoke-Git @('stash', 'apply', $Sha)
    if ($apply.ExitCode -ne 0) {
        return @{ Applied = $false; Dropped = $false; Output = $apply.Output }
    }
    $index   = Get-StashIndex -Sha $Sha
    $dropped = $false
    if ($index -ge 0) {
        $drop    = Invoke-Git @('stash', 'drop', "stash@{$index}")
        $dropped = ($drop.ExitCode -eq 0)
    }
    return @{ Applied = $true; Dropped = $dropped; Output = $apply.Output }
}

# Restores whatever this run changed (branch, stash) and exits. Called from every exit
# point instead of a bare `exit N` so cleanup always runs, success or failure -- a raw
# `exit` does not reliably trigger try/finally in a plain script, so this is explicit.
function Restore-AndExit {
    param(
        [int]$Code,
        [string]$OriginalBranch,
        [string]$OriginalCommit,
        [bool]$WasDetached,
        [bool]$SwitchedBranch,
        [string]$StashSha
    )
    if ($SwitchedBranch) {
        # A detached HEAD reports as the literal string 'HEAD' from
        # rev-parse --abbrev-ref, which is not a checkout-able branch name --
        # `git checkout HEAD` while already on master is a no-op that leaves
        # master checked out and silently fails to restore the detached state.
        if ($WasDetached) {
            $currentCommit = (Invoke-Git @('rev-parse', 'HEAD')).Output
            if ($currentCommit -ne $OriginalCommit) {
                Write-Host "`n==> Restoring detached HEAD at $OriginalCommit" -ForegroundColor Cyan
                Invoke-Git @('checkout', '--detach', $OriginalCommit) | Out-Null
            }
        } else {
            $current = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Output
            if ($current -ne $OriginalBranch) {
                Write-Host "`n==> Restoring original branch '$OriginalBranch'" -ForegroundColor Cyan
                Invoke-Git @('checkout', $OriginalBranch) | Out-Null
            }
        }
    }
    if ($StashSha) {
        Write-Host "`n==> Restoring stashed changes" -ForegroundColor Cyan
        $restore = Restore-StashBySha -Sha $StashSha
        if (-not $restore.Applied) {
            Write-Host "WARNING: restoring the autostash hit a conflict -- your changes are safely preserved in the stash (not dropped)." -ForegroundColor Yellow
            Write-GitOutput $restore.Output
            Write-Host "Resolve the conflict, then drop entry $StashSha -- 'git stash list' to find its index, 'git stash drop stash@{n}'." -ForegroundColor Yellow
        } elseif (-not $restore.Dropped) {
            Write-Host "PASS: stashed changes restored." -ForegroundColor Green
            Write-Host "WARNING: could not drop the applied entry $StashSha -- 'git stash list' to find its index, 'git stash drop stash@{n}'." -ForegroundColor Yellow
        } else {
            Write-Host "PASS: stashed changes restored." -ForegroundColor Green
        }
    }
    exit $Code
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
if ($SelfTest) {
    # A fixture repository rather than mocks: what is under test is *which* entry comes
    # back off a shared stash stack, and only real git plumbing can get that wrong.
    $fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) "sync-master-selftest-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    $RepoRoot = $fixtureRoot
    $mineFile  = Join-Path $fixtureRoot 'mine.txt'
    $decoyFile = Join-Path $fixtureRoot 'decoy.txt'

    try {
        Invoke-Git @('init', '--quiet', '--initial-branch=master') | Out-Null
        Invoke-Git @('config', 'user.email', 'selftest@example.invalid') | Out-Null
        Invoke-Git @('config', 'user.name', 'Sync-Master self-test') | Out-Null
        Set-Content $mineFile  'base' -NoNewline
        Set-Content $decoyFile 'base' -NoNewline
        Invoke-Git @('add', 'mine.txt', 'decoy.txt') | Out-Null
        Invoke-Git @('-c', 'commit.gpgsign=false', 'commit', '--quiet', '--no-verify', '-m', 'base') | Out-Null

        # The falsification case. Our entry is pushed first, a concurrent session's entry
        # lands on top of it, and only then do we restore. A bare `git stash pop` takes
        # stash@{0}, so it would apply the decoy into this tree and leave our own changes
        # stashed -- exactly the failure this addressing-by-SHA exists to prevent.
        Set-Content $mineFile 'mine' -NoNewline
        Invoke-Git @('stash', 'push', '-u', '-m', 'Sync-Master.ps1 autostash') | Out-Null
        $ourSha = "$((Invoke-Git @('rev-parse', 'stash@{0}')).Output)".Trim()
        Set-Content $decoyFile 'decoy' -NoNewline
        Invoke-Git @('stash', 'push', '-u', '-m', 'another session') | Out-Null

        # Read the tree back before the next case rewrites it -- the assertion table below
        # runs once, at the end, over facts captured as each case produced them.
        $restored     = Restore-StashBySha -Sha $ourSha
        $remaining    = @((Invoke-Git @('stash', 'list', '--format=%gs')).Output | Where-Object { $_ })
        $mineAfter    = Get-Content $mineFile -Raw
        $decoyAfter   = Get-Content $decoyFile -Raw

        # Clean tree, empty stack, for the next case.
        Invoke-Git @('reset', '--hard', '--quiet') | Out-Null
        Invoke-Git @('stash', 'drop', 'stash@{0}') | Out-Null

        # An entry no longer on the stack must fail loudly, not apply something else.
        Set-Content $mineFile 'untouched' -NoNewline
        $missing        = Restore-StashBySha -Sha ('0' * 40)
        $mineAfterMiss  = Get-Content $mineFile -Raw

        $cases = @(
            @{ Name = 'own entry applied';             Expect = 'True';      Actual = $restored.Applied }
            @{ Name = 'own entry dropped after apply'; Expect = 'True';      Actual = $restored.Dropped }
            @{ Name = 'own changes restored';          Expect = 'mine';      Actual = $mineAfter }
            @{ Name = 'decoy NOT applied';             Expect = 'base';      Actual = $decoyAfter }
            @{ Name = 'decoy left on the stack';       Expect = 'True';
               Actual = ($remaining.Count -eq 1 -and $remaining[0] -like '*another session*') }
            @{ Name = 'missing entry: not applied';    Expect = 'False';     Actual = $missing.Applied }
            @{ Name = 'missing entry: tree untouched'; Expect = 'untouched'; Actual = $mineAfterMiss }
        )

        $failed = 0
        foreach ($c in $cases) {
            $got = "$($c.Actual)"
            if ($got -eq $c.Expect) {
                Write-Host ("  PASS  {0,-32} -> {1}" -f $c.Name, $got) -ForegroundColor Green
            } else {
                Write-Host ("  FAIL  {0,-32} -> {1} (expected {2})" -f $c.Name, $got, $c.Expect) -ForegroundColor Red
                $failed++
            }
        }
        Write-Host ''
        if ($failed -gt 0) {
            Write-Host "$failed self-test case(s) FAILED." -ForegroundColor Red
            exit 1
        }
        Write-Host "All $($cases.Count) self-test cases passed." -ForegroundColor Green
    } finally {
        Remove-Item -Path $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    exit 0
}

Write-Host "`n==> Checking working tree" -ForegroundColor Cyan
$originalBranch = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Output
$originalCommit = (Invoke-Git @('rev-parse', 'HEAD')).Output
$wasDetached    = ($originalBranch -eq 'HEAD')
$switchedBranch = $false
# SHA of the entry this run pushed, '' when nothing was stashed. Deliberately not named
# $stashSha: variable names are case-insensitive, so it would shadow Restore-AndExit's
# own $StashSha parameter.
$autostashSha = ''

$status = Invoke-Git @('status', '--porcelain')
if ($Delegated) {
    # Never stash another working copy's changes. The stash stack is shared by every
    # worktree, so an entry pushed here could be popped by a different session working
    # somewhere else. Refuse and name the tree instead.
    $gitDir = (Invoke-Git @('rev-parse', '--path-format=absolute', '--git-dir')).Output
    $busy = @('MERGE_HEAD', 'rebase-merge', 'rebase-apply', 'CHERRY_PICK_HEAD', 'REVERT_HEAD') |
            Where-Object { Test-Path (Join-Path $gitDir $_) }
    # Tracked changes only. A fast-forward never touches untracked files, and git refuses
    # on its own if an incoming commit would overwrite one -- while tool caches and build
    # leftovers sit untracked in that tree permanently, so counting them as "dirty" would
    # block every delegated sync forever.
    $trackedChanges = @($status.Output | Where-Object { $_ -and $_ -notmatch '^\?\?' })
    if ($trackedChanges.Count -gt 0 -or $busy) {
        Write-Host "FAIL: master lives in another worktree, and that tree has uncommitted work:" -ForegroundColor Red
        Write-Host "  $RepoRoot" -ForegroundColor Yellow
        if ($busy) { Write-Host "  operation in progress: $($busy -join ', ')" -ForegroundColor Yellow }
        $trackedChanges | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
        Write-Host "Commit, discard or stash those changes there, then re-run this." -ForegroundColor Yellow
        exit 1
    }
    Write-Host "PASS: master's worktree is clean." -ForegroundColor Green
    Write-Host "  $RepoRoot"
} elseif ($status.Output) {
    Write-Host "Working tree has uncommitted changes -- stashing before sync." -ForegroundColor Yellow
    $stash = Invoke-Git @('stash', 'push', '-u', '-m', 'Sync-Master.ps1 autostash')
    if ($stash.ExitCode -ne 0) {
        Write-Host "FAIL: could not stash uncommitted changes." -ForegroundColor Red
        Write-GitOutput $stash.Output
        Restore-AndExit -Code 1 -OriginalBranch $originalBranch -OriginalCommit $originalCommit -WasDetached $wasDetached -SwitchedBranch $switchedBranch -StashSha $autostashSha
    }
    # Identify the entry now, while it is still on top. Everything after this addresses it
    # by SHA, so a concurrent session pushing its own entry cannot misdirect the restore.
    $pushed = "$((Invoke-Git @('rev-parse', 'stash@{0}')).Output)".Trim()
    if ($pushed -notmatch '^[0-9a-f]{40}$') {
        Write-Host "FAIL: stashed, but could not identify the pushed entry -- refusing to restore blindly." -ForegroundColor Red
        Write-Host "Your changes are in the stash tagged 'Sync-Master.ps1 autostash'; recover them with 'git stash list'." -ForegroundColor Yellow
        exit 1
    }
    $autostashSha = $pushed
    Write-Host "PASS: uncommitted changes stashed ($($autostashSha.Substring(0,7)))." -ForegroundColor Green
} else {
    Write-Host "PASS: working tree is clean." -ForegroundColor Green
}

Write-Host "`n==> Fetching origin/main" -ForegroundColor Cyan
$fetch = Invoke-Git @('fetch', 'origin', 'main')
if ($fetch.ExitCode -ne 0) {
    Write-Host "FAIL: git fetch origin main failed." -ForegroundColor Red
    Write-GitOutput $fetch.Output
    Restore-AndExit -Code 1 -OriginalBranch $originalBranch -OriginalCommit $originalCommit -WasDetached $wasDetached -SwitchedBranch $switchedBranch -StashSha $autostashSha
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
        Restore-AndExit -Code 1 -OriginalBranch $originalBranch -OriginalCommit $originalCommit -WasDetached $wasDetached -SwitchedBranch $switchedBranch -StashSha $autostashSha
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
        Restore-AndExit -Code 1 -OriginalBranch $originalBranch -OriginalCommit $originalCommit -WasDetached $wasDetached -SwitchedBranch $switchedBranch -StashSha $autostashSha
    }
    $mergeSha = (Invoke-Git @('rev-parse', '--short', 'HEAD')).Output
    Write-Host "PASS: merged origin/main into master (new commit $mergeSha); local-only commits preserved." -ForegroundColor Green
}

Write-Host "`nSync complete." -ForegroundColor Green
Restore-AndExit -Code 0 -OriginalBranch $originalBranch -OriginalCommit $originalCommit -WasDetached $wasDetached -SwitchedBranch $switchedBranch -StashSha $autostashSha
