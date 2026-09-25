<#
.SYNOPSIS
    Classify a diff into a validation tier — the single source of truth shared by
    Validate-PrePR.ps1 and .github/workflows/build-and-test.yml.

.DESCRIPTION
    Answers one question: "given what changed, how much validation is worth running?"

    Tiers, weakest to strictest (a mixed diff always takes the STRICTEST tier present):

      Docs     *.md, Docs/**, .claude/plans/**, .claude/skills/**.md,
               .claude/agents/**.md and their Codex counterparts
               -> nothing beyond the pre-commit hook's fast tests.

      Tooling  every *.ps1, *.py and *.cmd directly in Scripts/ that the Build list
               does not name: never compiled, never invoked by the engine
               -> a PowerShell syntax parse. A full build cannot catch anything here.

      Build    build.ps1, the Validate-* scripts, this script, .githooks/**,
               .github/**, CMake files
               -> full validation, no shortcut.

      Engine   everything else — *.cpp, *.h, *.json, AND anything unrecognised
               -> full validation.

    Two properties matter more than the speedup and are asserted by -SelfTest:

      1. FAIL CLOSED. The default rule is Engine. There is deliberately no
         "else -> cheap" branch: an unfamiliar path must cost time, never skip.
         Scripts/ is the one exception, and -CheckGates is what makes it safe:
         every script a gate invokes must be on the Build list.
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

.PARAMETER CheckGates
    Fail when a workflow, a git hook or a Build-tier script invokes a Scripts/ file
    that is not Build tier. Exits 1 on any such invocation, or when it finds none.

.OUTPUTS
    PSCustomObject with Tier, DecidingFile, ChangedFiles, IsFull.

.HOW TO INVOKE
    pwsh -ExecutionPolicy Bypass -File Scripts\Get-ChangeTier.ps1
    pwsh -ExecutionPolicy Bypass -File Scripts\Get-ChangeTier.ps1 -SelfTest
    pwsh -ExecutionPolicy Bypass -File Scripts\Get-ChangeTier.ps1 -CheckGates

.NOTES
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>

[CmdletBinding()]
param(
    [string]$BaseRef = 'origin/main',
    [string[]]$Paths,
    [switch]$SelfTest,
    [switch]$CheckGates
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
    # The workflow guards, which enforce properties of CI from inside CI. Same hazard
    # once more: a bug here disarms a guard silently. Named rather than left to the
    # fail-closed default, which would call them Engine -- stricter than scripts that
    # compile nothing and are never invoked by the engine deserve. The prefix match
    # covers guards added later, which would otherwise land at the wrong tier by
    # omission.
    if ($p -like '*Scripts/Test-Workflow*.ps1')             { return 'Build' }
    # Asserts a property of the shipping image from inside the pre-PR run, so the same
    # hazard applies: a bug here disarms the only check that -falign-functions=64
    # survived to the binary, and the failure it guards is already silent.
    if ($p -like '*Scripts/Test-CodeAlignment.ps1')         { return 'Build' }
    # Nothing invokes it automatically, but a bug in it reports a reproducible Release
    # build that is not one -- a false PASS about the property, which is the same
    # self-concealment. Build rather than Tooling for that reason alone.
    if ($p -like '*Scripts/Test-ReleaseReproducibility.ps1') { return 'Build' }
    # A guard that CI's classify job and Validate-PrePR.ps1 run on every change.
    if ($p -like '*Scripts/Test-ScriptBinding.ps1')         { return 'Build' }
    # Decides whether a build artifact counts as stale, and which binary a measurement
    # reads. Left to the fail-closed default they would be Engine, which costs every PR
    # that touches them the Engine tier. The hazard is the familiar one and it is why
    # they are Build rather than Tooling: a bug in either lets a validation or a
    # measurement run against the wrong binary while reporting success.
    if ($p -like '*Scripts/BuildFreshness.ps1')             { return 'Build' }
    if ($p -like '*Scripts/Get-BuildArtifact.ps1')          { return 'Build' }
    # Lint configuration decides what CI enforces about every source file. Named rather
    # than left to the fail-closed default, which would call it Engine; naming it also
    # lets the self-test assert it.
    if ($p -eq '.clang-format' -or $p -eq '.clang-tidy' -or
        $p -eq '.clang-tidy-deep' -or $p -like '*/.clang-tidy') { return 'Build' }
    if ($p -eq '.git-blame-ignore-revs')                     { return 'Build' }
    if ($p -like '.githooks/*')                              { return 'Build' }
    if ($p -like '.github/*')                                { return 'Build' }
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
    # Their Codex counterparts. The vendored skills' metadata and lock file describe
    # skills; any other file in a skill directory stays with the fail-closed default.
    if ($p -like '.agents/skills/*/agents/openai.yaml')      { return 'Docs' }
    if ($p -like '.codex/agents/*.toml')                     { return 'Docs' }
    if ($p -eq 'skills-lock.json' -or $p -eq 'LICENSE.txt')  { return 'Docs' }
    # Plans are prose whatever the extension, so this one is not '*.md'-scoped.
    if ($p -like '.claude/plans/*')                          { return 'Docs' }
    if ($p -like '*.md')                                     { return 'Docs' }
    if ($p -like 'Docs/*')                                   { return 'Docs' }

    # --- Tooling: helper scripts -----------------------------------------------
    # A script directly in Scripts/ is Tooling unless the Build list above names it.
    # A new gate script left off that list fails -CheckGates, which is why this default
    # is safe here and nowhere else. Subdirectories stay with the fail-closed default.
    if ($p -like 'Scripts/*' -and $p -notlike 'Scripts/*/*' -and
        ($p -like '*.ps1' -or $p -like '*.py' -or $p -like '*.cmd')) { return 'Tooling' }
    # Configures the editor's language server; no build reads it.
    if ($p -eq '.clangd')                                    { return 'Tooling' }

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

# A printed hint, not an invocation: New-PullRequest.ps1 prints the Get-PrChecks.ps1
# command for the user to run next.
$script:GateExempt = @('Get-PrChecks.ps1')

function Get-InvokedScript {
    # The Scripts/ file names a gate's text invokes, in the shapes the gates use:
    # Join-Path ... '<name>', -File <path>, & <path> and . <path>. Comment lines and
    # PowerShell help blocks are skipped, so a mention is not an invocation.
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)

    $name = '(?:Scripts[\\/])?(?<n>[\w-]+\.(?:ps1|py|cmd))'
    $shapes = @(
        "Join-Path\s+\S+\s+['`"]$name"
        "-File\s+['`"]?\S*?$name"
        "(?:^|[\s(])[&.]\s+['`"(]?\.?[\\/]?$name"
    )
    $inHelp = $false
    $lineNo = 0
    foreach ($line in $Text -split "`n") {
        $lineNo++
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith('<#')) { $inHelp = $true }
        if ($inHelp) {
            if ($trimmed.Contains('#>')) { $inHelp = $false }
            continue
        }
        if ($trimmed.StartsWith('#')) { continue }
        foreach ($shape in $shapes) {
            foreach ($m in [regex]::Matches($line, $shape)) {
                [PSCustomObject]@{ Line = $lineNo; Name = $m.Groups['n'].Value }
            }
        }
    }
}

function Find-UnlistedGateScript {
    # Every invocation, in any gate, of a tracked Scripts/ file that is not Build tier
    # and not exempt. Gates maps a gate's path to its text; Scripts lists the file
    # names tracked directly in Scripts/.
    param(
        [Parameter(Mandatory)][hashtable]$Gates,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Scripts
    )

    $invocations = 0
    $unlisted = foreach ($gate in $Gates.Keys | Sort-Object) {
        foreach ($call in @(Get-InvokedScript -Text $Gates[$gate])) {
            if ($call.Name -notin $Scripts) { continue }
            $invocations++
            $tier = Get-TierForPath -Path "Scripts/$($call.Name)"
            if ($tier -ne 'Build' -and $call.Name -notin $script:GateExempt) {
                '{0}:{1} invokes Scripts/{2}, which is {3} tier' -f $gate, $call.Line, $call.Name, $tier
            }
        }
    }
    [PSCustomObject]@{ Invocations = $invocations; Unlisted = @($unlisted) }
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
if ($SelfTest) {
    $cases = @(
        @{ Name = 'docs only';                  Files = @('README.md', 'Measurements/local.md', '.claude/plans/x.md'); Expect = 'Docs' }
        # Named rules, not the '*.md' fallthrough -- and only the Markdown inside those
        # directories; anything else there is still an unfamiliar path.
        @{ Name = 'skill definition -> Docs';   Files = @('.claude/skills/measure-strength/SKILL.md');           Expect = 'Docs' }
        @{ Name = 'agent definition -> Docs';   Files = @('.claude/agents/eval-reviewer.md');                    Expect = 'Docs' }
        @{ Name = 'FAIL CLOSED: skill script';  Files = @('.claude/skills/measure-strength/helper.ps1');         Expect = 'Engine' }
        @{ Name = 'FAIL CLOSED: skill dir';     Files = @('.claude/skills/measure-strength/');                   Expect = 'Engine' }
        @{ Name = 'tooling only';               Files = @('Scripts/Run-EloMatch.ps1');        Expect = 'Tooling' }
        @{ Name = 'docs + tooling -> Tooling';  Files = @('CLAUDE.md', 'Scripts/Run-Tests.ps1'); Expect = 'Tooling' }
        @{ Name = 'UCI driver lib -> Tooling';  Files = @('Scripts/UciDriver.ps1');           Expect = 'Tooling' }
        @{ Name = 'fake engine shim -> Tooling'; Files = @('Scripts/FakeUciEngine.cmd');      Expect = 'Tooling' }
        @{ Name = 'corpus tool -> Tooling';     Files = @('Scripts/build_corpus.py');         Expect = 'Tooling' }
        @{ Name = 'docs + cpp -> Engine';       Files = @('CLAUDE.md', 'StratEngine/Eval.cpp');                 Expect = 'Engine' }
        @{ Name = 'build.ps1 -> Build';         Files = @('build.ps1');                                          Expect = 'Build' }
        @{ Name = 'validator -> Build NOT Tooling'; Files = @('Scripts/Validate-PrePR.ps1');   Expect = 'Build' }
        @{ Name = 'alignment check -> Build';   Files = @('Scripts/Test-CodeAlignment.ps1');   Expect = 'Build' }
        @{ Name = 'reproducibility check -> Build'; Files = @('Scripts/Test-ReleaseReproducibility.ps1'); Expect = 'Build' }
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
        @{ Name = 'ccache path guard -> Build'; Files = @('Scripts/Test-WorkflowCcachePaths.ps1'); Expect = 'Build' }
        # Both decide which binary a build or a measurement reads; named, not fail-closed.
        @{ Name = 'freshness lib -> Build';     Files = @('Scripts/BuildFreshness.ps1');        Expect = 'Build' }
        @{ Name = 'artifact picker -> Build';   Files = @('Scripts/Get-BuildArtifact.ps1');     Expect = 'Build' }
        @{ Name = 'blame-ignore -> Build';      Files = @('.git-blame-ignore-revs');                             Expect = 'Build' }
        @{ Name = 'workflow -> Build';          Files = @('.github/workflows/build-and-test.yml');               Expect = 'Build' }
        @{ Name = 'nightly workflow -> Build';  Files = @('.github/workflows/nightly.yml');                      Expect = 'Build' }
        @{ Name = 'hook -> Build';              Files = @('.githooks/pre-commit');                               Expect = 'Build' }
        @{ Name = 'CMakeLists -> Build';        Files = @('CMakeLists.txt');                                     Expect = 'Build' }
        @{ Name = 'cmake module -> Build';      Files = @('cmake/Toolchain.cmake');                              Expect = 'Build' }
        @{ Name = 'CMakePresets -> Build';      Files = @('CMakePresets.json');                                  Expect = 'Build' }
        @{ Name = 'FAIL CLOSED: unknown ext';   Files = @('foo/bar.xyz');                                        Expect = 'Engine' }
        # Scripts/ defaults to Tooling; -CheckGates keeps the Build list complete.
        @{ Name = 'new script -> Tooling';      Files = @('Scripts/Brand-New.ps1');            Expect = 'Tooling' }
        @{ Name = 'new python script -> Tooling'; Files = @('Scripts/some_new_tool.py');       Expect = 'Tooling' }
        @{ Name = 'FAIL CLOSED: nested script'; Files = @('Scripts/sub/tool.ps1');             Expect = 'Engine' }
        @{ Name = 'FAIL CLOSED: other ext';     Files = @('Scripts/notes.txt');                Expect = 'Engine' }
        @{ Name = 'binding guard -> Build';     Files = @('Scripts/Test-ScriptBinding.ps1');   Expect = 'Build' }
        @{ Name = 'Codex skill meta -> Docs';   Files = @('.agents/skills/grill-me/agents/openai.yaml'); Expect = 'Docs' }
        @{ Name = 'Codex agent -> Docs';        Files = @('.codex/agents/eval-reviewer.toml');  Expect = 'Docs' }
        @{ Name = 'FAIL CLOSED: skill template'; Files = @('.agents/skills/diagnosing-bugs/scripts/hitl-loop.template.sh'); Expect = 'Engine' }
        @{ Name = 'licence + skills lock -> Docs'; Files = @('LICENSE.txt', 'skills-lock.json'); Expect = 'Docs' }
        @{ Name = 'clangd config -> Tooling';   Files = @('.clangd');                          Expect = 'Tooling' }
        @{ Name = 'FAIL CLOSED: hook configs';  Files = @('.claude/settings.json', '.codex/hooks.json'); Expect = 'Engine' }
        @{ Name = 'json -> Engine';             Files = @('StratChessEvolved/game_settings.json');               Expect = 'Engine' }
        @{ Name = 'header -> Engine';           Files = @('StratEngine/Eval.h');                                 Expect = 'Engine' }
        @{ Name = 'backslash paths normalise';  Files = @('Scripts\Run-EloMatch.ps1');         Expect = 'Tooling' }
        @{ Name = 'empty diff';                 Files = @();                                                     Expect = 'Docs' }
        # Whole change sets.
        @{ Name = 'CLAUDE.md only -> Docs';     Files = @('CLAUDE.md');                                          Expect = 'Docs' }
        @{ Name = 'SPRT change set -> Tooling'; Files = @('Scripts/Run-EloMatch.ps1', 'Measurements/local.md', 'CLAUDE.md', 'Docs/Changelog.md', '.claude/plans/elomatch-sprt-support.md'); Expect = 'Tooling' }
    )

    # The gate guard: Got is 'invocations/unlisted names', so a case pins both how many
    # calls it saw and which ones it flagged. The flagged names are untracked, so
    # -CheckGates, which also reads this file, does not flag these fixtures.
    $scripts = @('Run-Lint.ps1', 'Get-PrChecks.ps1', 'New-Gate.ps1', 'Fake-Tool.ps1')
    $gateCases = @(
        @{ Name = 'gate: Join-Path call';       Text = "`$s = Join-Path `$PSScriptRoot 'Run-Lint.ps1'"; Expect = '1/' }
        @{ Name = 'gate: -File call';           Text = 'pwsh -NoProfile -File Scripts/Run-Lint.ps1 -SelfTest'; Expect = '1/' }
        @{ Name = 'gate: call operator';        Text = '$c = & .\Scripts\Run-Lint.ps1 -Check Format'; Expect = '1/' }
        @{ Name = 'gate: dot-source';           Text = ". (Join-Path `$RepoRoot 'Scripts\Run-Lint.ps1')"; Expect = '1/' }
        @{ Name = 'gate: comment is no call';   Text = '# pwsh -File Scripts/Run-Lint.ps1'; Expect = '0/' }
        @{ Name = 'gate: help is no call';      Text = "<#`n    pwsh -File Scripts/Run-Lint.ps1`n#>"; Expect = '0/' }
        @{ Name = 'gate: mention is no call';   Text = "Write-Host 'see Run-Lint.ps1'"; Expect = '0/' }
        @{ Name = 'gate: untracked name';       Text = "Join-Path `$RepoRoot 'build.ps1'"; Expect = '0/' }
        @{ Name = 'gate: exempt hint';          Text = "Join-Path `$PSScriptRoot 'Get-PrChecks.ps1'"; Expect = '1/' }
        # Falsification: a new gate script left off the Build list.
        @{ Name = 'gate: unlisted gate FLAGGED'; Text = 'pwsh -File Scripts/New-Gate.ps1'; Expect = '1/New-Gate.ps1' }
        @{ Name = 'gate: Tooling call FLAGGED'; Text = "& (Join-Path `$PSScriptRoot 'Fake-Tool.ps1')"; Expect = '1/Fake-Tool.ps1' }
    )
    foreach ($g in $gateCases) {
        $r = Find-UnlistedGateScript -Gates @{ 'gate.ps1' = $g.Text } -Scripts $scripts
        $flagged = @($r.Unlisted | ForEach-Object { ($_ -split 'Scripts/')[1].Split(',')[0] }) -join ','
        $cases += @{ Name = $g.Name; Got = "$($r.Invocations)/$flagged"; Expect = $g.Expect }
    }

    $failed = 0
    foreach ($c in $cases) {
        $got = if ($c.ContainsKey('Got')) { $c.Got } else { (Get-ChangeTier -Files $c.Files).Tier }
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
# Gate guard
# ---------------------------------------------------------------------------
if ($CheckGates) {
    $RepoRoot = Split-Path $PSScriptRoot -Parent
    $tracked = @(git -C $RepoRoot ls-files)
    if ($LASTEXITCODE -ne 0 -or $tracked.Count -eq 0) { Write-Host 'CheckGates: git ls-files failed.' -ForegroundColor Red; exit 1 }
    $scriptNames = @($tracked | Where-Object { $_ -like 'Scripts/*' -and $_ -notlike 'Scripts/*/*' } |
        ForEach-Object { $_.Substring('Scripts/'.Length) })
    $gates = @{}
    foreach ($f in $tracked) {
        $isGate = $f -like '.githooks/*' -or $f -like '.github/workflows/*' -or
            ($f -like '*.ps1' -and (Get-TierForPath -Path $f) -eq 'Build')
        if ($isGate) { $gates[$f] = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $f) }
    }
    $r = Find-UnlistedGateScript -Gates $gates -Scripts $scriptNames
    # A scan that finds nothing has stopped reading the gates, not proved them clean.
    if ($r.Invocations -eq 0) { Write-Host "CheckGates: no script invocations found in $($gates.Count) gate files." -ForegroundColor Red; exit 1 }
    foreach ($u in $r.Unlisted) { Write-Host "  FAIL  $u" -ForegroundColor Red }
    if ($r.Unlisted.Count -gt 0) {
        Write-Host 'Add each to the Build list in Get-ChangeTier.ps1, or to $GateExempt if it is not a call.' -ForegroundColor Yellow
        exit 1
    }
    Write-Host "Gate scripts: $($r.Invocations) invocations in $($gates.Count) gate files, all Build tier." -ForegroundColor Green
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
