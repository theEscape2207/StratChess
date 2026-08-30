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
       detects that case and refuses with instructions, rather than half-doing it. An
       agent session permanently pinned inside that directory can pass `-FromInside` to
       proceed anyway: every git call below already targets the main checkout via `-C`,
       never cwd, so this is safe.
    2. **Merge verification, not name-trust.** Before deleting anything it checks the
       branch is actually an ancestor of `origin/main` (`git merge-base --is-ancestor`).
       A branch whose commits were squash-merged is NOT an ancestor, so that case is
       reported explicitly with a content-diff hint rather than silently blocked or
       silently deleted.
    3. **Locked worktrees** are unlocked first, otherwise removal fails with a message
       that does not name the lock as the cause.
    4. **Detached worktrees with a sibling branch.** Claude Code's auto-mode worktrees are
       detached, with a separately-created `claude/<dir-name>` branch parked at the same
       commit. Removing the directory alone leaves that branch behind, so they pile up
       unnoticed. This script finds it and deletes both -- but only when the branch both
       points at the worktree's HEAD and is named after the worktree, so an unrelated
       branch that merely shares the commit is never touched.

    Deletes the remote branch only if it exists. Optionally syncs `master` afterwards.

.PARAMETER Name
    Worktree directory name under `.claude/worktrees/`, e.g. 'eval-mobility-term'.

.PARAMETER Branch
    Explicitly name the branch to delete. Needed only when the worktree is detached and
    the sibling-branch match is ambiguous (or named unconventionally).

.PARAMETER Force
    Delete even if the branch is not merged into origin/main. Also passes --force to
    `git worktree remove` (discarding uncommitted changes in that worktree).

.PARAMETER KeepRemote
    Leave the remote branch alone.

.PARAMETER SyncMaster
    Run Sync-Master.ps1 afterwards so local master reflects the merge.

.PARAMETER FromInside
    Acknowledge that the caller's shell is permanently pinned inside the worktree
    being removed (an agent session, not a human terminal) and cannot cd to the
    main checkout. Every git call in this script already targets the main checkout
    via -C, so this is safe -- only the final directory deletion is skipped, same
    as the existing Trap 5 handling for a locked directory.

.WHEN TO USE
    After a PR merges. Equivalent to the `commit-commands:clean_gone` skill for a single
    known worktree.

.HOW TO INVOKE (from bash, cmd, or PowerShell) -- run from the MAIN checkout
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Remove-Worktree.ps1 -Name eval-mobility-term -SyncMaster

    An agent session whose shell is pinned inside the worktree being removed and cannot
    cd elsewhere should add -FromInside.

.NOTES
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$Branch,
    [switch]$Force,
    [switch]$KeepRemote,
    [switch]$SyncMaster,
    [switch]$FromInside
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Must be initialised: StrictMode makes reading an undefined variable a terminating error.
$script:DirLeftBehind = $false

$commonDir = & git rev-parse --path-format=absolute --git-common-dir 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: not inside a git repository." -ForegroundColor Red; exit 1 }
$MainCheckout = Split-Path $commonDir -Parent
$TargetPath   = Join-Path (Join-Path $MainCheckout '.claude\worktrees') $Name

# Trap 1: refuse to remove the worktree we are standing in, unless the caller has
# confirmed via -FromInside that it cannot cd elsewhere (an agent session pinned to
# this directory for its lifetime). Safe to allow: every git call below targets
# $MainCheckout via -C, never cwd, and Trap 5 already tolerates the directory itself
# surviving the removal.
$here = (Get-Location).Path
$insideTarget = $here -eq $TargetPath -or $here.StartsWith($TargetPath + [IO.Path]::DirectorySeparatorChar)
if ($insideTarget -and -not $FromInside) {
    Write-Host "FAIL: you are inside the worktree being removed." -ForegroundColor Red
    Write-Host "      git cannot delete its own working directory -- it would deregister the" -ForegroundColor Yellow
    Write-Host "      worktree and leave an orphaned folder behind." -ForegroundColor Yellow
    Write-Host "      Run this from the main checkout instead:" -ForegroundColor Yellow
    Write-Host "        cd `"$MainCheckout`"" -ForegroundColor Yellow
    Write-Host "      Or, if this is an agent session permanently pinned to this directory" -ForegroundColor Yellow
    Write-Host "      and cannot cd, re-run with -FromInside -- every git call below already" -ForegroundColor Yellow
    Write-Host "      targets the main checkout explicitly, so this is safe." -ForegroundColor Yellow
    exit 1
}
if ($insideTarget) {
    Write-Host "NOTE: running from inside the worktree being removed (-FromInside)." -ForegroundColor Yellow
    Write-Host "      The directory itself cannot be deleted until this session exits;" -ForegroundColor Yellow
    Write-Host "      branch cleanup below is unaffected." -ForegroundColor Yellow
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

if (-not $branch -and $Branch) {
    # Explicit override always wins.
    $branch = $Branch
    Write-Host "`n==> Worktree '$Name' -> branch '$branch' (from -Branch)" -ForegroundColor Cyan
}
elseif (-not $branch) {
    # Trap 4: a DETACHED worktree with a sibling branch at the same commit.
    #
    # Claude Code's auto-mode worktrees land in this shape: the worktree is detached,
    # while a separately-created branch (conventionally `claude/<dir-name>`) sits at the
    # same commit. Removing the directory alone silently leaves that branch behind, so
    # they accumulate invisibly -- exactly how code-terminal-auto-mode-2fd492 survived
    # unnoticed for a week.
    #
    # Matched conservatively: the branch must point at this worktree's HEAD *and* be
    # named either <Name> or <something>/<Name>. A branch that merely happens to share
    # the commit (master, main, or an unrelated branch sitting at the same merge) is
    # never picked up -- being at the same commit is not evidence of being related.
    $headSha = & git -C $TargetPath rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $headSha) {
        $candidates = @()
        foreach ($b in (& git -C $MainCheckout branch --format='%(refname:short)' --points-at $headSha)) {
            $b = $b.Trim()
            if (-not $b -or $b -eq 'master' -or $b -eq 'main') { continue }
            if ($b -eq $Name -or $b -like "*/$Name") { $candidates += $b }
        }

        if ($candidates.Count -eq 1) {
            $branch = $candidates[0]
            Write-Host "`n==> Worktree '$Name' is detached; found sibling branch '$branch' at the same commit." -ForegroundColor Cyan
        }
        elseif ($candidates.Count -gt 1) {
            Write-Host "`nNOTE: detached worktree with several matching branches -- not guessing." -ForegroundColor Yellow
            foreach ($c in $candidates) { Write-Host "        $c" -ForegroundColor Yellow }
            Write-Host "      Re-run with -Branch <name> to delete one of them." -ForegroundColor Yellow
        }
        else {
            Write-Host "`nNOTE: detached worktree, no matching branch -- removing the directory only." -ForegroundColor Yellow
        }
    }
}
else {
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
        # Trap 5: on Windows, git routinely deletes every file but cannot rmdir the
        # folder itself when any process holds a handle on it (a shell sitting in it,
        # Visual Studio, an indexer). That is a cosmetic leftover, NOT a reason to stop:
        # the branch cleanup below is the part that actually matters, and bailing here
        # is how orphaned branches accumulate. Deregister and carry on.
        Write-Host "WARNING: could not delete the directory (a process is holding it open)." -ForegroundColor Yellow
        Write-Host "         Deregistering it and continuing with branch cleanup." -ForegroundColor Yellow
        $script:DirLeftBehind = $true
    } else {
        Write-Host "PASS: worktree removed." -ForegroundColor Green
    }
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
    # $PSScriptRoot is unsafe here: when the executing script is the removed worktree's
    # own copy, Trap 3 above may have just deleted this directory's contents (including
    # this script's sibling Sync-Master.ps1). $MainCheckout's copy always survives.
    & pwsh -ExecutionPolicy Bypass -File (Join-Path $MainCheckout 'Scripts\Sync-Master.ps1')
}

if ($script:DirLeftBehind) {
    Write-Host "`nCleanup complete, except the (now empty, deregistered) directory:" -ForegroundColor Yellow
    Write-Host "  $TargetPath" -ForegroundColor Yellow
    Write-Host "It is harmless -- delete it once whatever is holding it open has exited." -ForegroundColor Yellow
} else {
    Write-Host "`nCleanup complete." -ForegroundColor Green
}
