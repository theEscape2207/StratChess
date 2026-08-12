<#
.SYNOPSIS
    Show every worktree with its drift against `origin/main` and its PR state.

.DESCRIPTION
    A worktree can sit idle and silently fall behind `main` indefinitely; the first
    symptom is usually a conflict discovered at PR review (see PR #57, where null-move
    pruning landed on main mid-session and collided with Board.h). CLAUDE.md's remedy is
    to check before resuming work in an idle worktree -- this is that check, for all of
    them at once.

    Per worktree it reports: the branch, how many commits it is ahead of / behind
    `origin/main`, whether the working tree is dirty, and -- if `gh` is available -- the
    number and state of any PR for that branch. Merged-but-not-cleaned-up worktrees are
    called out explicitly, since those are the ones that accumulate.

    Finally it scans `.claude\worktrees` directly for directories that are absent from
    `git worktree list`. Every other cleanup path enumerates from that list, so none of
    them can see a directory git has forgotten -- which is how ~2.2 GB of stale build
    output accumulated unnoticed (issue #149).

    Read-only: fetches `origin/main` but changes nothing else.

.WHEN TO USE
    - Start of a session, to see what is outstanding.
    - Before resuming work in a worktree that has been idle.
    - After a worktree removal that reported a locked directory.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\StratChessEvolved\Scripts\Get-Worktrees.ps1

.NOTES
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
#>

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonDir = & git rev-parse --path-format=absolute --git-common-dir 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: not inside a git repository." -ForegroundColor Red; exit 1 }
$MainCheckout = Split-Path $commonDir -Parent

Write-Host "`n==> Fetching origin/main" -ForegroundColor Cyan
& git -C $MainCheckout fetch origin main | Out-Null
$mainSha = & git -C $MainCheckout rev-parse --short origin/main
Write-Host "origin/main is at $mainSha`n" -ForegroundColor Green

$ghOk = $true
& gh auth status 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { $ghOk = $false }

# Parse `worktree list --porcelain` into (path, branch) pairs.
$entries = @()
$path = $null; $branch = $null; $detached = $false
foreach ($line in (& git -C $MainCheckout worktree list --porcelain)) {
    if ($line -like 'worktree *') { $path = $line.Substring(9); $branch = $null; $detached = $false }
    elseif ($line -like 'branch *')  { $branch = $line.Substring(7) -replace '^refs/heads/', '' }
    elseif ($line -eq 'detached')    { $detached = $true }
    elseif ($line -eq '') {
        if ($path) { $entries += [pscustomobject]@{ Path = $path; Branch = $branch; Detached = $detached } }
        $path = $null
    }
}
if ($path) { $entries += [pscustomobject]@{ Path = $path; Branch = $branch; Detached = $detached } }

foreach ($e in $entries) {
    $label = Split-Path $e.Path -Leaf
    $name  = if ($e.Detached) { "(detached HEAD)" } else { $e.Branch }

    Write-Host ("-" * 78)
    Write-Host ("{0}" -f $label) -ForegroundColor Cyan
    Write-Host ("  branch : {0}" -f $name)

    $ref = if ($e.Detached) { (& git -C $e.Path rev-parse HEAD) } else { $e.Branch }

    $counts = & git -C $MainCheckout rev-list --left-right --count "origin/main...$ref" 2>$null
    if ($LASTEXITCODE -eq 0 -and $counts) {
        $parts  = $counts -split '\s+'
        $behind = [int]$parts[0]
        $ahead  = [int]$parts[1]

        $driftColor = if ($behind -gt 0) { 'Yellow' } else { 'Green' }
        Write-Host ("  drift  : {0} ahead, {1} behind origin/main" -f $ahead, $behind) -ForegroundColor $driftColor

        if ($ahead -eq 0 -and $behind -gt 0 -and -not $e.Detached -and $e.Branch -ne 'master') {
            Write-Host "  status : fully merged -- safe to remove" -ForegroundColor Green
            Write-Host ("           Remove-Worktree.ps1 -Name {0} -SyncMaster" -f $label) -ForegroundColor DarkGray
        }
        if ($behind -gt 0 -and $ahead -gt 0) {
            Write-Host "  status : BEHIND main -- merge origin/main before doing more work" -ForegroundColor Yellow
        }
    }

    $dirty = & git -C $e.Path status --porcelain 2>$null
    if ($dirty) {
        $n = ($dirty | Measure-Object).Count
        Write-Host ("  tree   : {0} uncommitted change(s)" -f $n) -ForegroundColor Yellow
    } else {
        Write-Host "  tree   : clean"
    }

    if ($ghOk -and -not $e.Detached -and $e.Branch -and $e.Branch -ne 'master') {
        # `.[0] | ...` on an empty array yields the string "null null draft=null"
        # rather than nothing, so guard on length and emit nothing when there is no PR.
        $jq = 'if length > 0 then "\(.[0].number) \(.[0].state) draft=\(.[0].isDraft)" else empty end'
        $pr = & gh pr list --head $e.Branch --state all --json number,state,isDraft --jq $jq 2>$null
        if ($pr) { Write-Host ("  pr     : #{0}" -f $pr) }
        else     { Write-Host "  pr     : none" }
    }
}
Write-Host ("-" * 78)

# Everything above enumerates `git worktree list`, so it structurally cannot report a
# directory git has no record of. On Windows a removal routinely half-succeeds -- git
# deletes the files but cannot rmdir a folder another process holds open -- and older
# removals dropped the registration while leaving a full source tree behind. Both leave
# residue that no cleanup path can see, so scan the folder itself.
$wtRoot = Join-Path $MainCheckout '.claude\worktrees'
if (Test-Path $wtRoot) {
    $registered = @($entries | ForEach-Object { ($_.Path -replace '/', '\').TrimEnd('\') })
    $orphans = @(
        Get-ChildItem -LiteralPath $wtRoot -Directory -Force |
            Where-Object { $registered -notcontains $_.FullName.TrimEnd('\') }
    )

    if ($orphans.Count -eq 0) {
        Write-Host "unregistered: none" -ForegroundColor Green
    } else {
        Write-Host ("UNREGISTERED directories in .claude\worktrees: {0}" -f $orphans.Count) -ForegroundColor Red
        Write-Host '  Absent from `git worktree list` -- invisible to every other cleanup path.' -ForegroundColor DarkGray

        foreach ($o in $orphans) {
            $files = @(Get-ChildItem -LiteralPath $o.FullName -Recurse -File -Force -ErrorAction SilentlyContinue)
            # Under Set-StrictMode, Measure-Object over an empty collection returns an
            # object with no Sum property at all -- so guard on the count, not on $null.
            $mb = 0
            if ($files.Count -gt 0) {
                $mb = [math]::Round((($files | Measure-Object -Property Length -Sum).Sum) / 1MB, 0)
            }

            if ($files.Count -eq 0) {
                Write-Host ("  {0} : empty -- safe to delete outright" -f $o.Name) -ForegroundColor Yellow
            } elseif (Test-Path (Join-Path $o.FullName '.git')) {
                # Has a .git entry but is not in this repo's list: most likely registered
                # against a different repository. Do not offer to delete it.
                Write-Host ("  {0} : {1} MB, has .git -- belongs to another repo? investigate" -f $o.Name, $mb) -ForegroundColor Red
            } else {
                Write-Host ("  {0} : {1} MB in {2} file(s), no .git" -f $o.Name, $mb, $files.Count) -ForegroundColor Red
                Write-Host "           Verify by content before deleting -- NOT with ``git -C``, which" -ForegroundColor DarkGray
                Write-Host "           silently resolves against the outer repo and reports a clean tree." -ForegroundColor DarkGray
            }
        }
        Write-Host ("-" * 78)
    }
}
Write-Host ""
