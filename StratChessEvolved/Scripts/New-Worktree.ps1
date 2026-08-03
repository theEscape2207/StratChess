<#
.SYNOPSIS
    Create a fresh worktree + branch off `origin/main`.

.DESCRIPTION
    The project's unit of work is a per-task worktree forked from `origin/main`, living
    under `<main checkout>/.claude/worktrees/<name>`. Doing that by hand has three traps
    this script removes:

    1. **Wrong location breaks the tooling.** The build itself no longer cares -- CMake
       fetches its own dependencies -- but `Run-EloMatch.ps1` finds `EngineTesting\`
       (fastchess and the cached reference binaries) beside the *main* checkout, and a
       worktree planted somewhere else resolves it to the wrong place. This script always
       resolves the main checkout via `git rev-parse --git-common-dir`, so it plants the
       worktree correctly even when invoked from inside another worktree.
    2. **Forgetting to fetch first** silently forks from a stale `origin/main`, which
       surfaces as a merge conflict at PR time instead of now.
    3. **Forking from `master`.** `master` is a personal scratch branch carrying local
       merge commits; branching from it drags them into the eventual PR. This script
       always uses `origin/main` and never reads the current branch.

    Creates the branch as `worktree-<name>` (matching existing convention) unless
    -BranchName overrides it. Refuses rather than clobbers if the path or branch already
    exists.

.PARAMETER Name
    Short kebab-case task name, e.g. 'eval-mobility-term'. Becomes both the directory
    name and (prefixed) the branch name.

.PARAMETER BranchName
    Override the derived `worktree-<Name>` branch name.

.WHEN TO USE
    Starting any new task that will produce a PR.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\New-Worktree.ps1 -Name eval-mobility-term"

.NOTES
    Safe to run from the main checkout or from inside any existing worktree.
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
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

# Resolve the MAIN checkout, not whatever worktree we happen to be in. --git-common-dir
# points at the shared .git directory (the main checkout's) from any worktree; its parent
# is the main working tree, which is what EngineTesting\ is resolved against.
$commonDir = & git rev-parse --path-format=absolute --git-common-dir 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: not inside a git repository." -ForegroundColor Red
    exit 1
}
$MainCheckout = Split-Path $commonDir -Parent
$WorktreeRoot = Join-Path $MainCheckout '.claude\worktrees'
$TargetPath   = Join-Path $WorktreeRoot $Name

Write-Host "`n==> Target" -ForegroundColor Cyan
Write-Host "  main checkout : $MainCheckout"
Write-Host "  worktree path : $TargetPath"
Write-Host "  branch        : $BranchName"

if (Test-Path $TargetPath) {
    Write-Host "`nFAIL: path already exists: $TargetPath" -ForegroundColor Red
    Write-Host "      Pick another -Name, or clean it up with Remove-Worktree.ps1." -ForegroundColor Yellow
    exit 1
}

& git -C $MainCheckout show-ref --verify --quiet "refs/heads/$BranchName"
if ($LASTEXITCODE -eq 0) {
    Write-Host "`nFAIL: branch '$BranchName' already exists." -ForegroundColor Red
    Write-Host "      Pick another -Name, or delete it: git branch -D $BranchName" -ForegroundColor Yellow
    exit 1
}

Write-Host "`n==> Fetching origin/main" -ForegroundColor Cyan
& git -C $MainCheckout fetch origin main
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: git fetch origin main failed." -ForegroundColor Red
    exit 1
}
$base = (& git -C $MainCheckout rev-parse --short origin/main)
Write-Host "PASS: origin/main is at $base." -ForegroundColor Green

Write-Host "`n==> Creating worktree" -ForegroundColor Cyan
& git -C $MainCheckout worktree add -b $BranchName $TargetPath origin/main
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: git worktree add failed." -ForegroundColor Red
    exit 1
}

Write-Host "`nPASS: worktree ready." -ForegroundColor Green
Write-Host "`nNext:" -ForegroundColor Cyan
Write-Host "  cd `"$TargetPath`""
Write-Host "  .\build.ps1 all                 # also installs the pre-commit hook on first run"
Write-Host "  ... work ..."
Write-Host "  .\StratChessEvolved\Scripts\New-PullRequest.ps1 -Title `"...`""
