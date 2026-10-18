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
      4. PR     -- creates it against `main`, or updates the title and body of an open one.

    The body is written to a temp file and passed with `--body-file`, which bypasses
    `.github/pull_request_template.md`. The template stays the one source of the body's
    structure anyway: an unspecified body is scaffolded from it, and a supplied body gets
    one warning per template `## ` heading it lacks. A warning, not a failure, so a short
    Docs PR is not blocked. A diff above Docs tier that leaves Docs/Changelog.md untouched
    also warns, unless the body's `## Changelog` section says `none — <reason>`.

    That scaffolding applies ONLY when creating. Re-running the script after another
    commit -- the normal way to update an open PR -- leaves the existing description
    untouched unless -Body/-BodyFile is passed. Anything else would silently replace a
    hand-written PR description with the placeholder on every subsequent push. A -Title
    that differs from the open PR's replaces it, and the script says which of the two it
    changed.

    This script deliberately does NOT dispatch the specialised reviewers (eval-reviewer,
    search-reviewer). That is step 3 of the checklist and is a judgement call about what
    the diff touches -- it prints a reminder naming the relevant reviewer when the diff
    includes Eval.cpp/Eval.h, AIPerplex.cpp/.h, or the PST/material tables in defines.h,
    and leaves the call to you.

.PARAMETER Title
    PR title. Required when creating. On an open PR, replaces its title when it differs;
    omitted, the title is left unchanged.

.PARAMETER BodyFile
    Path to a markdown file to use as the PR body. Mutually exclusive with -Body.

.PARAMETER Body
    Inline PR body text. Mutually exclusive with -BodyFile.

.PARAMETER Draft
    Open the PR as a draft. Use for work that must not merge yet (e.g. blocked on
    another PR landing first).

.PARAMETER AllowUnlistedReformat
    Forwarded to Validate-PrePR.ps1's blame-ignore check. Use when a commit touches 20+ sources
    and genuinely changes code -- a wide refactor -- so it must NOT go into
    .git-blame-ignore-revs, which would hide real authorship from git blame.

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
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\New-PullRequest.ps1 -Title "Add mobility eval term (#98)"

.PARAMETER SelfTest
    Run the guard cases and exit. Pure: no git, no gh, no network.

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
    [switch]$NoPr,
    [switch]$AllowUnlistedReformat,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The two guards that decide whether this script will push at all, split out so they can
# be asserted without a repository.
function Test-BranchAllowedForPr {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Branch)
    return $Branch -ne 'master' -and $Branch -ne 'main'
}

# Would this untracked file plausibly be an omitted build input? A source file anywhere,
# or anything at all under a project directory. Deliberately wider than "*.cpp": PR #244
# swept 2,726 tool-downloaded files in, and the cost of a false block is one -- explicit --
# `git add`, while the cost of a miss is a build that compiles different code than the PR.
function Test-UntrackedBlocksPr {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Path)
    return $Path -match '\.(cpp|h)$' -or
           $Path -match '^(StratEngine|StratChessEvolved|StratChessTests)(/|\\)'
}

# The `gh pr edit` arguments for an open PR: only what the caller supplied and changed.
# Empty when there is nothing to edit.
function Get-PrEditArgument {
    param(
        [Parameter(Mandatory)][string]$Number,
        [AllowEmptyString()][string]$NewTitle = '',
        [AllowEmptyString()][string]$CurrentTitle = '',
        [AllowEmptyString()][string]$BodyPath = ''
    )
    $edits = @()
    # Case-sensitive: a capitalisation fix is a change worth making.
    if ($NewTitle -and $NewTitle -cne $CurrentTitle) { $edits += '--title', $NewTitle }
    if ($BodyPath) { $edits += '--body-file', $BodyPath }
    if ($edits.Count -eq 0) { return @() }
    return @('pr', 'edit', $Number) + $edits
}

# A markdown text without its HTML comments, so text quoted inside a template hint is not
# read as the author's.
function Remove-HtmlComment {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    return $Text -replace '(?s)<!--.*?-->', ''
}

# The `## ` headings of a markdown text, in order.
function Get-BodyHeading {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $visible = Remove-HtmlComment -Text $Text
    return @([regex]::Matches($visible, '(?m)^##[ \t]+(.+?)[ \t]*\r?$') | ForEach-Object { $_.Groups[1].Value })
}

# The template headings a body lacks. Case-insensitive: "Test Plan" satisfies "Test plan".
function Get-MissingHeading {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Template,
        [Parameter(Mandatory)][AllowEmptyString()][string]$BodyText
    )
    $present = @(Get-BodyHeading -Text $BodyText)
    return @($Template | Where-Object { $present -notcontains $_ })
}

# Does a PR above Docs tier lack a Docs/Changelog.md entry? The escape is a `none — <reason>`
# line under `## Changelog`; markdown emphasis around "none" is allowed, the placeholder is not.
function Test-ChangelogMissing {
    param(
        [Parameter(Mandatory)][string]$Tier,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$ChangedFiles,
        [Parameter(Mandatory)][AllowEmptyString()][string]$BodyText
    )
    if ($Tier -eq 'Docs' -or $ChangedFiles -contains 'Docs/Changelog.md') { return $false }
    $visible = Remove-HtmlComment -Text $BodyText
    $section = [regex]::Match($visible, '(?ims)^##[ \t]+Changelog[ \t]*\r?$(.*?)(?=^##[ \t]|\z)')
    if (-not $section.Success) { return $true }
    $escape = '(?im)^[ \t]*(?:[-*][ \t]+)?[`*_]*none[`*_]*(?:[ \t]*[—–][ \t]*|[ \t]+-+[ \t]+)(?!<reason>)\S'
    return $section.Groups[1].Value -notmatch $escape
}

if ($SelfTest) {
    $branchCases = @(
        @{ Name = 'master is refused';               Branch = 'master';                    Expect = $false }
        @{ Name = 'main is refused';                 Branch = 'main';                      Expect = $false }
        @{ Name = 'a task branch is allowed';        Branch = 'worktree-mobility-eval';    Expect = $true }
        @{ Name = 'a branch merely containing main'; Branch = 'worktree-main-line-fix';    Expect = $true }
        @{ Name = 'detached HEAD is not master';     Branch = 'HEAD';                      Expect = $true }
    )
    $untrackedCases = @(
        @{ Name = 'a stray .cpp blocks';             Path = 'scratch/Probe.cpp';                    Expect = $true }
        @{ Name = 'a stray .h blocks';               Path = 'Probe.h';                              Expect = $true }
        @{ Name = 'anything under StratEngine blocks'; Path = 'StratEngine/notes.txt';              Expect = $true }
        @{ Name = 'backslash paths are matched too'; Path = 'StratChessTests\new.txt';              Expect = $true }
        @{ Name = 'a doc does not block';            Path = 'Docs/Workflow.md';                     Expect = $false }
        @{ Name = 'a scratch note does not block';   Path = 'scratch/notes.txt';                    Expect = $false }
        # Named for the failure it guards: a directory whose name merely starts with a
        # project name is not inside one, so it must not be swept in by the prefix rule.
        @{ Name = 'a lookalike prefix does not block'; Path = 'StratEngineNotes/readme.txt';        Expect = $false }
    )

    $editCases = @(
        @{ Name = 'a changed title is edited';          New = 'default on'; Current = 'default off'; Body = '';     Expect = 'pr edit 7 --title default on' }
        @{ Name = 'FALSIFY: the same title is no edit'; New = 'default on'; Current = 'default on';  Body = '';     Expect = '' }
        @{ Name = 'a case-only title change is edited'; New = 'Default on'; Current = 'default on';  Body = '';     Expect = 'pr edit 7 --title Default on' }
        @{ Name = 'no -Title leaves the title alone';   New = '';           Current = 'default off'; Body = 'b.md'; Expect = 'pr edit 7 --body-file b.md' }
        @{ Name = 'title and body are edited together'; New = 'default on'; Current = 'default off'; Body = 'b.md'; Expect = 'pr edit 7 --title default on --body-file b.md' }
    )

    $fixtureTemplate = "## Why`n<!-- why`n## Not a heading -->`n`n## Summary`n`n-`n`n## Test plan`r`n`n## Notes`n"
    $headingCases = @(
        @{ Name = 'template headings, comments skipped, CRLF trimmed'; Body = $fixtureTemplate;                      Expect = 'Why|Summary|Test plan|Notes' }
        @{ Name = 'a ### subheading is not a heading';                 Body = "### Why`n## Summary";            Expect = 'Summary' }
        @{ Name = 'no headings';                                       Body = 'Refs #502';                      Expect = '' }
    )
    $missingCases = @(
        @{ Name = 'a full body misses nothing';           Body = "## Why`n## Summary`n## Test plan`n## Notes";  Expect = '' }
        @{ Name = 'FALSIFY: a missing heading is named';  Body = "## Summary`n## Test plan`n## Notes";          Expect = 'Why' }
        @{ Name = 'case does not matter';                 Body = "## why`n## summary`n## Test Plan`n## NOTES";  Expect = '' }
        @{ Name = 'a heading only in a comment misses';   Body = "<!-- ## Why -->`n## Summary`n## Test plan`n## Notes"; Expect = 'Why' }
        @{ Name = 'a headingless body misses all';        Body = 'Refs #502';                                   Expect = 'Why|Summary|Test plan|Notes' }
    )
    $templateHeadings = @(Get-BodyHeading -Text $fixtureTemplate)
    $code = @('Scripts/New-PullRequest.ps1')
    $withEntry = @('Scripts/New-PullRequest.ps1', 'Docs/Changelog.md')
    $changelogCases = @(
        @{ Name = 'FALSIFY: Build tier, no entry, no escape warns'; Tier = 'Build';  Files = $code;      Body = "## Summary`n- x"; Expect = $true }
        @{ Name = 'an entry in the diff satisfies it';             Tier = 'Engine'; Files = $withEntry; Body = '';                Expect = $false }
        @{ Name = 'a Docs-tier diff owes nothing';                 Tier = 'Docs';   Files = @('Measurements/local.md'); Body = ''; Expect = $false }
        @{ Name = 'the none — <reason> escape';                    Tier = 'Build';  Files = $code;      Body = "## Changelog`nnone — tooling message only`n## Notes"; Expect = $false }
        @{ Name = 'the escape as a bullet, hyphen dash';           Tier = 'Build';  Files = $code;      Body = "## Changelog`r`n- none - reverts #800`r`n"; Expect = $false }
        @{ Name = 'the escape with none in backticks';             Tier = 'Build';  Files = $code;      Body = "## Changelog`n``none`` — test-only`n"; Expect = $false }
        @{ Name = 'the escape with none in bold';                  Tier = 'Build';  Files = $code;      Body = "## Changelog`n**none** – test-only`n"; Expect = $false }
        @{ Name = 'the copied placeholder warns';                  Tier = 'Build';  Files = $code;      Body = "## Changelog`n``none — <reason>```n"; Expect = $true }
        @{ Name = 'a hyphenated word is not the escape';           Tier = 'Build';  Files = $code;      Body = "## Changelog`nnone-of-these`n"; Expect = $true }
        @{ Name = 'none with no reason warns';                     Tier = 'Build';  Files = $code;      Body = "## Changelog`nnone`n"; Expect = $true }
        @{ Name = '"entry added" without an entry warns';          Tier = 'Build';  Files = $code;      Body = "## Changelog`nentry added`n"; Expect = $true }
        @{ Name = 'an escape inside a comment warns';              Tier = 'Build';  Files = $code;      Body = "## Changelog`n<!-- none — x -->`n"; Expect = $true }
        @{ Name = 'an escape under another heading warns';         Tier = 'Build';  Files = $code;      Body = "## Changelog`n`n## Notes`nnone — x"; Expect = $true }
    )

    $failed = 0
    foreach ($case in $headingCases) {
        $actual = @(Get-BodyHeading -Text $case.Body) -join '|'
        if ($actual -eq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else { $failed++; Write-Host "  FAIL  $($case.Name): got '$actual', expected '$($case.Expect)'" -ForegroundColor Red }
    }
    foreach ($case in $missingCases) {
        $actual = @(Get-MissingHeading -Template $templateHeadings -BodyText $case.Body) -join '|'
        if ($actual -eq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else { $failed++; Write-Host "  FAIL  $($case.Name): got '$actual', expected '$($case.Expect)'" -ForegroundColor Red }
    }
    foreach ($case in $changelogCases) {
        $actual = Test-ChangelogMissing -Tier $case.Tier -ChangedFiles $case.Files -BodyText $case.Body
        if ($actual -eq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else { $failed++; Write-Host "  FAIL  $($case.Name): got $actual, expected $($case.Expect)" -ForegroundColor Red }
    }
    foreach ($case in $editCases) {
        $actual = @(Get-PrEditArgument -Number '7' -NewTitle $case.New -CurrentTitle $case.Current -BodyPath $case.Body) -join ' '
        if ($actual -eq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else { $failed++; Write-Host "  FAIL  $($case.Name): got '$actual', expected '$($case.Expect)'" -ForegroundColor Red }
    }
    foreach ($case in $branchCases) {
        $actual = Test-BranchAllowedForPr -Branch $case.Branch
        if ($actual -eq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else { $failed++; Write-Host "  FAIL  $($case.Name): got $actual, expected $($case.Expect)" -ForegroundColor Red }
    }
    foreach ($case in $untrackedCases) {
        $actual = Test-UntrackedBlocksPr -Path $case.Path
        if ($actual -eq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else { $failed++; Write-Host "  FAIL  $($case.Name): got $actual, expected $($case.Expect)" -ForegroundColor Red }
    }

    Write-Host ''
    if ($failed -gt 0) {
        Write-Host "$failed self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    $total = $branchCases.Count + $untrackedCases.Count + $editCases.Count + $headingCases.Count + $missingCases.Count + $changelogCases.Count
    Write-Host "All $total self-test cases passed." -ForegroundColor Green
    exit 0
}

if ($BodyFile -and $Body) {
    Write-Host "FAIL: -Body and -BodyFile are mutually exclusive." -ForegroundColor Red
    exit 1
}

$RepoRoot = Split-Path $PSScriptRoot -Parent
$branch = (& git -C $RepoRoot rev-parse --abbrev-ref HEAD)
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: not a git repository." -ForegroundColor Red; exit 1 }

if (-not (Test-BranchAllowedForPr -Branch $branch)) {
    Write-Host "FAIL: refusing to open a PR from '$branch'." -ForegroundColor Red
    Write-Host "      PRs come from a per-task worktree branched off origin/main." -ForegroundColor Yellow
    Write-Host "      Create one: Scripts\New-Worktree.ps1 -Name <task>" -ForegroundColor Yellow
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
$blockingUntracked = @($untracked | Where-Object { Test-UntrackedBlocksPr -Path $_ })
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
    & pwsh -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Validate-PrePR.ps1') `
        -AllowUnlistedReformat:$AllowUnlistedReformat
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nFAIL: pre-PR validation failed -- nothing pushed." -ForegroundColor Red
        exit 1
    }
}

# --- 2b. Reviewer reminder ------------------------------------------------------
$changed = (& git -C $RepoRoot diff --name-only origin/main...HEAD)
$diffFailed = $LASTEXITCODE -ne 0
$reviewers = @()
if ($changed -match 'StratEngine/Eval\.(cpp|h)$')      { $reviewers += 'eval-reviewer' }
# Move ordering is not confined to AIPerplex: killer/history maintenance lives in ThreadData.h
# and MVV-LVA ordering in Sort.cpp. Reminding on any touch is deliberate -- see CLAUDE.md step 3
# for the criteria under which a human may self-certify a skip. Do not add suppression logic here.
if ($changed -match 'StratEngine/(AIPerplex\.(cpp|h)|ThreadData\.h|Sort\.(cpp|h))$') { $reviewers += 'search-reviewer' }
# defines.h is not an eval file in general, but the PSTs (g_Eval_Bitboards) and the material
# values (g_iPieceValues) live there, so a retune can land without touching Eval.cpp/.h at all.
# Match on touched line ranges rather than on the diff text: a retune edits table *values*, and
# those +/- lines never contain the symbol name.
if ($changed -match 'StratEngine/defines\.h$') {
    $definesLines = Get-Content -LiteralPath (Join-Path $RepoRoot 'StratEngine/defines.h')
    $spans = @()
    for ($i = 0; $i -lt $definesLines.Count; $i++) {
        if ($definesLines[$i] -match '\b(g_Eval_Bitboards|g_iPieceValues)\b') {
            $end = $i
            while ($end -lt $definesLines.Count - 1 -and $definesLines[$end] -notmatch '^\s*\};') { $end++ }
            $spans += , @(($i + 1), ($end + 1))
        }
    }
    foreach ($hunk in (& git -C $RepoRoot diff -U0 origin/main...HEAD -- 'StratEngine/defines.h')) {
        if ($hunk -notmatch '^@@ .* \+(\d+)(?:,(\d+))? @@') { continue }
        $first = [int]$Matches[1]
        $count = if ($Matches[2]) { [int]$Matches[2] } else { 1 }
        # A pure deletion reports +N,0; treat it as touching line N so it is not missed.
        $last = $first + [Math]::Max($count, 1) - 1
        foreach ($span in $spans) {
            if ($first -le $span[1] -and $last -ge $span[0]) { $reviewers += 'eval-reviewer'; break }
        }
    }
}
$reviewers = @($reviewers | Select-Object -Unique)
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

# A hint, not a call. Waiting on CI here would change this script's contract from
# "submit" to "submit and babysit", and would conflate two exit codes that have to
# stay separate: 0 from this script means the pull request was submitted, never that
# CI liked it. Naming the command is enough -- the failure mode is forgetting the
# step, not finding it hard to type. (-NoPr returns above, having nothing to watch.)
function Show-NextStep {
    Write-Host "`nNext:" -ForegroundColor Cyan
    Write-Host ("  pwsh -File {0} -Wait" -f (Join-Path $PSScriptRoot 'Get-PrChecks.ps1'))
    Write-Host '    one line if the checks go green; the failing step and its errors if not' -ForegroundColor DarkGray
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

$currentTitle = ''
if ($existing) {
    $currentTitle = (& gh pr view $existing --json title --jq '.title')
    if ($LASTEXITCODE -ne 0) {
        Write-Host "WARNING: could not read PR #$existing's title -- branch is pushed, update the PR yourself." -ForegroundColor Yellow
        exit 0
    }
}
$titleChanged = $existing -and @(Get-PrEditArgument -Number $existing -NewTitle $Title -CurrentTitle $currentTitle).Count -gt 0

if ($existing -and -not $bodySupplied -and -not $titleChanged) {
    # Updating an open PR with nothing new to say: leave it alone. Re-running this script
    # after another commit is the normal case, and silently replacing a hand-written
    # description with the scaffold is destructive and easy to miss.
    Write-Host "PR #$existing is already open for this branch -- pushed; title and body left unchanged." -ForegroundColor Yellow
    Write-Host "  (pass -Title or -Body/-BodyFile to update them)" -ForegroundColor DarkGray
    & gh pr view $existing --json url --jq '.url'
    Write-Host "`nDone." -ForegroundColor Green
    Show-NextStep
    exit 0
}

$templatePath = Join-Path $RepoRoot '.github/pull_request_template.md'

$tempBody = $null
$bodyPath = ''
if ($BodyFile) {
    if (-not (Test-Path $BodyFile)) { Write-Host "FAIL: -BodyFile not found: $BodyFile" -ForegroundColor Red; exit 1 }
    $bodyPath = $BodyFile
} elseif ($bodySupplied -or -not $existing) {
    # An open PR whose title alone changes keeps its description: no scaffold.
    if (-not $Body) {
        # --body-file bypasses the template, so scaffold from it rather than open an empty PR.
        $Body = Get-Content -LiteralPath $templatePath -Raw
    }
    $tempBody = Join-Path ([System.IO.Path]::GetTempPath()) ("pr-body-{0}.md" -f ([guid]::NewGuid()))
    Set-Content -Path $tempBody -Value $Body -Encoding utf8
    $bodyPath = $tempBody
}

if ($bodyPath) {
    $prBody = "$(Get-Content -LiteralPath $bodyPath -Raw)"
    if ($bodySupplied) {
        $templateHeadings = @(Get-BodyHeading -Text (Get-Content -LiteralPath $templatePath -Raw))
        $missingHeadings = @(Get-MissingHeading -Template $templateHeadings -BodyText $prBody)
        foreach ($h in $missingHeadings) { Write-Host "WARNING: body lacks the template heading '## $h'." -ForegroundColor Yellow }
    }
    # A warning, not a failure: a fail would block a PR on prose. A failed diff fails closed.
    $prFiles = @($changed | Where-Object { $_ })
    $prTier = if ($diffFailed) { 'Engine' } elseif ($prFiles.Count -gt 0) { (& (Join-Path $PSScriptRoot 'Get-ChangeTier.ps1') -Paths $prFiles).Tier } else { 'Docs' }
    if (Test-ChangelogMissing -Tier $prTier -ChangedFiles $prFiles -BodyText $prBody) {
        Write-Host "WARNING: $prTier-tier diff with no Docs/Changelog.md entry -- add one, or write 'none — <reason>' under '## Changelog'." -ForegroundColor Yellow
    }
}

try {
    if ($existing) {
        $titleNote = if ($titleChanged) { "title '$currentTitle' -> '$Title'" } else { 'title unchanged' }
        $bodyNote = if ($bodySupplied) { 'body updated' } else { 'body unchanged' }
        Write-Host "PR #$existing is already open for this branch -- $titleNote; $bodyNote." -ForegroundColor Yellow
        $editArgs = @(Get-PrEditArgument -Number $existing -NewTitle $Title -CurrentTitle $currentTitle -BodyPath $bodyPath)
        # `gh pr edit` prints the PR URL to stdout on success. Suppress it and let the
        # explicit view below be the one source of that line, so every path through
        # this step emits exactly one URL.
        & gh @editArgs | Out-Null
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
Show-NextStep
