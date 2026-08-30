<#
.SYNOPSIS
    Start a task on a fresh branch in the CURRENT worktree.

.DESCRIPTION
    The in-place counterpart to New-Worktree.ps1, for running several small tasks in
    sequence out of one worktree instead of creating a directory per task. It removes the
    same traps that script removes, minus the ones about directories:

    1. **Forking from the wrong base.** A hand-typed `git checkout -b` off the previous
       task's branch drags that task's commits into the next PR. This always forks from
       `origin/main` and never reads the current branch.
    2. **Forgetting to fetch first** silently forks from a stale `origin/main`, which
       surfaces as a merge conflict at PR time instead of now.
    3. **Carrying uncommitted work across tasks.** `git checkout` moves a dirty tree onto
       the new branch whenever it can do so without conflict, which is how one task's
       edits end up in another task's commit. Separate worktrees make that impossible;
       here it takes a check.

    Untracked files do NOT block: they survive a checkout unchanged, cannot enter a commit
    on their own, and tool caches and build leftovers would otherwise trip every run.

.PARAMETER Name
    Short kebab-case task name, e.g. 'eval-mobility-term'. Becomes the branch name,
    prefixed.

.PARAMETER BranchName
    Override the derived `worktree-<Name>` branch name.

.WHEN TO USE
    Starting a task when you intend to stay in this worktree. Use New-Worktree.ps1 instead
    when the task must run alongside another one, or when you need to park it half-finished
    and come back -- this mode is sequential, since one worktree holds one branch at a time.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\New-TaskBranch.ps1 -Name eval-mobility-term

.NOTES
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$BranchName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Name -notmatch '^[a-z0-9]+(-[a-z0-9]+)*$') {
    Write-Host "FAIL: -Name must be kebab-case (lowercase letters, digits, single hyphens): '$Name'" -ForegroundColor Red
    Write-Host "      e.g. -Name eval-mobility-term" -ForegroundColor Yellow
    exit 1
}

if (-not $BranchName) { $BranchName = "worktree-$Name" }

& git rev-parse --git-dir *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: not inside a git repository." -ForegroundColor Red
    exit 1
}

# Tracked changes only -- porcelain reports untracked entries as '??', and those are
# deliberately allowed through (see .DESCRIPTION).
$tracked = @(& git status --porcelain | Where-Object { $_ -and $_ -notmatch '^\?\?' })
if ($tracked.Count -gt 0) {
    Write-Host "`nFAIL: working tree has uncommitted changes to tracked files." -ForegroundColor Red
    Write-Host "      Switching branches would carry them into the new task." -ForegroundColor Yellow
    $tracked | ForEach-Object { Write-Host "        $_" -ForegroundColor Yellow }
    Write-Host "      Commit them, or discard them, then re-run." -ForegroundColor Yellow
    exit 1
}

$currentBranch = (& git rev-parse --abbrev-ref HEAD).Trim()

Write-Host "`n==> Fetching origin/main" -ForegroundColor Cyan
& git fetch origin main
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: git fetch origin main failed." -ForegroundColor Red
    exit 1
}
$base = (& git rev-parse --short origin/main).Trim()
Write-Host "PASS: origin/main is at $base." -ForegroundColor Green

# Leaving commits behind is legitimate -- an open PR is the usual reason -- so this warns
# rather than blocks. It exists because in-place mode leaves no directory sitting there to
# remind you that the previous task was never finished.
$aheadRaw = (& git rev-list --count origin/main..HEAD 2>$null)
$ahead = 0
if ($LASTEXITCODE -eq 0 -and $aheadRaw) { $ahead = [int]$aheadRaw }
if ($ahead -gt 0) {
    Write-Host "`nNOTE: '$currentBranch' has $ahead commit(s) not in origin/main." -ForegroundColor Yellow
    Write-Host "      They stay on that branch; make sure they are pushed or PR'd." -ForegroundColor Yellow
}

& git show-ref --verify --quiet "refs/heads/$BranchName"
if ($LASTEXITCODE -eq 0) {
    Write-Host "`nFAIL: branch '$BranchName' already exists." -ForegroundColor Red
    Write-Host "      Pick another -Name, or delete it: git branch -D $BranchName" -ForegroundColor Yellow
    exit 1
}

Write-Host "`n==> Creating branch" -ForegroundColor Cyan
& git checkout -b $BranchName origin/main
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: git checkout -b failed." -ForegroundColor Red
    exit 1
}

Write-Host "`nPASS: on '$BranchName', forked from origin/main ($base)." -ForegroundColor Green
Write-Host "`nNext:" -ForegroundColor Cyan
Write-Host "  .\build.ps1 all"
Write-Host "  ... work ..."
Write-Host "  .\Scripts\New-PullRequest.ps1 -Title `"...`""
