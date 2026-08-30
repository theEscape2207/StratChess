<#
.SYNOPSIS
    Classify a diff into a validation tier — the single source of truth shared by
    Validate-PrePR.ps1 and .github/workflows/build-and-test.yml.

.DESCRIPTION
    Answers one question: "given what changed, how much validation is worth running?"

    Tiers, weakest to strictest (a mixed diff always takes the STRICTEST tier present):

      Docs     *.md, Docs/**, .claude/plans/**,
               .claude/skills/**.md, .claude/agents/**.md
               -> nothing beyond the pre-commit hook's fast tests.

      Tooling  measurement/helper scripts that are never compiled and never invoked
               by the engine (Run-EloMatch, Run-Tests, Sync-Master, verify_mate_key)
               -> a PowerShell syntax parse. A full build cannot catch anything here.

      Build    build.ps1, the Validate-* scripts, this script, .githooks/**,
               .github/**, project/props files
               -> full validation, no shortcut.

      Engine   everything else — *.cpp, *.h, *.json, AND anything unrecognised
               -> full validation.

    Two properties matter more than the speedup and are asserted by -SelfTest:

      1. FAIL CLOSED. The default rule is Engine. There is deliberately no
         "else -> cheap" branch: an unfamiliar path must cost time, never skip.
      2. NO SELF-EXEMPTION. The validation machinery itself (Validate-*.ps1, this
         file, build.ps1) is Build tier. If a change to them could take its own
         shortcut, a classifier bug would be self-concealing — it would disable
         validation and then decline to validate the change that disabled it.

.PARAMETER BaseRef
    Ref to diff against. Default 'origin/main'. Ignored when -Paths is supplied.

.PARAMETER Paths
    Explicit file list instead of shelling out to git. Used by -SelfTest and by
    callers that already have the diff.

.PARAMETER SelfTest
    Run the assertion table and exit. Exits 1 on any failure.

.OUTPUTS
    PSCustomObject with Tier, DecidingFile, ChangedFiles, IsFull.

.HOW TO INVOKE
    pwsh -ExecutionPolicy Bypass -File Scripts\Get-ChangeTier.ps1
    pwsh -ExecutionPolicy Bypass -File Scripts\Get-ChangeTier.ps1 -SelfTest

.NOTES
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>

param(
    [string]$BaseRef = 'origin/main',
    [string[]]$Paths,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest

# Strictness ranking. Higher wins when a diff spans several tiers.
$script:TierRank = @{ 'Docs' = 0; 'Tooling' = 1; 'Build' = 2; 'Engine' = 3 }

function Get-TierForPath {
    param([Parameter(Mandatory)][string]$Path)

    # Normalise to forward slashes so the rules below are separator-agnostic
    # (git reports '/', Windows callers may pass '\').
    $p = $Path.Replace('\', '/').Trim()

    # --- Build: the validation/build machinery itself -------------------------
    # MUST be tested before any general Scripts/*.ps1 rule, or the validators
    # would fall into Tooling — the exact self-exemption hazard this guards.
    if ($p -eq 'build.ps1')                                  { return 'Build' }
    if ($p -like '*Scripts/Validate-*.ps1')                 { return 'Build' }
    # Gates whether Validate-PrePR.ps1 runs at all, so a bug in it could exempt a real
    # change from validation — the same self-concealment hazard the Validate-* rule
    # above guards against. Build tier, never Tooling.
    if ($p -like '*Scripts/New-PullRequest.ps1')            { return 'Build' }
    if ($p -like '*Scripts/Get-ChangeTier.ps1')             { return 'Build' }
    # Validate-PrePR.ps1 invokes Run-Lint.ps1's format check, so a bug in it could
    # suppress a gate and then decline to validate the change that suppressed it --
    # the same self-concealment hazard as the Validate-* rule above. Build, never
    # Tooling, despite living beside the engine-inert helper scripts.
    if ($p -like '*Scripts/Run-Lint.ps1')                   { return 'Build' }
    if ($p -like '*Scripts/New-TidyCompileDatabase.ps1')    { return 'Build' }
    # Enforces that every CI job states a timeout, from inside CI. Same hazard once
    # more: a bug here disarms a guard silently. Named rather than left to the
    # fail-closed default, which would call it Engine -- stricter than a script that
    # compiles nothing and is never invoked by the engine deserves.
    if ($p -like '*Scripts/Test-WorkflowTimeouts.ps1')      { return 'Build' }
    # Decides whether a build artifact counts as stale, and which binary a measurement
    # reads. Both reach Build anyway through the fail-closed default, but only as
    # "unrecognised", which costs every PR that touches them the Engine tier. The hazard
    # is the familiar one and it is why they are Build rather than Tooling: a bug in
    # either lets a validation or a measurement run against the wrong binary while
    # reporting success.
    if ($p -like '*Scripts/BuildFreshness.ps1')             { return 'Build' }
    if ($p -like '*Scripts/Get-BuildArtifact.ps1')          { return 'Build' }
    # Lint configuration decides what CI enforces about every source file. It reaches
    # Build tier anyway through the fail-closed default, but only as "unrecognised";
    # naming it makes the classification deliberate and the self-test able to assert it.
    if ($p -eq '.clang-format' -or $p -eq '.clang-tidy' -or
        $p -eq '.clang-tidy-deep' -or $p -like '*/.clang-tidy') { return 'Build' }
    if ($p -eq '.git-blame-ignore-revs')                     { return 'Build' }
    if ($p -like '.githooks/*')                              { return 'Build' }
    if ($p -like '.github/*')                                { return 'Build' }
    if ($p -like '*.vcxproj' -or $p -like '*.vcxproj.*')     { return 'Build' }
    if ($p -like '*.props' -or $p -like '*.sln')             { return 'Build' }
    if ($p -like 'CMakeLists.txt' -or $p -like '*/CMakeLists.txt') { return 'Build' }
    if ($p -like '*.cmake')                                  { return 'Build' }
    # Presets carry the compiler, generator and cache variables, so a change here
    # can alter the produced binary as surely as a compile flag in CMakeLists.txt.
    if ($p -like 'CMakePresets.json' -or $p -like '*/CMakePresets.json') { return 'Build' }

    # --- Docs -----------------------------------------------------------------
    # Skill and agent definitions are named ahead of the '*.md' rule so their tier is a
    # decision rather than an extension match. Docs is what that decision is today: the
    # stricter tiers add a PowerShell parse and a build, neither of which says anything
    # about a Markdown definition file. Scoped to '*.md' deliberately -- a skill directory
    # can hold executable content, and that stays with the fail-closed default.
    if ($p -like '.claude/skills/*.md')                      { return 'Docs' }
    if ($p -like '.claude/agents/*.md')                      { return 'Docs' }
    # Plans are prose whatever the extension, so this one is not '*.md'-scoped.
    if ($p -like '.claude/plans/*')                          { return 'Docs' }
    if ($p -like '*.md')                                     { return 'Docs' }
    if ($p -like 'Docs/*')                                   { return 'Docs' }

    # --- Tooling: engine-inert helper scripts ---------------------------------
    # Enumerated explicitly rather than matched as 'Scripts/*.ps1'. A wildcard
    # here would silently absorb any NEW script added to that folder, including
    # one that does affect the build — fail-closed means new files land in
    # Engine until someone deliberately classifies them.
    if ($p -like '*Scripts/Run-EloMatch.ps1')               { return 'Tooling' }
    if ($p -like '*Scripts/Run-Bench.ps1')                  { return 'Tooling' }
    if ($p -like '*Scripts/Compare-SearchEquivalence.ps1')  { return 'Tooling' }
    if ($p -like '*Scripts/Run-PerftCheck.ps1')             { return 'Tooling' }
    if ($p -like '*Scripts/Run-Tests.ps1')                  { return 'Tooling' }
    if ($p -like '*Scripts/Sync-Master.ps1')                { return 'Tooling' }
    if ($p -like '*Scripts/verify_mate_key.py')             { return 'Tooling' }
    if ($p -like '*Scripts/build_corpus.py')                { return 'Tooling' }
    if ($p -like '*Scripts/uci_race_probe.py')              { return 'Tooling' }
    if ($p -like '*Scripts/Measure-UciLatency.ps1')         { return 'Tooling' }
    # Dot-sourced by Compare-SearchEquivalence.ps1 and Run-Bench.ps1, both Tooling. Unlike
    # BuildFreshness.ps1 it gates nothing: it drives an engine and reports what it said,
    # so a bug in it makes a measurement fail loudly rather than pass wrongly.
    if ($p -like '*Scripts/UciDriver.ps1')                  { return 'Tooling' }
    # Branch/worktree management. These create, list and tear down worktrees and
    # branches; none of them compiles anything or is invoked by the engine, so a
    # change to one cannot alter build or test behaviour.
    #
    # New-PullRequest.ps1 is deliberately NOT here — it has its own Build rule above,
    # because it gates whether validation runs.
    if ($p -like '*Scripts/New-Worktree.ps1')               { return 'Tooling' }
    if ($p -like '*Scripts/Remove-Worktree.ps1')            { return 'Tooling' }
    if ($p -like '*Scripts/Get-Worktrees.ps1')              { return 'Tooling' }
    # The in-place counterparts: a task branch in the current worktree instead of a
    # worktree of its own. Same reasoning — neither compiles anything nor is invoked by
    # the engine.
    if ($p -like '*Scripts/New-TaskBranch.ps1')             { return 'Tooling' }
    if ($p -like '*Scripts/Remove-MergedBranches.ps1')      { return 'Tooling' }
    # Reports a pull request's check state. Read-only and advisory: it gates nothing,
    # so unlike New-PullRequest.ps1 above there is no self-concealment hazard to guard
    # against. Same shape as Get-Worktrees.ps1.
    if ($p -like '*Scripts/Get-PrChecks.ps1')               { return 'Tooling' }

    # --- Fail closed ----------------------------------------------------------
    # Everything else, INCLUDING anything unrecognised. Do not add an
    # "else -> Docs/Tooling" branch here; see the note in .DESCRIPTION.
    return 'Engine'
}

function Get-ChangeTier {
    param([string[]]$Files)

    if (-not $Files -or $Files.Count -eq 0) {
        # An empty diff has nothing to validate. Docs is the correct (weakest)
        # answer — but note this is reached only when git reports no changes at
        # all, never as a fallback for an unclassifiable path.
        return [PSCustomObject]@{
            Tier = 'Docs'; DecidingFile = ''; ChangedFiles = @(); IsFull = $false
        }
    }

    $winner = 'Docs'
    $deciding = $Files[0]
    foreach ($f in $Files) {
        if ([string]::IsNullOrWhiteSpace($f)) { continue }
        $t = Get-TierForPath -Path $f
        if ($script:TierRank[$t] -gt $script:TierRank[$winner]) {
            $winner = $t
            $deciding = $f
        }
    }

    return [PSCustomObject]@{
        Tier         = $winner
        DecidingFile = $deciding
        ChangedFiles = $Files
        IsFull       = ($winner -eq 'Build' -or $winner -eq 'Engine')
    }
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
if ($SelfTest) {
    $cases = @(
        @{ Name = 'docs only';                  Files = @('README.md', 'Docs/EloLog.md', '.claude/plans/x.md'); Expect = 'Docs' }
        # Named rules, not the '*.md' fallthrough -- and only the Markdown inside those
        # directories; anything else there is still an unfamiliar path.
        @{ Name = 'skill definition -> Docs';   Files = @('.claude/skills/measure-strength/SKILL.md');           Expect = 'Docs' }
        @{ Name = 'agent definition -> Docs';   Files = @('.claude/agents/eval-reviewer.md');                    Expect = 'Docs' }
        @{ Name = 'FAIL CLOSED: skill script';  Files = @('.claude/skills/measure-strength/helper.ps1');         Expect = 'Engine' }
        @{ Name = 'FAIL CLOSED: skill dir';     Files = @('.claude/skills/measure-strength/');                   Expect = 'Engine' }
        @{ Name = 'tooling only';               Files = @('Scripts/Run-EloMatch.ps1');        Expect = 'Tooling' }
        @{ Name = 'corpus tool -> Tooling';     Files = @('Scripts/build_corpus.py');         Expect = 'Tooling' }
        @{ Name = 'race probe -> Tooling';      Files = @('Scripts/uci_race_probe.py');       Expect = 'Tooling' }
        @{ Name = 'bench tool -> Tooling';      Files = @('Scripts/Run-Bench.ps1');          Expect = 'Tooling' }
        @{ Name = 'perftcheck tool -> Tooling'; Files = @('Scripts/Run-PerftCheck.ps1');     Expect = 'Tooling' }
        @{ Name = 'equivalence tool -> Tooling'; Files = @('Scripts/Compare-SearchEquivalence.ps1'); Expect = 'Tooling' }
        @{ Name = 'UCI driver lib -> Tooling';  Files = @('Scripts/UciDriver.ps1');           Expect = 'Tooling' }
        @{ Name = 'docs + tooling -> Tooling';  Files = @('CLAUDE.md', 'Scripts/Run-Tests.ps1'); Expect = 'Tooling' }
        @{ Name = 'docs + cpp -> Engine';       Files = @('CLAUDE.md', 'StratEngine/Eval.cpp');                 Expect = 'Engine' }
        @{ Name = 'build.ps1 -> Build';         Files = @('build.ps1');                                          Expect = 'Build' }
        @{ Name = 'validator -> Build NOT Tooling'; Files = @('Scripts/Validate-PrePR.ps1');   Expect = 'Build' }
    @{ Name = 'New-Worktree -> Tooling';    Files = @('Scripts/New-Worktree.ps1');    Expect = 'Tooling' }
    @{ Name = 'Remove-Worktree -> Tooling'; Files = @('Scripts/Remove-Worktree.ps1'); Expect = 'Tooling' }
    @{ Name = 'Get-Worktrees -> Tooling';   Files = @('Scripts/Get-Worktrees.ps1');   Expect = 'Tooling' }
        @{ Name = 'Get-PrChecks -> Tooling';    Files = @('Scripts/Get-PrChecks.ps1');   Expect = 'Tooling' }
    # The in-place counterparts to New-Worktree/Remove-Worktree: same reasoning, same tier.
    @{ Name = 'New-TaskBranch -> Tooling';       Files = @('Scripts/New-TaskBranch.ps1');       Expect = 'Tooling' }
    @{ Name = 'Remove-MergedBranches -> Tooling'; Files = @('Scripts/Remove-MergedBranches.ps1'); Expect = 'Tooling' }
    # The PR driver gates validation, so it must never take the Tooling shortcut.
    @{ Name = 'New-PullRequest -> Build NOT Tooling'; Files = @('Scripts/New-PullRequest.ps1'); Expect = 'Build' }
        @{ Name = 'classifier -> Build';        Files = @('Scripts/Get-ChangeTier.ps1');       Expect = 'Build' }
        # The lint runner gates validation, so it must never take the Tooling shortcut.
        @{ Name = 'Run-Lint -> Build NOT Tooling'; Files = @('Scripts/Run-Lint.ps1');           Expect = 'Build' }
        @{ Name = '.clang-format -> Build';     Files = @('.clang-format');                                      Expect = 'Build' }
        @{ Name = '.clang-tidy -> Build';       Files = @('.clang-tidy');                                        Expect = 'Build' }
        @{ Name = 'Deep tidy config -> Build';  Files = @('.clang-tidy-deep');                                   Expect = 'Build' }
        @{ Name = 'test tidy config -> Build';  Files = @('StratChessTests/.clang-tidy');                         Expect = 'Build' }
        @{ Name = 'tidy DB normalizer -> Build'; Files = @('Scripts/New-TidyCompileDatabase.ps1'); Expect = 'Build' }
        @{ Name = 'timeout guard -> Build';     Files = @('Scripts/Test-WorkflowTimeouts.ps1');  Expect = 'Build' }
        # Both decided which binary a build or a measurement reads while classified only by
        # the fail-closed default. Named now, so the classification is a decision.
        @{ Name = 'freshness lib -> Build';     Files = @('Scripts/BuildFreshness.ps1');        Expect = 'Build' }
        @{ Name = 'artifact picker -> Build';   Files = @('Scripts/Get-BuildArtifact.ps1');     Expect = 'Build' }
        @{ Name = 'blame-ignore -> Build';      Files = @('.git-blame-ignore-revs');                             Expect = 'Build' }
        @{ Name = 'workflow -> Build';          Files = @('.github/workflows/build-and-test.yml');               Expect = 'Build' }
        @{ Name = 'hook -> Build';              Files = @('.githooks/pre-commit');                               Expect = 'Build' }
        @{ Name = 'vcxproj -> Build';           Files = @('StratChessTests/StratChessTests.vcxproj');            Expect = 'Build' }
        @{ Name = 'CMakeLists -> Build';        Files = @('CMakeLists.txt');                                     Expect = 'Build' }
        @{ Name = 'cmake module -> Build';      Files = @('cmake/Toolchain.cmake');                              Expect = 'Build' }
        @{ Name = 'CMakePresets -> Build';      Files = @('CMakePresets.json');                                  Expect = 'Build' }
        @{ Name = 'FAIL CLOSED: unknown ext';   Files = @('foo/bar.xyz');                                        Expect = 'Engine' }
        @{ Name = 'FAIL CLOSED: new script';    Files = @('Scripts/Brand-New.ps1');            Expect = 'Engine' }
        @{ Name = 'json -> Engine';             Files = @('StratChessEvolved/game_settings.json');               Expect = 'Engine' }
        @{ Name = 'header -> Engine';           Files = @('StratEngine/Eval.h');                                 Expect = 'Engine' }
        @{ Name = 'backslash paths normalise';  Files = @('Scripts\Run-EloMatch.ps1');         Expect = 'Tooling' }
        @{ Name = 'empty diff';                 Files = @();                                                     Expect = 'Docs' }
        # Real PRs this issue was filed over.
        @{ Name = 'PR #123 (CLAUDE.md only)';   Files = @('CLAUDE.md');                                          Expect = 'Docs' }
        @{ Name = 'PR #133 (SPRT)';             Files = @('Scripts/Run-EloMatch.ps1', 'Docs/EloLog.md', 'CLAUDE.md', 'Docs/Changelog.md', '.claude/plans/elomatch-sprt-support.md'); Expect = 'Tooling' }
    )

    $failed = 0
    foreach ($c in $cases) {
        $got = (Get-ChangeTier -Files $c.Files).Tier
        if ($got -eq $c.Expect) {
            Write-Host ("  PASS  {0,-34} -> {1}" -f $c.Name, $got) -ForegroundColor Green
        } else {
            Write-Host ("  FAIL  {0,-34} -> {1} (expected {2})" -f $c.Name, $got, $c.Expect) -ForegroundColor Red
            $failed++
        }
    }
    Write-Host ''
    if ($failed -gt 0) { Write-Host "$failed self-test case(s) FAILED." -ForegroundColor Red; exit 1 }
    Write-Host "All $($cases.Count) self-test cases passed." -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# Normal invocation
# ---------------------------------------------------------------------------
if (-not $Paths) {
    $RepoRoot = Split-Path $PSScriptRoot -Parent
    # Three-dot: changes on HEAD since it diverged from BaseRef, which is what a
    # PR actually contains -- two-dot would also report changes made on BaseRef.
    $committed = @(git -C $RepoRoot diff --name-only "$BaseRef...HEAD" 2>$null | Where-Object { $_ })
    if ($LASTEXITCODE -ne 0) {
        # Cannot diff (missing ref, shallow clone). Fail closed: assume the most
        # expensive tier rather than skipping validation on a broken lookup.
        Write-Warning "Get-ChangeTier: 'git diff $BaseRef...HEAD' failed -- assuming Engine tier (fail closed)."
        [PSCustomObject]@{ Tier = 'Engine'; DecidingFile = '<git diff failed>'; ChangedFiles = @(); IsFull = $true }
        return
    }

    # Uncommitted work counts too. Classifying only committed changes would mean
    # that running this before committing -- which is exactly when someone reaches
    # for a validation script -- silently classifies an empty diff as Docs and skips
    # every gate. Validate what is on disk, not merely what has been recorded.
    # --porcelain covers staged, unstaged and untracked; the status code occupies
    # the first 3 columns. Renames appear as 'old -> new'; take the destination.
    $working = @(git -C $RepoRoot status --porcelain 2>$null | Where-Object { $_ } | ForEach-Object {
        $p = $_.Substring(3).Trim().Trim('"')
        if ($p -match '\s->\s') { $p = ($p -split '\s->\s')[-1].Trim().Trim('"') }
        $p
    })

    $Paths = @($committed + $working | Where-Object { $_ } | Select-Object -Unique)
}

Get-ChangeTier -Files $Paths
