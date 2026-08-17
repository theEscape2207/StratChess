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

    Read-only by default: fetches `origin/main` but changes nothing else. -Prune is
    the one exception, and it only ever deletes unregistered directories.

.PARAMETER Prune
    Delete the unregistered directories it can prove are safe. Empty ones go
    outright. A populated one is kept unless every file in it is either covered by
    this repository's ignore rules -- build output, .vs, tool downloads, all
    regenerable -- or holds content that is already a blob in the object database,
    i.e. committed somewhere. Anything else is reported per path and the directory
    is kept.

    The safety rule is deliberately NOT "matches origin/main". A copy that is
    simply behind main loses nothing when deleted, and testing against a moving
    tip would refuse it for no reason.

    A directory carrying its own .git is never touched: it most likely belongs to
    another repository, which this script cannot reason about.

.PARAMETER Force
    With -Prune, delete a directory even though it holds unaccounted content.
    Review the reported paths first -- that content exists nowhere else.

.WHEN TO USE
    - Start of a session, to see what is outstanding.
    - Before resuming work in a worktree that has been idle.
    - After a worktree removal that reported a locked directory.
    - With -Prune, to reclaim the residue those removals leave behind. Run it from
      the main checkout: a shell sitting inside a directory holds it open, and
      Windows will not delete it (the same reason Remove-Worktree.ps1 refuses to
      remove the worktree you are standing in).

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\StratChessEvolved\Scripts\Get-Worktrees.ps1

.NOTES
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
#>

[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$Prune,
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commonDir = & git rev-parse --path-format=absolute --git-common-dir 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: not inside a git repository." -ForegroundColor Red; exit 1 }
$MainCheckout = Split-Path $commonDir -Parent

# --- -Prune support -------------------------------------------------------------
#
# A directory is safe to delete when nothing in it would be lost: every file is
# either ignored by this repository's rules -- and therefore regenerable -- or its
# exact content is already a blob in the object database, i.e. committed
# somewhere. Deliberately NOT "matches origin/main": a stale copy that is simply
# behind main is safe, and comparing against a moving tip would refuse it.
#
# The rules come from the main checkout, which is what makes this work on a
# directory that has no .git of its own -- `git -C <orphan>` would resolve
# against the outer repository and report a clean tree for anything.

# Directory-only patterns (`build/`, `.vs/`, `Python/`) match a path git believes
# is a directory. check-ignore infers that from the filesystem it runs in, where
# an orphan's subdirectory does not exist -- so the trailing slash has to be
# supplied explicitly or `.vs` reads as a file and matches nothing.
function Split-IgnoredPaths {
    param(
        [Parameter(Mandatory)] [System.Collections.IEnumerable] $Items,
        [Parameter(Mandatory)] [string] $Root
    )

    $probes = @{}
    foreach ($item in $Items) {
        $rel = $item.FullName.Substring($Root.Length).TrimStart('\') -replace '\\', '/'
        if ($item.PSIsContainer) { $rel += '/' }
        $probes[$rel] = $item
    }
    if ($probes.Count -eq 0) { return @{ IgnoredCount = 0; Kept = @() } }

    # Paths go as ARGUMENTS, never through `--stdin`: PowerShell terminates the
    # lines it writes to a native command with CRLF, git keeps the CR as part of
    # the path, and every probe then silently fails to match -- it C-quotes the
    # result too, so even the echo does not compare equal. Chunked so a directory
    # with many children cannot overflow the command line, which still costs one
    # git process per chunk instead of one per path.
    $ignored = @{}
    $keys = @($probes.Keys)
    for ($i = 0; $i -lt $keys.Count; $i += 100) {
        $chunk = @($keys[$i..([math]::Min($i + 99, $keys.Count - 1))])
        foreach ($line in (& git -C $MainCheckout -c core.quotePath=false check-ignore -- @chunk)) {
            $ignored[$line] = $true
        }
    }

    $keptItems = @()
    foreach ($rel in $keys) {
        if (-not $ignored.ContainsKey($rel)) { $keptItems += $probes[$rel] }
    }
    return @{ IgnoredCount = $ignored.Count; Kept = $keptItems }
}

# Every file under $Root that this repository's ignore rules do not cover.
function Get-UnignoredFiles {
    param([Parameter(Mandatory)] [string] $Root)

    $files = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    $pending = [System.Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($Root)

    while ($pending.Count -gt 0) {
        $dir = $pending.Dequeue()
        $children = @(Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue)
        if ($children.Count -eq 0) { continue }

        $split = Split-IgnoredPaths -Items $children -Root $Root
        foreach ($item in $split.Kept) {
            if ($item.PSIsContainer) { $pending.Enqueue($item.FullName) }
            else                     { $files.Add($item) }
        }
    }
    # Comma operator: `return $files` unrolls the list into the pipeline, so a
    # single-file directory would come back as a bare FileInfo and every .Count
    # on it would fail under Set-StrictMode.
    return ,$files
}

# The files whose content is NOT already in the object database. Hashing and
# lookup are batched: two git processes for the whole directory.
function Get-UnaccountedFiles {
    param([Parameter(Mandatory)] [System.Collections.Generic.List[System.IO.FileInfo]] $Files)

    if ($Files.Count -eq 0) { return @() }

    $shas = @($Files.FullName | & git -C $MainCheckout hash-object --stdin-paths)
    if ($shas.Count -ne $Files.Count) {
        throw "hash-object returned $($shas.Count) hashes for $($Files.Count) files."
    }

    # `<sha> blob <size>` when present, `<sha> missing` when not.
    $probe = @($shas | & git -C $MainCheckout cat-file --batch-check)

    $unaccounted = @()
    for ($i = 0; $i -lt $shas.Count; $i++) {
        if ($probe[$i] -like '* missing') { $unaccounted += $Files[$i] }
    }
    return $unaccounted
}

function Remove-OrphanDirectory {
    param(
        [Parameter(Mandatory)] [System.IO.DirectoryInfo] $Directory,
        [Parameter(Mandatory)] [string] $Reason
    )

    # Windows will not delete a directory any process has open, and the most
    # common such process is this session: a shell whose working directory is
    # inside the target holds it for as long as it lives. Nothing here can fix
    # that from within, so say so plainly instead of failing halfway and leaving
    # a half-deleted tree behind.
    $here = (Get-Location -PSProvider FileSystem).ProviderPath.TrimEnd('\')
    $target = $Directory.FullName.TrimEnd('\')
    if ($here -eq $target -or $here.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host ("           SKIPPED: this session's working directory is inside it ({0})." -f $here) -ForegroundColor Yellow
        Write-Host "           Re-run from the main checkout." -ForegroundColor Yellow
        return
    }

    if (-not $PSCmdlet.ShouldProcess($target, 'Remove directory')) { return }

    try {
        Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
        Write-Host ("           DELETED ({0})." -f $Reason) -ForegroundColor Green
    } catch {
        # Distinct from the case above: some *other* process -- an editor, an
        # indexer, a second shell -- holds a handle. Partial deletion is normal
        # here and the next run picks up whatever survived.
        Write-Host ("           FAILED: {0}" -f $_.Exception.Message) -ForegroundColor Red
        Write-Host "           Another process is holding it open; close it and re-run." -ForegroundColor Yellow
    }
}

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
                if ($Prune) { Remove-OrphanDirectory -Directory $o -Reason 'empty' }
            } elseif (Test-Path (Join-Path $o.FullName '.git')) {
                # Has a .git entry but is not in this repo's list: most likely registered
                # against a different repository. Do not offer to delete it.
                Write-Host ("  {0} : {1} MB, has .git -- belongs to another repo? investigate" -f $o.Name, $mb) -ForegroundColor Red
                if ($Prune) { Write-Host "           SKIPPED by -Prune: never deletes a directory with a .git entry." -ForegroundColor DarkGray }
            } else {
                Write-Host ("  {0} : {1} MB in {2} file(s), no .git" -f $o.Name, $mb, $files.Count) -ForegroundColor Red
                if (-not $Prune) {
                    Write-Host "           Verify by content before deleting -- NOT with ``git -C``, which" -ForegroundColor DarkGray
                    Write-Host "           silently resolves against the outer repo and reports a clean tree." -ForegroundColor DarkGray
                    Write-Host "           -Prune runs that verification and deletes what it proves safe." -ForegroundColor DarkGray
                    continue
                }

                $unignored   = Get-UnignoredFiles -Root $o.FullName
                $unaccounted = @(Get-UnaccountedFiles -Files $unignored)
                Write-Host ("           {0} file(s) not covered by ignore rules; {1} hold content that is not in the object database." -f $unignored.Count, $unaccounted.Count) -ForegroundColor DarkGray

                if ($unaccounted.Count -eq 0) {
                    Remove-OrphanDirectory -Directory $o -Reason 'every file is ignored or already committed'
                } else {
                    foreach ($u in ($unaccounted | Select-Object -First 10)) {
                        Write-Host ("             unaccounted: {0}" -f $u.FullName.Substring($o.FullName.Length).TrimStart('\')) -ForegroundColor Yellow
                    }
                    if ($unaccounted.Count -gt 10) {
                        Write-Host ("             ... and {0} more" -f ($unaccounted.Count - 10)) -ForegroundColor Yellow
                    }
                    if ($Force) {
                        Remove-OrphanDirectory -Directory $o -Reason '-Force, despite unaccounted content'
                    } else {
                        Write-Host "           KEPT: review the paths above, then re-run with -Force to delete anyway." -ForegroundColor Red
                    }
                }
            }
        }
        Write-Host ("-" * 78)
    }
}
Write-Host ""
