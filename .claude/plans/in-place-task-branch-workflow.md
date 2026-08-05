# In-place task branches: one worktree, N sequential branches

> **For agentic workers:** implement task-by-task, in order. Steps use checkbox (`- [ ]`) syntax for
> tracking. Each task ends with a commit and is independently reviewable.

**Goal:** make "one worktree, many sequential task branches" a first-class workflow alongside the
per-task worktree model, enforcing in tooling the two invariants it currently leaves to discipline.

**Architecture:** a parallel verb set. Two new scripts mirror `New-Worktree.ps1` and
`Remove-Worktree.ps1` without touching them, and `Sync-Master.ps1` learns to route to whichever
worktree holds `master` instead of demanding it be run from there.

**Tech Stack:** PowerShell 7 (`pwsh`), git worktrees. No test framework — see "Testing note".

## Global constraints

- PowerShell scripts are invoked with `-File`, never dot-sourced (`$PSScriptRoot` is `$null` under
  dot-source). Every script states this in `.NOTES`.
- House script style: comment-based help with `.SYNOPSIS`, `.DESCRIPTION`, `.WHEN TO USE`,
  `.HOW TO INVOKE`, `.NOTES`; `Set-StrictMode -Version Latest`; `$ErrorActionPreference = 'Stop'`.
- Task branches are named `worktree-<name>`; `<name>` is kebab-case.
- Comments describe the code as it stands — no task/gate labels, no history.

## Testing note

There is no Pester or other PowerShell test framework in this repository, and these scripts are
Tooling tier, so `Validate-PrePR.ps1` only syntax-parses them. The test-first cycle here is at
command level: run the verification command, watch it fail for the stated reason, implement, run it
again. The one exception is Task 4, where `Get-ChangeTier.ps1 -SelfTest` is a real assertion table
and gets a genuine red-to-green cycle.

## The two modes, and why both are worth having

| | Per-task worktree | In-place task branches |
|---|---|---|
| Start | `New-Worktree.ps1 -Name x` | `New-TaskBranch.ps1 -Name x` |
| Finish | `Remove-Worktree.ps1 -Name x -SyncMaster` | `Remove-MergedBranches.ps1 -SyncMaster` |
| Parallel tasks | Yes — park one, switch to another | No — sequential only |
| Build directory | Cold per worktree; first build needs network | Stays warm across tasks |
| Path depth | Deeper (`.claude/worktrees/<name>/…`) | Repository root |
| Cleanup failure mode | Orphaned directories and unregistered worktrees | `git branch -D` |

Neither dominates. The worktree model is right when work must be parked and resumed, or when two
tasks are genuinely in flight. In-place is right for a run of small sequential PRs — four of them in
one session, in the case that prompted this — where per-task directory churn buys nothing and the
warm build directory is worth real minutes.

**The catch this plan addresses:** the worktree model enforces two invariants *structurally*, and
in-place mode leaves both to discipline.

1. **Every task forks fresh from `origin/main`.** `New-Worktree.ps1` fetches and forks from
   `origin/main` unconditionally and never reads the current branch. In-place, a hand-typed
   `git checkout -b` can branch off the previous task, dragging its commits into the next PR.
2. **Uncommitted work cannot leak between tasks.** Separate working trees make it impossible.
   In-place, `git checkout` carries a dirty tree across branches whenever it can do so without
   conflict.

## Key findings

Established while writing this plan; each one changes a step.

1. **The two scripts resolve their target repository differently, and only one is right.**
   `New-Worktree.ps1` asks git (`rev-parse --path-format=absolute --git-common-dir`) and is
   documented as safe from anywhere. `Sync-Master.ps1` derives its root from `$PSScriptRoot` — the
   copy of the file that was invoked — so running a worktree's copy makes it operate on that
   worktree and attempt `git checkout master`, which git refuses because `master` is checked out in
   the main checkout.
2. **`Sync-Master.ps1` already computes the right answer, and throws it away.** Its failure path
   greps `git worktree list` for `[master]` to tell the user where to go. Promoting that from
   diagnostic to routing is most of the fix.
3. **The stash stack is shared across all worktrees** (CLAUDE.md). `Sync-Master.ps1` stashes
   uncommitted changes by design, so a delegated sync must **refuse** on a dirty target rather than
   stash — another session could pop those changes.
4. **Nothing keys on the `worktree-` branch prefix.** `delete-merged-branch.yml` deletes any merged
   PR head branch except `master`/`main`; `Remove-Worktree.ps1` resolves branches by worktree path.
   The prefix is therefore a free choice, so it stays as-is: one prefix, zero migration, and it
   simply stops meaning "has a directory".
5. **`Get-ChangeTier.ps1` fails closed on new scripts.** Anything unrecognised under `Scripts/` is
   Engine tier, with a self-test (`FAIL CLOSED: new script`) asserting it. New tooling must be
   enumerated deliberately — which makes this change Build tier, since it edits the classifier.
6. **`Remove-Worktree.ps1` is 217 lines of hard-won traps** — refusing to remove the worktree you
   stand in, resolving branches from `worktree list --porcelain`, catching detached auto-mode
   worktrees with orphaned sibling branches. None of it may be re-implemented.
7. **Git refuses even a ref-only update to a branch checked out elsewhere.** Verified:
   `git fetch origin main:master` from a worktree returns *"refusing to fetch into branch
   'refs/heads/master' checked out at …"*. There is no worktree-side workaround, which is why
   delegation is the only design available.
8. **Every git call in `Sync-Master.ps1` goes through one `Invoke-Git` wrapper** that reads
   `$RepoRoot`. Retargeting that single variable therefore redirects the whole script, which is what
   keeps the change small.

## Design decisions

**A parallel verb set, not `-InPlace` flags on the existing scripts.** `New-Worktree.ps1 -InPlace`
would create no worktree, so the name lies at every call site, and both existing scripts would grow a
second path through logic that is already subtle (finding 6). Separate small scripts keep each name
honest and leave the worktree scripts untouched.

**The delegated sync refuses rather than stashes.** Finding 3. `Sync-Master.ps1` keeps stashing when
it operates on its own worktree, because those are the caller's own changes; it must not do so to
another working copy.

**Merged-ness is verified, never inferred from a name.** `git merge-base --is-ancestor <branch>
origin/main`, per the project's own rule after an earlier near-miss. `git branch -d` is not
sufficient: it asks whether the branch merged into the *current* branch, which is the wrong question.

**A merged current branch is reported, not auto-switched.** Silently moving someone's HEAD is
surprising; `New-TaskBranch.ps1` is the thing that moves you off it, deliberately.

**Untracked files never count as "dirty", in either script.** They survive a checkout unchanged,
cannot leak into a commit by themselves, and a fast-forward does not touch them — git refuses on its
own in the one case that matters, where an incoming commit would overwrite one. Only tracked
modifications block.

This turned out to be load-bearing rather than a nicety. The first run of the delegated sync refused
because the main checkout held an untracked `.lean-ctx/` tool-cache directory; since such
directories are permanent fixtures, treating untracked entries as dirt would have blocked every
delegated sync forever and the fix would have fixed nothing. Discovered by running it, not by
reading it.

## Files changed

| File | Change |
|---|---|
| `StratChessEvolved/Scripts/Sync-Master.ps1` | Route to the worktree holding `master`; refuse on a dirty delegated target |
| `StratChessEvolved/Scripts/New-TaskBranch.ps1` | **New** — fork `worktree-<name>` from `origin/main` in the current worktree |
| `StratChessEvolved/Scripts/Remove-MergedBranches.ps1` | **New** — delete verified-merged local branches; optional `-SyncMaster` |
| `StratChessEvolved/Scripts/Get-ChangeTier.ps1` | Classify both new scripts as Tooling; one self-test case each |
| `CLAUDE.md` | Two script rows; a note that both modes exist and what each guarantees |
| `Docs/Workflow.md` | The two modes, the two invariants, and which script enforces each |

---

## Task 1: Route `Sync-Master.ps1` to wherever `master` lives

**Files:**
- Modify: `StratChessEvolved/Scripts/Sync-Master.ps1` (help block, lines 46-47, the stash block)

**Interfaces:**
- Consumes: nothing.
- Produces: `Sync-Master.ps1` succeeds from any worktree. Task 3 relies on this for `-SyncMaster`.

- [ ] **Step 1: Capture the current failure**

Run from this worktree:

```
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Sync-Master.ps1
```

Expected: `FAIL: could not check out master.` followed by
`master is checked out at: C:/Users/thees/source/repos/StratChessEvolved`, exit code 1.

- [ ] **Step 2: Add the resolver and retarget `$RepoRoot`**

Replace lines 46-47 (`$GameDir` / `$RepoRoot`) with:

```powershell
$GameDir   = Split-Path $PSScriptRoot -Parent
$LocalRoot = Split-Path $GameDir -Parent

# A branch can be checked out in only one worktree, so `master` has exactly one home
# and the sync has to happen there. Refs and objects are shared, so everything else
# works from anywhere. This is why the script never needs the caller to be in a
# particular directory -- it finds the tree that owns the branch and works there.
function Get-MasterWorktree {
    param([string]$Root)
    $path = $null
    foreach ($line in (& git -C $Root worktree list --porcelain 2>$null)) {
        if ($line -like 'worktree *') { $path = $line.Substring(9) }
        elseif ($line -eq 'branch refs/heads/master' -and $path) { return $path }
    }
    return $null
}

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
```

Every git call routes through `Invoke-Git`, which reads `$RepoRoot`, so this one assignment
redirects the whole script. `$originalBranch` is then read from the target tree and is `master`
there, which makes the existing `if ($originalBranch -ne 'master')` checkout block skip itself.

- [ ] **Step 3: Guard the stash block and refuse a dirty delegated target**

Replace the `==> Checking working tree` block (the `$status = Invoke-Git @('status', '--porcelain')`
section) with:

```powershell
Write-Host "`n==> Checking working tree" -ForegroundColor Cyan
$originalBranch = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Output
$switchedBranch = $false
$stashed = $false

$status = Invoke-Git @('status', '--porcelain')
if ($Delegated) {
    # Never stash another working copy's changes: the stash stack is shared across
    # every worktree, so a different session could pop them. Refuse instead, and say
    # where to go.
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
}
elseif ($status.Output) {
    Write-Host "Working tree has uncommitted changes -- stashing before sync." -ForegroundColor Yellow
    $stash = Invoke-Git @('stash', 'push', '-u', '-m', 'Sync-Master.ps1 autostash')
    if ($stash.ExitCode -ne 0) {
        Write-Host "FAIL: could not stash uncommitted changes." -ForegroundColor Red
        Write-GitOutput $stash.Output
        Restore-AndExit -Code 1 -OriginalBranch $originalBranch -SwitchedBranch $switchedBranch -Stashed $stashed
    }
    $stashed = $true
    Write-Host "PASS: uncommitted changes stashed." -ForegroundColor Green
}
else {
    Write-Host "PASS: working tree is clean." -ForegroundColor Green
}
```

- [ ] **Step 4: Update `.NOTES`**

Replace the "Must be run from the main repository checkout…" paragraph with:

```
    Runs from anywhere in the repository. `master` can only be checked out in one
    worktree, so the script finds that worktree and syncs there. When that is not the
    tree you invoked from, it refuses on a dirty target instead of stashing -- the
    stash stack is shared, and those changes are not this script's to move.
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
```

- [ ] **Step 5: Verify the delegated path**

```
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Sync-Master.ps1
```

Expected: `PASS: master's worktree is clean`, then either `already up to date with origin/main` or a
fast-forward report. Exit code 0. Then confirm nothing moved locally: `git status --porcelain` in
this worktree is unchanged, and `git rev-parse master origin/main` prints two identical SHAs.

- [ ] **Step 6: Commit**

```bash
git add StratChessEvolved/Scripts/Sync-Master.ps1
git commit -m "Route Sync-Master to the worktree holding master"
```

---

## Task 2: `New-TaskBranch.ps1`

**Files:**
- Create: `StratChessEvolved/Scripts/New-TaskBranch.ps1`

**Interfaces:**
- Consumes: nothing.
- Produces: `New-TaskBranch.ps1 -Name <kebab> [-BranchName <override>]`, creating branch
  `worktree-<Name>` at `origin/main`. Task 3 points users at it; Task 4 classifies it.

- [ ] **Step 1: Verify it does not exist yet**

```
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\New-TaskBranch.ps1 -Name probe-case
```

Expected: the file is not found, non-zero exit.

- [ ] **Step 2: Write the script**

```powershell
<#
.SYNOPSIS
    Start a task on a fresh branch in the CURRENT worktree.

.DESCRIPTION
    The in-place counterpart to New-Worktree.ps1, for running several small tasks in
    sequence out of one worktree instead of one directory per task. It removes the same
    traps that script removes, minus the ones about directories:

    1. **Forking from the wrong base.** A hand-typed `git checkout -b` off the previous
       task's branch drags that task's commits into the next PR. This always forks from
       `origin/main` and never reads the current branch.
    2. **Forgetting to fetch first** silently forks from a stale `origin/main`, which
       surfaces as a merge conflict at PR time instead of now.
    3. **Carrying uncommitted work across tasks.** `git checkout` moves a dirty tree to
       the new branch whenever it can do so without conflict, which is how one task's
       edits end up in another task's commit. Separate worktrees make that impossible;
       here it takes a check.

    Untracked files do NOT block: they survive a checkout unchanged, cannot enter a
    commit on their own, and `build/` would otherwise trip every run.

.PARAMETER Name
    Short kebab-case task name, e.g. 'eval-mobility-term'. Becomes the branch name,
    prefixed.

.PARAMETER BranchName
    Override the derived `worktree-<Name>` branch name.

.WHEN TO USE
    Starting a task when you intend to stay in this worktree. Use New-Worktree.ps1
    instead when the task must run alongside another one, or when you need to park it
    half-finished and come back.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\New-TaskBranch.ps1 -Name eval-mobility-term"

.NOTES
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

& git rev-parse --git-dir *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: not inside a git repository." -ForegroundColor Red
    exit 1
}

# Tracked changes only. Untracked entries are reported by porcelain as '??' and are
# deliberately allowed through.
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

# Leaving commits behind is legal -- an open PR is the usual reason -- so this warns
# rather than blocks. It exists because in-place mode has no directory left sitting
# there to remind you.
$ahead = [int](& git rev-list --count origin/main..HEAD 2>$null)
if ($LASTEXITCODE -eq 0 -and $ahead -gt 0) {
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
Write-Host "  .\StratChessEvolved\Scripts\New-PullRequest.ps1 -Title `"...`""
```

- [ ] **Step 3: Verify the happy path forks from `origin/main`, not HEAD**

Run it from a branch that is *ahead* of `origin/main`, so "forked from HEAD" and "forked from
`origin/main`" would give different answers:

```
git rev-parse --abbrev-ref HEAD              # this plan's branch, ahead by 1+ commits
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\New-TaskBranch.ps1 -Name probe-case
git rev-parse HEAD
git rev-parse origin/main
```

Expected: the last two SHAs are identical — the new branch sits exactly on `origin/main` and carries
none of the previous branch's commits.

- [ ] **Step 4: Verify the three refusals and the untracked exemption**

```
pwsh ... New-TaskBranch.ps1 -Name Bad_Name        # kebab-case refusal, exit 1
pwsh ... New-TaskBranch.ps1 -Name probe-case      # existing-branch refusal, exit 1
echo x >> CLAUDE.md ; pwsh ... New-TaskBranch.ps1 -Name probe-two   # dirty refusal, exit 1
git checkout -- CLAUDE.md
echo x > scratch.txt ; pwsh ... New-TaskBranch.ps1 -Name probe-two  # untracked: SUCCEEDS
```

Then clean up the probe branches:

```
git checkout worktree-in-place-task-branches
git branch -D worktree-probe-case worktree-probe-two
rm scratch.txt
```

- [ ] **Step 5: Commit**

```bash
git add StratChessEvolved/Scripts/New-TaskBranch.ps1
git commit -m "Add New-TaskBranch for in-place task starts"
```

---

## Task 3: `Remove-MergedBranches.ps1`

**Files:**
- Create: `StratChessEvolved/Scripts/Remove-MergedBranches.ps1`

**Interfaces:**
- Consumes: `Sync-Master.ps1` (Task 1) for `-SyncMaster`.
- Produces: `Remove-MergedBranches.ps1 [-SyncMaster] [-DryRun]`.

- [ ] **Step 1: Write the script**

```powershell
<#
.SYNOPSIS
    Delete local branches already merged into origin/main.

.DESCRIPTION
    The in-place counterpart to Remove-Worktree.ps1's branch cleanup, for the mode where
    tasks are branches in one worktree rather than directories. It answers one question
    per branch -- "is this contained in origin/main?" -- and answers it with
    `git merge-base --is-ancestor`, never by matching a name. A branch called
    `worktree-old-thing` may hold unpushed work; a branch whose name looks unfamiliar may
    be fully merged. Only ancestry is evidence.

    `git branch -d` is not sufficient: it tests whether a branch merged into the CURRENT
    branch, which is a different question and the wrong one here. This verifies against
    origin/main first, then deletes with -D.

    Never deletes: master, main, the branch you are on, and any branch checked out in
    another worktree (git would refuse anyway).

.PARAMETER SyncMaster
    Run Sync-Master.ps1 afterwards so local master reflects the merges.

.PARAMETER DryRun
    Report what would be deleted and delete nothing.

.WHEN TO USE
    After one or more PRs merge, when working in-place. The worktree flow uses
    Remove-Worktree.ps1 instead, which also removes the directory.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    cmd.exe /c "pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Remove-MergedBranches.ps1 -SyncMaster"

.NOTES
    Must be invoked with -File, not dot-sourced ($PSScriptRoot is $null under dot-source).
#>

[CmdletBinding()]
param(
    [switch]$SyncMaster,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

& git rev-parse --git-dir *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: not inside a git repository." -ForegroundColor Red
    exit 1
}

Write-Host "`n==> Fetching origin/main" -ForegroundColor Cyan
& git fetch origin main --prune
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: git fetch origin main failed." -ForegroundColor Red
    exit 1
}

$currentBranch = (& git rev-parse --abbrev-ref HEAD).Trim()

# Branches checked out anywhere else cannot be deleted, so they are skipped with a
# reason rather than attempted and reported as errors.
$checkedOutElsewhere = @()
foreach ($line in (& git worktree list --porcelain)) {
    if ($line -like 'branch *') {
        $b = $line.Substring(7) -replace '^refs/heads/', ''
        if ($b -ne $currentBranch) { $checkedOutElsewhere += $b }
    }
}

$deleted = @()
$skipped = @()
foreach ($branch in (& git branch --format='%(refname:short)')) {
    $branch = $branch.Trim()
    if (-not $branch) { continue }
    if ($branch -in @('master', 'main')) { continue }
    if ($branch -eq $currentBranch)      { continue }
    if ($branch -in $checkedOutElsewhere) {
        $skipped += "$branch (checked out in another worktree)"
        continue
    }

    & git merge-base --is-ancestor $branch origin/main
    if ($LASTEXITCODE -ne 0) {
        $skipped += "$branch (not merged into origin/main)"
        continue
    }

    if ($DryRun) {
        $deleted += "$branch (dry run)"
    } else {
        & git branch -D $branch *> $null
        if ($LASTEXITCODE -eq 0) { $deleted += $branch }
        else { $skipped += "$branch (delete failed)" }
    }
}

Write-Host "`n--- Merged branches ---" -ForegroundColor Cyan
if ($deleted.Count -eq 0) { Write-Host "  none" }
else { $deleted | ForEach-Object { Write-Host "  deleted: $_" -ForegroundColor Green } }

if ($skipped.Count -gt 0) {
    Write-Host "`n--- Kept ---" -ForegroundColor Cyan
    $skipped | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
}

# Reported rather than acted on: moving someone's HEAD without being asked is
# surprising, and New-TaskBranch.ps1 is the thing that moves you off it.
& git merge-base --is-ancestor $currentBranch origin/main
if ($LASTEXITCODE -eq 0 -and $currentBranch -notin @('master', 'main')) {
    Write-Host "`nNOTE: the branch you are on ('$currentBranch') is also merged." -ForegroundColor Yellow
    Write-Host "      Start the next task with New-TaskBranch.ps1, which moves you off it," -ForegroundColor Yellow
    Write-Host "      then re-run this to clean it up." -ForegroundColor Yellow
}

if ($SyncMaster) {
    Write-Host "`n==> Syncing master" -ForegroundColor Cyan
    & pwsh -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Sync-Master.ps1')
}

exit 0
```

- [ ] **Step 2: Verify with throwaway branches**

Build one branch of each kind without disturbing the current one:

```
git branch probe-merged origin/main                  # contained in origin/main by construction
git checkout -b probe-unmerged origin/main
git commit --allow-empty -m "probe commit"           # now NOT contained in origin/main
git checkout worktree-in-place-task-branches
pwsh ... Remove-MergedBranches.ps1 -DryRun
```

Expected: `probe-merged` listed as a dry-run deletion; `probe-unmerged` kept with "not merged into
origin/main"; the current branch and this worktree's own branch never listed for deletion.

- [ ] **Step 3: Verify the real deletion, then clean up**

```
pwsh ... Remove-MergedBranches.ps1
git branch --list 'probe-*'
git branch -D probe-unmerged
```

Expected: `probe-merged` gone, `probe-unmerged` still present.

- [ ] **Step 4: Commit**

```bash
git add StratChessEvolved/Scripts/Remove-MergedBranches.ps1
git commit -m "Add Remove-MergedBranches for in-place cleanup"
```

---

## Task 4: Classify both scripts as Tooling

**Files:**
- Modify: `StratChessEvolved/Scripts/Get-ChangeTier.ps1` (Tooling rules; `$cases` table)

**Interfaces:**
- Consumes: the two script paths from Tasks 2 and 3.
- Produces: both classify as `Tooling`; `-SelfTest` covers them.

- [ ] **Step 1: Add the failing self-test cases first**

In the `$cases` array, after the `Get-Worktrees -> Tooling` line:

```powershell
    @{ Name = 'New-TaskBranch -> Tooling';  Files = @('StratChessEvolved/Scripts/New-TaskBranch.ps1');       Expect = 'Tooling' }
    @{ Name = 'Remove-MergedBranches -> Tooling'; Files = @('StratChessEvolved/Scripts/Remove-MergedBranches.ps1'); Expect = 'Tooling' }
```

- [ ] **Step 2: Run the self-test and watch it fail**

```
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Get-ChangeTier.ps1 -SelfTest
```

Expected: both new cases FAIL with `-> Engine (expected Tooling)`, exit code 1. That is the
fail-closed rule working: an unrecognised script is Engine tier until someone classifies it.

- [ ] **Step 3: Add the classification rules**

In `Get-TierForPath`, with the other enumerated helpers:

```powershell
    if ($p -like '*/Scripts/New-TaskBranch.ps1')             { return 'Tooling' }
    if ($p -like '*/Scripts/Remove-MergedBranches.ps1')      { return 'Tooling' }
```

- [ ] **Step 4: Run the self-test and watch it pass**

```
pwsh -ExecutionPolicy Bypass -File StratChessEvolved\Scripts\Get-ChangeTier.ps1 -SelfTest
```

Expected: `All N self-test cases passed.`, exit code 0.

- [ ] **Step 5: Commit**

```bash
git add StratChessEvolved/Scripts/Get-ChangeTier.ps1
git commit -m "Classify the in-place task scripts as Tooling"
```

---

## Task 5: Document both modes

**Files:**
- Modify: `CLAUDE.md` (Scripts table, Commit & PR Conventions)
- Modify: `Docs/Workflow.md` (a "Two ways to run a task" section)

**Interfaces:**
- Consumes: everything above.
- Produces: nothing further depends on this.

- [ ] **Step 1: Add the two rows to `CLAUDE.md`'s Scripts table**

After the `New-Worktree.ps1` row:

```
| `New-TaskBranch.ps1 -Name <task>` | Starting a task without a new worktree — forks a branch from `origin/main` in place |
| `Remove-MergedBranches.ps1 [-SyncMaster]` | After PRs merge, when working in-place — deletes branches verified merged into `origin/main` |
```

And amend the `Sync-Master.ps1` row, which no longer carries a location constraint:

```
| `Sync-Master.ps1` | Bring `master` up to `origin/main` — runs from anywhere; finds the worktree holding `master` |
```

- [ ] **Step 2: Add the mode note to `CLAUDE.md`'s Commit & PR Conventions**

After the "Work happens in per-task worktrees" bullet:

```
- Two ways to run a task, both forking fresh from `origin/main`: a per-task worktree
  (`New-Worktree.ps1`) when work must be parked or run alongside another, or a task branch in the
  current worktree (`New-TaskBranch.ps1`) for a run of small sequential PRs. In-place mode is
  sequential only, and its guard is that the script refuses to start a task on a dirty tree.
```

- [ ] **Step 3: Add the section to `Docs/Workflow.md`**

Before the CI section, add "Two ways to run a task" carrying: the comparison table from this plan,
the two invariants and which script enforces each, and the note that `Sync-Master.ps1` routes to
`master`'s worktree so in-place mode never silently drifts.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md Docs/Workflow.md
git commit -m "Document both task-running modes"
```

---

## Validation plan

Tooling tier: `Validate-PrePR.ps1` syntax-parses these scripts and runs nothing else, so behaviour
must be checked by hand — the per-task steps above are that check. Before the PR:

| Check | Method |
|---|---|
| Classifier | `Get-ChangeTier.ps1 -SelfTest` green |
| Pre-PR gate | `New-PullRequest.ps1` — Build tier, because `Get-ChangeTier.ps1` itself changed |
| No stray probe branches | `git branch --list '*probe*'` is empty — catches both `probe-*` (Task 3) and `worktree-probe-*` (Task 2) |
| `master` untouched by the probes | `git rev-parse master origin/main` prints identical SHAs |

**Known coverage gap.** The "refuse when the delegated target is dirty" path needs the main checkout
to be dirty, and an agent session confined to a worktree cannot arrange that. It is verified by
inspection only; the project owner can exercise it in one command from the main checkout, or accept
it untested. It is recorded here rather than quietly counted as covered.

## Invariants after this work

1. Every task branch, in either mode, is forked from a freshly fetched `origin/main` — never from
   `master`, never from the previous task's branch.
2. No script stashes or switches branches in a working tree other than the one it was invoked in.
   Exactly one cross-worktree mutation exists — advancing `master` by fast-forward, or by a merge
   commit when it has diverged — and it happens only in a tree first verified clean and free of an
   in-progress merge or rebase. (That mutation does update files in the other tree; it is the point
   of the sync, and it is safe precisely because the tree was verified clean first.)
3. A branch is deleted only after `merge-base --is-ancestor` proves it is contained in
   `origin/main`. Names are never evidence.
4. `Sync-Master.ps1` succeeds from anywhere in the repository, or explains precisely which working
   tree is blocking it.
5. New scripts under `Scripts/` remain Engine tier until deliberately classified, and every
   classification carries a self-test case.
