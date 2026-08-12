<#
.SYNOPSIS
    Run the pre-PR checklist, push the current worktree branch, and open (or update) its PR.

.DESCRIPTION
    Automates CLAUDE.md's "Pre-PR checklist" so the steps cannot be done out of order or
    silently skipped:

      1. Sync   -- `git fetch origin main` then `git merge origin/main`. Stops on conflict
                   rather than leaving a half-merged tree.
      2. Validate -- `Validate-PrePR.ps1`, which self-scopes via Get-ChangeTier.ps1.
      3. Push   -- `git push -u origin <branch>`.
      4. PR     -- creates it against `main`, or updates the body if one is already open.

    The body is written to a temp file and passed with `--body-file`, because
    `gh pr create --body` bypasses `.github/pull_request_template.md` entirely. To keep
    PRs consistent anyway, an unspecified body is scaffolded with the project's
    Summary / Test plan / Notes headings rather than left empty.

    That scaffolding applies ONLY when creating. Re-running the script after another
    commit -- the normal way to update an open PR -- leaves the existing description
    untouched unless -Body/-BodyFile is passed. Anything else would silently replace a
    hand-written PR description with the placeholder on every subsequent push.

    This script deliberately does NOT dispatch the specialised reviewers (eval-reviewer,
    search-reviewer). That is step 3 of the checklist and is a judgement call about what
    the diff touches -- it prints a reminder naming the relevant reviewer when the diff
    includes Eval.cpp/Eval.h or AIPerplex.cpp/.h, and leaves the call to you.

.PARAMETER Title
    PR title. Required when creating; ignored when updating an existing PR.

.PARAMETER BodyFile
    Path to a markdown file to use as the PR body. Mutually exclusive with -Body.

.PARAMETER Body
    Inline PR body text. Mutually exclusive with -BodyFile.

.PARAMETER Draft
    Open the PR as a draft. Use for work that must not merge yet (e.g. blocked on
    another PR landing first).

.PARAMETER SkipValidation
    Skip Validate-PrePR.ps1. For docs-only changes where the pre-commit hook already
    covered it, or to re-push after a trivial amend. Prints a loud warning -- the whole
    point of the script is that this step is not silently skippable.

.PARAMETER NoPr
    Sync, validate and push, but stop before touching `gh`. Use when you prefer to open
    the PR from Visual Studio.

.WHEN TO USE
    When a worktree branch is ready to become a PR, or to update one already open.

.HOW TO INVOKE (from bash, cmd, or PowerShell), from inside the worktree
    pwsh -ExecutionPolicy Bypass -File C:\...\StratChessEvolved\Scripts\New-PullRequest.ps1 -Title "Add mobility eval term (#98)"

.NOTES
    Refuses to run on `master` -- that branch is personal scratch and is not the source
    of PRs (see CLAUDE.md). Must be invoked with -File, not dot-sourced.
#>

[CmdletBinding()]
param(
    [string]$Title,
    [string]$BodyFile,
    [string]$Body,
    [switch]$Draft,
    [switch]$SkipValidation,
    [switch]$NoPr
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($BodyFile -and $Body) {
    Write-Host "FAIL: -Body and -BodyFile are mutually exclusive." -ForegroundColor Red
    exit 1
}

$GameDir  = Split-Path $PSScriptRoot -Parent
$RepoRoot = Split-Path $GameDir -Parent

$branch = (& git -C $RepoRoot rev-parse --abbrev-ref HEAD)
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: not a git repository." -ForegroundColor Red; exit 1 }

if ($branch -eq 'master' -or $branch -eq 'main') {
    Write-Host "FAIL: refusing to open a PR from '$branch'." -ForegroundColor Red
    Write-Host "      PRs come from a per-task worktree branched off origin/main." -ForegroundColor Yellow
    Write-Host "      Create one: StratChessEvolved\Scripts\New-Worktree.ps1 -Name <task>" -ForegroundColor Yellow
    exit 1
}

Write-Host "`n==> Branch: $branch" -ForegroundColor Cyan

# Tracked changes must be committed. Untracked files are allowed only when they cannot
# plausibly be an omitted build input; see Docs/Workflow.md for the narrow contract.
$tracked = @(& git -C $RepoRoot status --porcelain | Where-Object { $_ -and $_ -notmatch '^\?\?' })
if ($tracked.Count -gt 0) {
    Write-Host "FAIL: working tree has uncommitted changes to tracked files -- commit them first." -ForegroundColor Red
    Write-Host (($tracked | Out-String).Trim()) -ForegroundColor Yellow
    exit 1
}

# `git status` collapses an untracked directory to one entry, which would hide an untracked
# .cpp/.h nested under an otherwise harmless directory. `ls-files --others` lists every file.
$untracked = @(& git -C $RepoRoot ls-files --others --exclude-standard)
$blockingUntracked = @($untracked | Where-Object {
    $_ -match '\.(cpp|h)$' -or $_ -match '^(StratEngine|StratChessEvolved|StratChessTests)(/|\\)'
})
if ($blockingUntracked.Count -gt 0) {
    Write-Host "FAIL: working tree has untracked files that could be omitted from the build or commit." -ForegroundColor Red
    $blockingUntracked | ForEach-Object { Write-Host "      $_" -ForegroundColor Yellow }
    Write-Host "      Add them if intentional, or remove/move them, then re-run." -ForegroundColor Yellow
    exit 1
}
if ($untracked.Count -gt 0) {
    Write-Host "WARNING: continuing with untracked non-build files:" -ForegroundColor Yellow
    $untracked | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkYellow }
}

# --- 1. Sync -------------------------------------------------------------------
Write-Host "`n==> [1/4] Syncing with origin/main" -ForegroundColor Cyan
& git -C $RepoRoot fetch origin main
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: fetch failed." -ForegroundColor Red; exit 1 }

$behind = (& git -C $RepoRoot rev-list --count "HEAD..origin/main")
if ([int]$behind -gt 0) {
    Write-Host "Branch is $behind commit(s) behind origin/main -- merging." -ForegroundColor Yellow
    & git -C $RepoRoot merge origin/main
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nFAIL: merge conflict. Resolve it, commit, then re-run this script." -ForegroundColor Red
        Write-Host "      Keep BOTH sides when the conflict is just two PRs adding unrelated" -ForegroundColor Yellow
        Write-Host "      declarations at the same anchor -- do not drop either (see CLAUDE.md)." -ForegroundColor Yellow
        exit 1
    }
    Write-Host "PASS: merged origin/main." -ForegroundColor Green
} else {
    Write-Host "PASS: already up to date with origin/main." -ForegroundColor Green
}

# --- 2. Validate ---------------------------------------------------------------
Write-Host "`n==> [2/4] Pre-PR validation" -ForegroundColor Cyan
if ($SkipValidation) {
    Write-Host "WARNING: -SkipValidation set; Validate-PrePR.ps1 did NOT run." -ForegroundColor Yellow
} else {
    & pwsh -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Validate-PrePR.ps1')
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nFAIL: pre-PR validation failed -- nothing pushed." -ForegroundColor Red
        exit 1
    }
}

# --- 2b. Reviewer reminder ------------------------------------------------------
$changed = (& git -C $RepoRoot diff --name-only origin/main...HEAD)
$reviewers = @()
if ($changed -match 'StratEngine/Eval\.(cpp|h)$')      { $reviewers += 'eval-reviewer' }
# Move ordering is not confined to AIPerplex: killer/history maintenance lives in ThreadData.h
# and MVV-LVA ordering in Sort.cpp. Reminding on any touch is deliberate -- see CLAUDE.md step 3
# for the criteria under which a human may self-certify a skip. Do not add suppression logic here.
if ($changed -match 'StratEngine/(AIPerplex\.(cpp|h)|ThreadData\.h|Sort\.(cpp|h))$') { $reviewers += 'search-reviewer' }
if ($reviewers.Count -gt 0) {
    Write-Host "`nREMINDER: this diff touches a reviewed area." -ForegroundColor Yellow
    foreach ($r in $reviewers) { Write-Host "  dispatch the '$r' subagent before merging (CLAUDE.md step 3)." -ForegroundColor Yellow }
}

# --- 3. Push -------------------------------------------------------------------
Write-Host "`n==> [3/4] Pushing" -ForegroundColor Cyan
& git -C $RepoRoot push -u origin $branch
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: push failed." -ForegroundColor Red; exit 1 }
Write-Host "PASS: pushed." -ForegroundColor Green

if ($NoPr) {
    Write-Host "`n-NoPr set: stopping before the PR. Open it from Visual Studio when ready." -ForegroundColor Cyan
    exit 0
}

# --- 4. PR ---------------------------------------------------------------------
Write-Host "`n==> [4/4] Pull request" -ForegroundColor Cyan

$existing = (& gh pr list --head $branch --state open --json number --jq '.[0].number' 2>$null)
if ($LASTEXITCODE -ne 0) {
    Write-Host "WARNING: 'gh' unavailable or not authenticated -- branch is pushed, open the PR yourself." -ForegroundColor Yellow
    exit 0
}

# Did the caller actually supply a body? Must be captured BEFORE the scaffold below
# fills $Body in, otherwise the update path cannot tell "no body given" apart from
# "body given", and would overwrite a real PR description with the placeholder.
$bodySupplied = [bool]($BodyFile -or $Body)

if ($existing -and -not $bodySupplied) {
    # Updating an open PR with nothing to say about the body: leave it alone. Re-running
    # this script after another commit is the normal case, and silently replacing a
    # hand-written description with the scaffold is destructive and easy to miss.
    Write-Host "PR #$existing is already open for this branch -- pushed; body left unchanged." -ForegroundColor Yellow
    Write-Host "  (pass -Body/-BodyFile to update the description)" -ForegroundColor DarkGray
    & gh pr view $existing --json url --jq '.url'
    Write-Host "`nDone." -ForegroundColor Green
    exit 0
}

$tempBody = $null
if ($BodyFile) {
    if (-not (Test-Path $BodyFile)) { Write-Host "FAIL: -BodyFile not found: $BodyFile" -ForegroundColor Red; exit 1 }
    $bodyPath = $BodyFile
} else {
    if (-not $Body) {
        # gh --body bypasses .github/pull_request_template.md, so scaffold the house
        # headings rather than opening an empty PR.
        $Body = @"
## Summary

<what changed and why>

## Test plan

- ``Scripts\Validate-PrePR.ps1`` -- PASS
-

## Notes

<risks, follow-ups, or 'none'>
"@
    }
    $tempBody = Join-Path ([System.IO.Path]::GetTempPath()) ("pr-body-{0}.md" -f ([guid]::NewGuid()))
    Set-Content -Path $tempBody -Value $Body -Encoding utf8
    $bodyPath = $tempBody
}

try {
    if ($existing) {
        Write-Host "PR #$existing is already open for this branch -- updating its body." -ForegroundColor Yellow
        & gh pr edit $existing --body-file $bodyPath
        if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: gh pr edit failed." -ForegroundColor Red; exit 1 }
        & gh pr view $existing --json url --jq '.url'
    } else {
        if (-not $Title) {
            Write-Host "FAIL: -Title is required when creating a new PR." -ForegroundColor Red
            exit 1
        }
        $ghArgs = @('pr', 'create', '--base', 'main', '--head', $branch, '--title', $Title, '--body-file', $bodyPath)
        if ($Draft) { $ghArgs += '--draft' }
        & gh @ghArgs
        if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: gh pr create failed." -ForegroundColor Red; exit 1 }
    }
} finally {
    if ($tempBody -and (Test-Path $tempBody)) { Remove-Item $tempBody -Force }
}

Write-Host "`nDone." -ForegroundColor Green
