<#
.SYNOPSIS
    Tear down a merged worktree: directory, local branch, and remote branch.

.DESCRIPTION
    Post-merge cleanup, which CLAUDE.md treats as part of finishing a task rather than an
    optional extra. Handles the traps that make doing it by hand unpleasant:
    Resolves the target against Git's worktree registry, accepting a registered checkout
    in either Claude's or Codex's directory layout. An unregistered directory is accepted
    by -Name only under .claude/worktrees. A name with no registered worktree or Claude
    directory is refused, even with -Branch; use Remove-MergedBranches.ps1 to clean up
    a branch after its worktree and directory are both gone.

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
    Worktree directory name under .claude/worktrees, or the Codex-managed parent
    directory name above the repository checkout. Ambiguous names require -Path.

.PARAMETER Path
    Exact registered worktree path. Use when the directory layout is unfamiliar or
    several worktrees have the same name. The main checkout is refused. A checkout
    outside both known layouts has no inferred sibling branch; pass -Branch to
    delete one.

.PARAMETER Branch
    Explicitly name the branch to delete for a detached worktree. A branch different
    from the checked-out branch is refused before cleanup. For an unregistered
    Claude directory, pass -Branch if its former branch also needs deletion.

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

.PARAMETER SelfTest
    Run resolver cases and local Git fixture cleanups, then exit.

.WHEN TO USE
    After a PR merges. Equivalent to the `commit-commands:clean_gone` skill for a single
    known worktree.

.HOW TO INVOKE (from bash, cmd, or PowerShell) -- use the script in this repository
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Remove-Worktree.ps1 -Name eval-mobility-term -SyncMaster
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Remove-Worktree.ps1 -Path C:\...\.codex\worktrees\foo\StratChessEvolved -SyncMaster

    An agent session whose shell is pinned inside the worktree being removed and cannot
    cd elsewhere should add -FromInside.

.NOTES
    The script's own checkout identifies the repository; the caller's working directory
    only matters for the inside-target guard.
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>

[CmdletBinding(DefaultParameterSetName = 'ByName')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'ByName')]
    [string]$Name,

    [Parameter(Mandatory = $true, ParameterSetName = 'ByPath')]
    [string]$Path,

    [string]$Branch,
    [switch]$Force,
    [switch]$KeepRemote,
    [switch]$SyncMaster,
    [switch]$FromInside,
    [Parameter(Mandatory = $true, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Normalize-WorktreePath {
    param([Parameter(Mandatory)][string]$Value)
    return [IO.Path]::GetFullPath($Value).TrimEnd([char[]]@('\', '/')).Replace('/', '\')
}

function Resolve-WorktreeTarget {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Entries,
        [Parameter(Mandatory)][string]$Main,
        [string]$RequestedName,
        [string]$RequestedPath,
        [string]$RequestedBranch
    )

    $mainPath = Normalize-WorktreePath $Main
    $claudeRoot = Normalize-WorktreePath (Join-Path $mainPath '.claude\worktrees')
    $repoName = Split-Path $mainPath -Leaf
    $matchingEntries = @()

    if ($RequestedPath) {
        $exactPath = Normalize-WorktreePath $RequestedPath
        if ($exactPath -ieq $mainPath) {
            return [pscustomobject]@{ Error = 'The main checkout cannot be removed.'; Candidates = @() }
        }
        $matchingEntries = @($Entries | Where-Object { (Normalize-WorktreePath $_.Path) -ieq $exactPath })
        if ($matchingEntries.Count -eq 0) {
            return [pscustomobject]@{ Error = "Path is not a registered worktree of this repository: $exactPath"; Candidates = @() }
        }
    } else {
        if ($RequestedName -in @('.', '..') -or
            $RequestedName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
            return [pscustomobject]@{ Error = 'Name must be one directory name.'; Candidates = @() }
        }
        foreach ($entry in $Entries) {
            $entryPath = Normalize-WorktreePath $entry.Path
            if ($entryPath -ieq $mainPath) { continue }
            $parentPath = Split-Path $entryPath -Parent
            $leaf = Split-Path $entryPath -Leaf
            $claudeMatch = $parentPath -ieq $claudeRoot -and $leaf -ieq $RequestedName
            $codexMatch = $leaf -ieq $repoName -and (Split-Path $parentPath -Leaf) -ieq $RequestedName
            if ($claudeMatch -or $codexMatch) { $matchingEntries += $entry }
        }
        if ($matchingEntries.Count -gt 1) {
            return [pscustomobject]@{ Error = "Several worktrees match '$RequestedName'; use -Path."; Candidates = @($matchingEntries.Path) }
        }
        if ($matchingEntries.Count -eq 0) {
            $orphanPath = Join-Path $claudeRoot $RequestedName
            if (-not (Test-Path -LiteralPath $orphanPath)) {
                return [pscustomobject]@{ Error = "No worktree matches '$RequestedName'."; Candidates = @($Entries.Path) }
            }
            $matchingEntries = @([pscustomobject]@{ Path = $orphanPath; Branch = $null; Detached = $true; Registered = $false })
        }
    }

    $target = $matchingEntries[0]
    $selectedPath = Normalize-WorktreePath $target.Path
    $targetParent = Split-Path $selectedPath -Parent
    $targetLeaf = Split-Path $selectedPath -Leaf
    $targetName = if ($targetParent -ieq $claudeRoot) {
        $targetLeaf
    } elseif ($targetLeaf -ieq $repoName) {
        Split-Path $targetParent -Leaf
    } else {
        $null
    }
    $checkedOutBranch = $target.Branch
    if ($RequestedBranch -and $checkedOutBranch -and $RequestedBranch -cne $checkedOutBranch) {
        return [pscustomobject]@{ Error = "Branch '$RequestedBranch' conflicts with checked-out branch '$checkedOutBranch'."; Candidates = @() }
    }
    $resolvedBranch = if ($checkedOutBranch) { $checkedOutBranch } else { $RequestedBranch }
    $registeredTarget = $true
    if ($target.PSObject.Properties['Registered']) { $registeredTarget = $target.Registered }
    return [pscustomobject]@{
        Error = $null
        Path = $selectedPath
        Name = $targetName
        Branch = $resolvedBranch
        Registered = $registeredTarget
        Candidates = @()
    }
}

function Read-WorktreeRegistry {
    param([Parameter(Mandatory)][string]$Main)
    $entries = @()
    $entryPath = $null
    $entryBranch = $null
    $entryDetached = $false
    foreach ($line in (& git -C $Main worktree list --porcelain)) {
        if ($line -like 'worktree *') {
            if ($entryPath) { $entries += [pscustomobject]@{ Path = $entryPath; Branch = $entryBranch; Detached = $entryDetached } }
            $entryPath = $line.Substring(9)
            $entryBranch = $null
            $entryDetached = $false
        } elseif ($line -like 'branch *') {
            $entryBranch = $line.Substring(7) -replace '^refs/heads/', ''
        } elseif ($line -eq 'detached') {
            $entryDetached = $true
        }
    }
    if ($entryPath) { $entries += [pscustomobject]@{ Path = $entryPath; Branch = $entryBranch; Detached = $entryDetached } }
    return $entries
}

# Must be initialised: StrictMode makes reading an undefined variable a terminating error.
$script:DirLeftBehind = $false

if ($SelfTest) {
    $testRoot = Join-Path ([IO.Path]::GetTempPath()) ("remove-worktree-test-" + [guid]::NewGuid().ToString('N'))
    $testRoot = [IO.Path]::GetFullPath($testRoot)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $testRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Self-test root is outside the temp directory: $testRoot"
    }
    try {
        $syntheticMain = Join-Path $testRoot 'synthetic/repo'
        $claudePath = Join-Path $syntheticMain '.claude/worktrees/foo'
        $codexPath = Join-Path $testRoot 'managed/foo/repo'
        $mainEntry = [pscustomobject]@{ Path = $syntheticMain; Branch = 'main'; Detached = $false }
        $claudeEntry = [pscustomobject]@{ Path = $claudePath; Branch = 'claude/foo'; Detached = $false }
        $codexEntry = [pscustomobject]@{ Path = $codexPath; Branch = 'codex/foo'; Detached = $false }
        $detachedEntry = [pscustomobject]@{ Path = $codexPath; Branch = $null; Detached = $true }
        $cases = @(
            @{ Label = 'Claude name'; Entries = @($mainEntry, $claudeEntry); Name = 'foo'; Path = $null; Branch = $null; Expected = $claudePath; ExpectedBranch = 'claude/foo'; Error = $false }
            @{ Label = 'Codex name'; Entries = @($mainEntry, $codexEntry); Name = 'foo'; Path = $null; Branch = $null; Expected = $codexPath; ExpectedBranch = 'codex/foo'; Error = $false }
            @{ Label = 'ambiguous name'; Entries = @($mainEntry, $claudeEntry, $codexEntry); Name = 'foo'; Path = $null; Branch = $null; Expected = $null; Error = $true }
            @{ Label = 'path case and separators'; Entries = @($mainEntry, $codexEntry); Name = $null; Path = $codexPath.ToUpperInvariant().Replace('\', '/'); Branch = $null; Expected = $codexPath; ExpectedBranch = 'codex/foo'; Error = $false }
            @{ Label = 'main checkout'; Entries = @($mainEntry); Name = $null; Path = $syntheticMain; Branch = $null; Expected = $null; Error = $true }
            @{ Label = 'unregistered path'; Entries = @($mainEntry); Name = $null; Path = $codexPath; Branch = $null; Expected = $null; Error = $true }
            @{ Label = 'unknown name'; Entries = @($mainEntry); Name = 'missing'; Path = $null; Branch = $null; Expected = $null; Error = $true }
            @{ Label = 'detached override'; Entries = @($mainEntry, $detachedEntry); Name = 'foo'; Path = $null; Branch = 'codex/foo'; Expected = $codexPath; ExpectedBranch = 'codex/foo'; Error = $false }
            @{ Label = 'conflicting override'; Entries = @($mainEntry, $codexEntry); Name = 'foo'; Path = $null; Branch = 'other'; Expected = $null; Error = $true }
            @{ Label = 'branch case conflict'; Entries = @($mainEntry, $codexEntry); Name = 'foo'; Path = $null; Branch = 'Codex/foo'; Expected = $null; Error = $true }
            @{ Label = 'name traversal'; Entries = @($mainEntry); Name = '..'; Path = $null; Branch = $null; Expected = $null; Error = $true }
        )
        foreach ($case in $cases) {
            $actual = Resolve-WorktreeTarget -Entries $case.Entries -Main $syntheticMain -RequestedName $case.Name -RequestedPath $case.Path -RequestedBranch $case.Branch
            if ([bool]$actual.Error -ne $case.Error) { throw "Resolver case '$($case.Label)' returned: $($actual.Error)" }
            if (-not $case.Error -and (Normalize-WorktreePath $actual.Path) -ine (Normalize-WorktreePath $case.Expected)) {
                throw "Resolver case '$($case.Label)' chose $($actual.Path), expected $($case.Expected)"
            }
            if (-not $case.Error -and $actual.Branch -cne $case.ExpectedBranch) {
                throw "Resolver case '$($case.Label)' chose branch '$($actual.Branch)', expected '$($case.ExpectedBranch)'."
            }
        }

        foreach ($layout in @('Codex', 'Claude')) {
            $fixtureRoot = Join-Path $testRoot $layout
            $fixtureMain = Join-Path $fixtureRoot 'repo'
            $originPath = Join-Path $fixtureRoot 'origin.git'
            $worktreePath = if ($layout -eq 'Codex') {
                Join-Path $fixtureRoot 'managed/foo/repo'
            } else {
                Join-Path $fixtureMain '.claude/worktrees/foo'
            }
            $null = New-Item -ItemType Directory -Path $fixtureRoot -Force
            & git init --bare $originPath | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Could not create $layout bare origin." }
            & git init -b main $fixtureMain | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Could not create $layout main checkout." }
            $otherRepo = Join-Path $fixtureRoot 'other'
            & git init -b main $otherRepo | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Could not create $layout other repository." }
            $otherNested = Join-Path $otherRepo 'nested'
            $null = New-Item -ItemType Directory -Path $otherNested -Force
            $null = New-Item -ItemType Directory -Path (Join-Path $fixtureMain 'Scripts') -Force
            $fixtureScript = Join-Path $fixtureMain 'Scripts/Remove-Worktree.ps1'
            Copy-Item -LiteralPath $PSCommandPath -Destination $fixtureScript
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Sync-Master.ps1') -Destination (Join-Path $fixtureMain 'Scripts/Sync-Master.ps1')
            & git -C $fixtureMain config user.name 'Fixture User' | Out-Null
            & git -C $fixtureMain config user.email 'fixture@example.invalid' | Out-Null
            Set-Content -LiteralPath (Join-Path $fixtureMain 'fixture.txt') -Value 'initial'
            & git -C $fixtureMain add fixture.txt Scripts/Remove-Worktree.ps1 Scripts/Sync-Master.ps1 | Out-Null
            & git -C $fixtureMain commit -m initial | Out-Null
            & git -C $fixtureMain branch master | Out-Null
            & git -C $fixtureMain remote add origin $originPath | Out-Null
            & git -C $fixtureMain push -u origin main | Out-Null
            Push-Location $otherNested
            try {
                $otherResult = @(& pwsh -ExecutionPolicy Bypass -File $fixtureScript -Path $otherNested 2>&1)
                if ($LASTEXITCODE -eq 0 -or ($otherResult -join ' ') -notlike '*belongs to another repository*') {
                    throw "$layout other-repository path was not diagnosed."
                }
            } finally {
                Pop-Location
            }
            & git -C $fixtureMain worktree add -b codex/foo $worktreePath main | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Could not create $layout worktree." }
            Set-Content -LiteralPath (Join-Path $worktreePath 'fixture.txt') -Value 'merged'
            & git -C $worktreePath add fixture.txt | Out-Null
            & git -C $worktreePath commit -m merged | Out-Null
            Push-Location $fixtureMain
            try {
                & pwsh -ExecutionPolicy Bypass -File $fixtureScript -Name foo -Branch other 2>&1 | Out-Null
                if ($LASTEXITCODE -eq 0) { throw "$layout conflicting branch was accepted." }
                & pwsh -ExecutionPolicy Bypass -File $fixtureScript -Name foo 2>&1 | Out-Null
                if ($LASTEXITCODE -eq 0) { throw "$layout unmerged worktree was removed." }
            } finally {
                Pop-Location
            }
            if (-not (Test-Path -LiteralPath $worktreePath)) { throw "$layout unmerged checkout vanished." }
            & git -C $fixtureMain merge --ff-only codex/foo | Out-Null
            & git -C $fixtureMain push origin main codex/foo | Out-Null
            Set-Content -LiteralPath (Join-Path $worktreePath 'dirty.tmp') -Value 'keep'
            Push-Location $fixtureMain
            try {
                & pwsh -ExecutionPolicy Bypass -File $fixtureScript -Name foo 2>&1 | Out-Null
                if ($LASTEXITCODE -eq 0) { throw "$layout dirty worktree was removed." }
            } finally {
                Pop-Location
            }
            if (-not (Test-Path -LiteralPath $worktreePath)) { throw "$layout dirty checkout vanished." }
            Remove-Item -LiteralPath (Join-Path $worktreePath 'dirty.tmp')
            & git -C $fixtureMain worktree lock $worktreePath
            if ($LASTEXITCODE -ne 0) { throw "Could not lock $layout worktree." }
            $cleanupFrom = if ($layout -eq 'Claude') { $worktreePath } else { $fixtureMain }
            Push-Location $cleanupFrom
            try {
                if ($layout -eq 'Claude') {
                    & pwsh -ExecutionPolicy Bypass -File $fixtureScript -Name foo -FromInside -SyncMaster | Out-Null
                } else {
                    & pwsh -ExecutionPolicy Bypass -File $fixtureScript -Path $worktreePath | Out-Null
                }
                if ($LASTEXITCODE -ne 0) { throw "$layout fixture cleanup failed." }
            } finally {
                Pop-Location
            }
            if ($layout -eq 'Codex' -and (Test-Path -LiteralPath $worktreePath)) {
                throw "$layout worktree directory remains."
            }
            $registered = & git -C $fixtureMain worktree list --porcelain
            if ($registered -match [regex]::Escape($worktreePath)) { throw "$layout worktree remains registered." }
            & git -C $fixtureMain show-ref --verify --quiet refs/heads/codex/foo
            if ($LASTEXITCODE -eq 0) { throw "$layout local branch remains." }
            $remoteBranch = & git -C $fixtureMain ls-remote --heads origin codex/foo
            if ($remoteBranch) { throw "$layout remote branch remains." }
            if ($layout -eq 'Claude') {
                $mainHead = & git -C $fixtureMain rev-parse main
                $masterHead = & git -C $fixtureMain rev-parse master
                if ($mainHead -ne $masterHead) { throw "$layout -SyncMaster did not update master." }
            }
        }
        Write-Host "PASS: Remove-Worktree resolver and fixture self-test." -ForegroundColor Green
    } catch {
        Write-Host "FAIL: Remove-Worktree self-test: $_" -ForegroundColor Red
        exit 1
    } finally {
        if (Test-Path -LiteralPath $testRoot) {
            Remove-Item -LiteralPath $testRoot -Recurse -Force
        }
    }
    exit 0
}

$scriptRepoRoot = Split-Path $PSScriptRoot -Parent
$commonDir = & git -C $scriptRepoRoot rev-parse --path-format=absolute --git-common-dir 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: not inside a git repository." -ForegroundColor Red; exit 1 }
$MainCheckout = Split-Path $commonDir -Parent
$registryEntries = @(Read-WorktreeRegistry -Main $MainCheckout)
$resolved = Resolve-WorktreeTarget -Entries $registryEntries -Main $MainCheckout -RequestedName $Name -RequestedPath $Path -RequestedBranch $Branch
if ($resolved.Error) {
    $diagnostic = $resolved.Error
    if ($Path -and (Test-Path -LiteralPath $Path)) {
        $otherCommon = & git -C $Path rev-parse --path-format=absolute --git-common-dir 2>$null
        if ($LASTEXITCODE -eq 0 -and
            (Normalize-WorktreePath $otherCommon) -ine (Normalize-WorktreePath $commonDir)) {
            $diagnostic = "Path belongs to another repository: $Path"
        }
    }
    Write-Host "FAIL: $diagnostic" -ForegroundColor Red
    foreach ($candidate in $resolved.Candidates) { Write-Host "  $candidate" -ForegroundColor Yellow }
    exit 1
}
$TargetPath = $resolved.Path
$targetName = $resolved.Name
$resolvedBranch = $resolved.Branch

# Trap 1: refuse to remove the worktree we are standing in, unless the caller has
# confirmed via -FromInside that it cannot cd elsewhere (an agent session pinned to
# this directory for its lifetime). Safe to allow: every git call below targets
# $MainCheckout via -C, never cwd, and Trap 5 already tolerates the directory itself
# surviving the removal.
$here = (Get-Location).Path
$insideTarget = $here -eq $TargetPath -or
    $here.StartsWith($TargetPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
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

# Resolve a detached worktree's sibling branch when the caller did not supply one.
if (-not $resolvedBranch -and $targetName -and $resolved.Registered) {
    # Trap 4: a DETACHED worktree with a sibling branch at the same commit.
    #
    # Claude Code's auto-mode worktrees land in this shape: the worktree is detached,
    # while a separately-created branch (conventionally `claude/<dir-name>`) sits at the
    # same commit. Removing the directory alone silently leaves that branch behind, so
    # they accumulate invisibly -- exactly how code-terminal-auto-mode-2fd492 survived
    # unnoticed for a week.
    #
    # Matched conservatively: the branch must point at this worktree's HEAD *and* be
    # named either <targetName> or <something>/<targetName>. A branch that merely happens to share
    # the commit (master, main, or an unrelated branch sitting at the same merge) is
    # never picked up -- being at the same commit is not evidence of being related.
    $headSha = & git -C $TargetPath rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $headSha) {
        $candidates = @()
        foreach ($b in (& git -C $MainCheckout branch --format='%(refname:short)' --points-at $headSha)) {
            $b = $b.Trim()
            if (-not $b -or $b -eq 'master' -or $b -eq 'main') { continue }
            if ($b -eq $targetName -or $b -like "*/$targetName") { $candidates += $b }
        }

        if ($candidates.Count -eq 1) {
            $resolvedBranch = $candidates[0]
            Write-Host "`n==> Worktree '$targetName' is detached; found sibling branch '$resolvedBranch' at the same commit." -ForegroundColor Cyan
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
elseif (-not $resolvedBranch) {
    Write-Host "NOTE: no branch inferred for this target; skipping sibling-branch cleanup." -ForegroundColor Yellow
    if (-not $resolved.Registered) {
        Write-Host "      The worktree is unregistered, so its former HEAD cannot be checked. Pass -Branch to delete its branch." -ForegroundColor Yellow
    } elseif (-not $targetName) {
        Write-Host "      This path is outside the known layouts. Pass -Branch to delete a detached sibling branch." -ForegroundColor Yellow
    }
}
elseif ($resolvedBranch) {
    Write-Host "`n==> Worktree '$TargetPath' -> branch '$resolvedBranch'" -ForegroundColor Cyan
}

# Trap 2: verify merged, by ancestry -- not by branch name.
if ($resolvedBranch -and -not $Force) {
    & git -C $MainCheckout fetch origin main | Out-Null
    & git -C $MainCheckout merge-base --is-ancestor $resolvedBranch origin/main
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nFAIL: '$resolvedBranch' is not an ancestor of origin/main -- refusing to delete." -ForegroundColor Red
        Write-Host "      If the PR was SQUASH-merged this is expected; confirm the content landed:" -ForegroundColor Yellow
        Write-Host "        git diff origin/main $resolvedBranch --stat     # empty => safe to -Force" -ForegroundColor Yellow
        Write-Host "      Then re-run with -Force." -ForegroundColor Yellow
        exit 1
    }
    Write-Host "PASS: '$resolvedBranch' is contained in origin/main." -ForegroundColor Green
}

# Trap 3: locked worktrees refuse removal with an unhelpful message.
if (Test-Path $TargetPath) {
    & git -C $MainCheckout worktree unlock $TargetPath 2>&1 | Out-Null

    Write-Host "`n==> Removing worktree" -ForegroundColor Cyan
    $rmArgs = @('worktree', 'remove', $TargetPath)
    if ($Force) { $rmArgs += '--force' }
    & git -C $MainCheckout @rmArgs
    if ($LASTEXITCODE -ne 0) {
        $remaining = @(Read-WorktreeRegistry -Main $MainCheckout | Where-Object {
            (Normalize-WorktreePath $_.Path) -ieq $TargetPath
        })
        if ($remaining.Count -gt 0) {
            Write-Host "FAIL: worktree is still registered; branch cleanup is unsafe." -ForegroundColor Red
            exit 1
        }
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

if ($resolvedBranch) {
    Write-Host "`n==> Deleting local branch" -ForegroundColor Cyan
    & git -C $MainCheckout branch -D $resolvedBranch
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: local branch was not deleted; remote branch is unchanged." -ForegroundColor Red
        exit 1
    }
    Write-Host "PASS: local branch deleted." -ForegroundColor Green

    if (-not $KeepRemote) {
        Write-Host "`n==> Deleting remote branch" -ForegroundColor Cyan
        $remoteRef = & git -C $MainCheckout ls-remote --heads origin $resolvedBranch
        if ($remoteRef) {
            & git -C $MainCheckout push origin --delete $resolvedBranch
            if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: remote branch was not deleted." -ForegroundColor Red; exit 1 }
            Write-Host "PASS: remote branch deleted." -ForegroundColor Green
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
