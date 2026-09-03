<#
.SYNOPSIS
    Pre-PR validation: full parallel build + extended test suite + self-play.

.DESCRIPTION
    1. Builds main solution and test project in parallel.
    2. Runs the full extended test suite (including [slow]).
    3. Runs the exe tactical suite in stability mode (10 consecutive runs of
       Tests/tactical_test_cases.json, 90% threshold per run + no pass/fail flips).
    4. Runs a headless AIPerplex vs AIPerplex self-play game (60s timeout).
    Preceded by cheap text-only gates: clang-format, blame-ignore coverage, workflow
    job timeouts, and the -SelfTest of any changed script that carries one -- or of the
    script that covers it, for a dot-sourced library or a fixture that cannot carry one.
    On every tier, including the Docs and Tooling fast paths, it also checks that every
    Build-tier script carries a -SelfTest at all.
    Every check runs before exit so all failures are visible at once.
    Exits with code 1 if any check fails. Run Validate-PreCommit.ps1 first.

.WHEN TO USE
    Before opening a pull request.

.HOW TO INVOKE (from bash, cmd, or PowerShell)
    pwsh -ExecutionPolicy Bypass -File C:\...\Scripts\Validate-PrePR.ps1

.NOTES
    Prerequisite: game_settings.json must have "type": 6 for both players (AIPerplex vs AIPerplex).
    Must be invoked with -File, not dot-sourced -- a dot-sourced script runs in the
    caller's scope, where its variables collide and its exit ends the caller's session.
#>

[CmdletBinding()]
param(
    # Run every gate regardless of what the diff contains. Use when your judgement
    # disagrees with the classifier.
    [switch]$Force,
    # Ref the diff is computed against when choosing a validation tier.
    [string]$BaseRef = 'origin/main',
    # Acknowledge commits that touch 20+ sources but genuinely change code, so they are not
    # added to .git-blame-ignore-revs. Forwarded to Run-Lint.ps1 -Check BlameIgnore, whose help
    # names this as the answer for exactly that case.
    [switch]$AllowUnlistedReformat,
    # Run this script's own cases and exit. Pure: no build, no git, no toolchain.
    [switch]$SelfTest,
    # Run the -SelfTest of every script that carries one, ignoring the diff, and exit.
    # The nightly entry point: the PR gate runs only the self-tests of scripts a change
    # touched, which is right for a PR but leaves a script broken from elsewhere -- a
    # renamed parameter, an altered shared helper -- unnoticed until someone edits it.
    [switch]$AllSelfTests
)

Set-StrictMode -Version Latest
# Do NOT set $ErrorActionPreference = 'Stop' — this script runs all three checks before
# exiting so the summary table is always printed. Each step checks $LASTEXITCODE directly.

$RepoRoot    = Split-Path $PSScriptRoot -Parent
$GameDir     = Join-Path $RepoRoot 'StratChessEvolved'
$buildScript = Join-Path $RepoRoot 'build.ps1'
$logsDir     = Join-Path $GameDir 'logs'
$outFile     = Join-Path $GameDir 'pre_pr_selfplay_out.txt'
$aiLogFile   = Join-Path $logsDir 'aiperplex.log'
$checkResults = [ordered]@{}

# --- Script self-tests -------------------------------------------------------
# Several Scripts/*.ps1 carry a -SelfTest switch holding synthetic cases that prove
# the script's own logic -- the only coverage those scripts have, since no build or
# test gate can reach them. Naming each one here would mean remembering to add the
# next one, and the ones most likely to be forgotten are the newest. Detecting the
# switch from the parsed AST instead means a script that grows tests gets them run
# from the commit that adds them.
# Pure: which self-test a changed file maps to, or $null for none. A file with no
# -SelfTest of its own can still be covered by one -- a dot-sourced library has no
# param() block to hang the switch on, and a fixture is not a script at all. Without
# the coverer step, a change that breaks one reaches the PR gate having run nothing.
function Resolve-SelfTestFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][bool]$HasOwnSelfTest,
        [Parameter(Mandatory)][hashtable]$Coverer
    )

    if ($HasOwnSelfTest) { return $Path }
    if ($Coverer.ContainsKey($Path)) { return $Coverer[$Path] }
    return $null
}

function Invoke-ChangedScriptSelfTest {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$ChangedFile)

    $anyFailed = $false
    $ran = @()
    foreach ($f in $ChangedFile) {
        $full = Join-Path $RepoRoot $f
        # A deleted file still appears in the diff.
        if (-not (Test-Path -LiteralPath $full)) { continue }

        $hasOwnSelfTest = $false
        if ($f -like '*.ps1') {
            $tokens = $null; $errors = $null
            $ast = [System.Management.Automation.Language.Parser]::ParseInput(
                (Get-Content -LiteralPath $full -Raw), [ref]$tokens, [ref]$errors)
            if ($errors.Count -gt 0) {
                Write-Host "  FAIL  $f (syntax)" -ForegroundColor Red
                $errors | ForEach-Object { Write-Host "        $($_.Message)" -ForegroundColor Red }
                $anyFailed = $true
                continue
            }
            Write-Host "  PASS  $f (syntax)" -ForegroundColor Green

            if ($ast.ParamBlock) {
                $names = @($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
                $hasOwnSelfTest = $names -contains 'SelfTest'
            }
        }

        $selfTestFile = Resolve-SelfTestFile -Path $f -HasOwnSelfTest $hasOwnSelfTest `
            -Coverer $script:SelfTestCoverers
        if ($null -eq $selfTestFile) { continue }
        if ($selfTestFile -ne $f) {
            Write-Host "  ....  $f is covered by $selfTestFile" -ForegroundColor DarkGray
        }

        # Two changed files can name one coverer; it only needs running once.
        if ($ran -contains $selfTestFile) { continue }
        $selfTestPath = Join-Path $RepoRoot $selfTestFile
        if (-not (Test-Path -LiteralPath $selfTestPath)) { continue }

        $ran += $selfTestFile
        # A child pwsh, not dot-sourcing: these scripts exit rather than return, and
        # dot-sourcing one would take this script down with it.
        & pwsh -NoProfile -ExecutionPolicy Bypass -File $selfTestPath -SelfTest | Out-Host
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  FAIL  $selfTestFile (-SelfTest exited $LASTEXITCODE)" -ForegroundColor Red
            $anyFailed = $true
        }
    }
    return [pscustomobject]@{ Failed = $anyFailed; SelfTestsRun = $ran.Count }
}

# --- Self-test coverage rule -------------------------------------------------
# Every Build-tier script must carry a -SelfTest. Those scripts gate validation
# itself, so a bug in one can exempt a real change from the checks and then decline
# to report it -- the same self-concealment hazard that puts them in Build tier.
# The discovery above only runs the self-test of a script the diff touched, which is
# right for a PR gate but means nothing ever requires the switch to exist.
#
# Coverers are enumerated, each naming the script that tests the one on the left,
# rather than derived from a structural rule. The obvious structural rule -- "no
# param() block means it is a dot-sourced library" -- is wrong here:
# Validate-PreCommit.ps1 and Sync-Master.ps1 have no param() block either and are
# ordinary top-level scripts. Fail closed, as Get-ChangeTier.ps1's own allowlist does:
# a new library fails this check until someone classifies it deliberately.
#
# The map is read twice: as the exemption list for the Build-tier rule below, and by
# Invoke-ChangedScriptSelfTest, which runs the coverer when the changed file has no
# -SelfTest of its own. Every entry's coverer is verified whatever the covered file's
# tier -- an unverified entry silently covers nothing once its coverer loses the switch.
$script:SelfTestCoverers = @{
    # Dot-sourced by build.ps1 and Get-BuildArtifact.ps1, so it has no param() block
    # to hang a switch on. build.ps1 -SelfTest asserts its freshness verdicts.
    'Scripts/BuildFreshness.ps1' = 'build.ps1'

    # Dot-sourced by Compare-SearchEquivalence.ps1 and Run-Bench.ps1, and likewise
    # param()-less. Test-UciDriver.ps1 drives it against the fake engine below.
    'Scripts/UciDriver.ps1'      = 'Scripts/Test-UciDriver.ps1'
    'Scripts/FakeUciEngine.ps1'  = 'Scripts/Test-UciDriver.ps1'
    'Scripts/FakeUciEngine.cmd'  = 'Scripts/Test-UciDriver.ps1'
}

# Pure: takes the facts, returns the violations. The walk that produces the facts is
# Get-ScriptSelfTestFact below, kept separate so this can be asserted without a tree.
function Test-SelfTestCoverage {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Fact,
        [Parameter(Mandatory)][hashtable]$Exemption
    )

    $violations = @()
    $byPath = @{}
    foreach ($f in $Fact) { $byPath[$f.Path] = $f }

    foreach ($f in $Fact) {
        if ($f.Tier -ne 'Build') { continue }
        if ($f.HasSelfTest) { continue }
        if (-not $Exemption.ContainsKey($f.Path)) {
            $violations += "$($f.Path) is Build tier and carries no -SelfTest."
        }
    }

    # Every entry is a claim that another script covers this one, and the claim is checked
    # whatever the covered file's tier -- most entries are Tooling, so gating this on Build
    # would leave them unchecked, and an unchecked entry becomes a silent hole the moment
    # its coverer loses its own -SelfTest.
    foreach ($path in $Exemption.Keys) {
        if (-not $byPath.ContainsKey($path)) {
            $violations += "Exemption for '$path' is stale: no such script."
            continue
        }
        if ($byPath[$path].HasSelfTest) {
            $violations += "Exemption for '$path' is stale: it now carries its own -SelfTest."
        }
        $coverer = $Exemption[$path]
        if (-not $byPath.ContainsKey($coverer)) {
            $violations += "'$path' is covered by '$coverer', which does not exist."
        }
        elseif (-not $byPath[$coverer].HasSelfTest) {
            $violations += "'$path' is covered by '$coverer', which has no -SelfTest of its own."
        }
    }

    return @($violations)
}

# Impure half: what is on disk, and what tier each path classifies as.
function Get-ScriptSelfTestFact {
    param([Parameter(Mandatory)][string]$Root)

    $tierScript = Join-Path $Root 'Scripts\Get-ChangeTier.ps1'
    # .cmd as well as .ps1: a coverer entry naming a file this walk cannot see reads as
    # a stale exemption, so the census has to cover everything the map may name.
    $files = @(Get-ChildItem -LiteralPath (Join-Path $Root 'Scripts') -Include *.ps1, *.cmd -File -Recurse -Depth 0) +
             @(Get-Item -LiteralPath (Join-Path $Root 'build.ps1'))

    $facts = @()
    foreach ($file in $files) {
        $rel = if ($file.Name -eq 'build.ps1') { 'build.ps1' } else { "Scripts/$($file.Name)" }

        $hasSelfTest = $false
        if ($file.Extension -eq '.ps1') {
            $tokens = $null; $errors = $null
            $ast = [System.Management.Automation.Language.Parser]::ParseInput(
                (Get-Content -LiteralPath $file.FullName -Raw), [ref]$tokens, [ref]$errors)
            if ($ast.ParamBlock) {
                $names = @($ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
                $hasSelfTest = $names -contains 'SelfTest'
            }
        }

        # In-process, so this is 23 script invocations rather than 23 child processes,
        # and it reuses the classifier instead of restating its rules here.
        $facts += [pscustomobject]@{
            Path        = $rel
            Tier        = (& $tierScript -Paths @($rel)).Tier
            HasSelfTest = $hasSelfTest
        }
    }
    return @($facts)
}

if ($AllSelfTests) {
    Write-Host "`n==> Every script self-test" -ForegroundColor Cyan
    $facts = @(Get-ScriptSelfTestFact -Root $RepoRoot | Where-Object { $_.HasSelfTest })
    $failed = @()
    foreach ($fact in $facts) {
        Write-Host "`n--- $($fact.Path)" -ForegroundColor Cyan
        # A child pwsh, not dot-sourcing: these scripts exit rather than return.
        & pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot $fact.Path) -SelfTest | Out-Host
        if ($LASTEXITCODE -ne 0) { $failed += $fact.Path }
    }

    Write-Host ''
    if ($failed.Count -gt 0) {
        $failed | ForEach-Object { Write-Host "FAIL  $_" -ForegroundColor Red }
        Write-Host "$($failed.Count) of $($facts.Count) script self-test(s) FAILED." -ForegroundColor Red
        exit 1
    }
    # An empty set means discovery broke, not that everything passed.
    if ($facts.Count -eq 0) {
        Write-Host 'FAIL: no script carries a -SelfTest -- discovery is broken.' -ForegroundColor Red
        exit 1
    }
    Write-Host "All $($facts.Count) script self-tests passed." -ForegroundColor Green
    exit 0
}

if ($SelfTest) {
    $fixture = @(
        [pscustomobject]@{ Path = 'build.ps1';                    Tier = 'Build';   HasSelfTest = $true }
        [pscustomobject]@{ Path = 'Scripts/Validate-PrePR.ps1';   Tier = 'Build';   HasSelfTest = $true }
        [pscustomobject]@{ Path = 'Scripts/BuildFreshness.ps1';   Tier = 'Build';   HasSelfTest = $false }
        [pscustomobject]@{ Path = 'Scripts/Run-Bench.ps1';        Tier = 'Tooling'; HasSelfTest = $true }
        [pscustomobject]@{ Path = 'Scripts/UciDriver.ps1';        Tier = 'Tooling'; HasSelfTest = $false }
        [pscustomobject]@{ Path = 'Scripts/Test-UciDriver.ps1';   Tier = 'Tooling'; HasSelfTest = $true }
    )
    # Both shapes the map holds: a Build-tier library, and a Tooling-tier one whose
    # coverer is checked only because every entry is checked.
    $exempt = @{
        'Scripts/BuildFreshness.ps1' = 'build.ps1'
        'Scripts/UciDriver.ps1'      = 'Scripts/Test-UciDriver.ps1'
    }

    function New-Fixture {
        param([object[]]$Base, [string]$Path, [string]$Property, [object]$Value)
        return @($Base | ForEach-Object {
            $copy = [pscustomobject]@{ Path = $_.Path; Tier = $_.Tier; HasSelfTest = $_.HasSelfTest }
            if ($copy.Path -eq $Path) { $copy.$Property = $Value }
            $copy
        })
    }

    $cases = @(
        @{ Name = 'the real shape of the tree is clean'
           Fact = $fixture; Exempt = $exempt; ExpectViolations = 0 }
        # The falsification: this is the situation the rule exists to catch.
        @{ Name = 'FALSIFY: a Build-tier script losing its -SelfTest is caught'
           Fact = (New-Fixture $fixture 'Scripts/Validate-PrePR.ps1' 'HasSelfTest' $false)
           Exempt = $exempt; ExpectViolations = 1 }
        @{ Name = 'a Tooling script without one is not a violation'
           Fact = (New-Fixture $fixture 'Scripts/Run-Bench.ps1' 'HasSelfTest' $false)
           Exempt = $exempt; ExpectViolations = 0 }
        # An exemption is a claim about a coverer; the claim is checked, not trusted.
        @{ Name = 'an exemption whose coverer loses its -SelfTest is caught'
           Fact = (New-Fixture $fixture 'build.ps1' 'HasSelfTest' $false)
           Exempt = $exempt; ExpectViolations = 2 }
        # The same claim, for a coverer of a Tooling-tier file. Checking only the
        # Build-tier entries would leave this one unverified, and the driver's coverage
        # would disappear the moment its test lost the switch.
        @{ Name = 'FALSIFY: a Tooling coverer losing its -SelfTest is caught'
           Fact = (New-Fixture $fixture 'Scripts/Test-UciDriver.ps1' 'HasSelfTest' $false)
           Exempt = $exempt; ExpectViolations = 1 }
        @{ Name = 'an exemption naming a script that does not exist is caught'
           Fact = $fixture; Exempt = @{ 'Scripts/BuildFreshness.ps1' = 'Scripts/Gone.ps1' }
           ExpectViolations = 1 }
        @{ Name = 'a stale exemption for a missing script is caught'
           Fact = $fixture; Exempt = @{ 'Scripts/Deleted.ps1' = 'build.ps1' }
           ExpectViolations = 2 }
        @{ Name = 'a stale exemption for a script that gained a -SelfTest is caught'
           Fact = (New-Fixture $fixture 'Scripts/BuildFreshness.ps1' 'HasSelfTest' $true)
           Exempt = $exempt; ExpectViolations = 1 }
        @{ Name = 'an empty tree is vacuously clean'
           Fact = @(); Exempt = @{}; ExpectViolations = 0 }
    )

    $failed = 0
    foreach ($case in $cases) {
        $violations = @(Test-SelfTestCoverage -Fact $case.Fact -Exemption $case.Exempt)
        if ($violations.Count -eq $case.ExpectViolations) {
            Write-Host "  PASS  $($case.Name)" -ForegroundColor Green
        }
        else {
            $failed++
            Write-Host ("  FAIL  {0}: got {1} violation(s), expected {2}" -f `
                    $case.Name, $violations.Count, $case.ExpectViolations) -ForegroundColor Red
            $violations | ForEach-Object { Write-Host "          $_" -ForegroundColor DarkYellow }
        }
    }

    # Which self-test a changed file maps to. The coverer cases are the ones that matter:
    # before they existed, editing UciDriver.ps1 ran nothing at all.
    $coverer = @{ 'Scripts/UciDriver.ps1' = 'Scripts/Test-UciDriver.ps1' }
    $resolutions = @(
        @{ Name = 'a script with its own -SelfTest runs itself'
           Path = 'Scripts/Run-Bench.ps1'; Own = $true;  Expect = 'Scripts/Run-Bench.ps1' }
        @{ Name = 'a covered library runs its coverer'
           Path = 'Scripts/UciDriver.ps1'; Own = $false; Expect = 'Scripts/Test-UciDriver.ps1' }
        @{ Name = 'an uncovered script without one runs nothing'
           Path = 'Scripts/Sync-Master.ps1'; Own = $false; Expect = $null }
        # Own beats the map: a library that grew a switch tests itself, and the stale
        # check below is what then retires the entry.
        @{ Name = 'a covered file that gained its own -SelfTest runs itself'
           Path = 'Scripts/UciDriver.ps1'; Own = $true;  Expect = 'Scripts/UciDriver.ps1' }
    )
    foreach ($case in $resolutions) {
        $got = Resolve-SelfTestFile -Path $case.Path -HasOwnSelfTest $case.Own -Coverer $coverer
        if ($got -eq $case.Expect) {
            Write-Host "  PASS  $($case.Name)" -ForegroundColor Green
        }
        else {
            $failed++
            Write-Host ("  FAIL  {0}: got '{1}', expected '{2}'" -f $case.Name, $got, $case.Expect) -ForegroundColor Red
        }
    }

    # The fixture above asserts the rule; this asserts the rule against the real tree,
    # so the switch cannot pass while the repository itself violates it.
    Write-Host ''
    $realViolations = @(Test-SelfTestCoverage -Fact (Get-ScriptSelfTestFact -Root $RepoRoot) `
            -Exemption $script:SelfTestCoverers)
    if ($realViolations.Count -eq 0) {
        Write-Host '  PASS  every Build-tier script in the tree carries a -SelfTest' -ForegroundColor Green
    }
    else {
        $failed++
        Write-Host '  FAIL  self-test coverage in the tree:' -ForegroundColor Red
        $realViolations | ForEach-Object { Write-Host "          $_" -ForegroundColor Yellow }
    }

    Write-Host ''
    if ($failed -gt 0) {
        Write-Host "$failed self-test case(s) FAILED." -ForegroundColor Red
        exit 1
    }
    Write-Host "All $($cases.Count + $resolutions.Count + 1) self-test cases passed." -ForegroundColor Green
    exit 0
}

# -AllowMissing: this resolves the path before Step 1 builds it, so on a fresh
# worktree nothing is there yet. Resolved after the -SelfTest dispatch above, which
# must stay free of anything that touches the build.
$gameExe = & (Join-Path $PSScriptRoot 'Get-BuildArtifact.ps1') -AllowMissing

# --- Scope the run to what actually changed (issue #124) ---------------------
# Full build + extended [slow] tests + a 10-run tactical suite + self-play cannot
# catch anything a documentation edit or a measurement script could break, and
# running them anyway burns minutes for a guaranteed pass. Get-ChangeTier.ps1 is
# the single source of truth for this decision, shared with CI
# (.github/workflows/build-and-test.yml) so the two definitions cannot drift.
# It fails closed: anything unrecognised classifies as Engine and gets the full run.
$tierScript = Join-Path $PSScriptRoot 'Get-ChangeTier.ps1'
$change = & $tierScript -BaseRef $BaseRef

if ($Force) {
    Write-Host "`n==> Change tier: $($change.Tier) -- overridden by -Force, running every gate." -ForegroundColor Yellow
} else {
    Write-Host "`n==> Change tier: $($change.Tier) (decided by: $($change.DecidingFile))" -ForegroundColor Cyan
}

# --- Self-test coverage, on every tier ---------------------------------------
# A whole-tree property, not a property of the diff, so it deliberately runs ahead of
# the fast-path exits below: scoping it to changed files would let a new Build-tier
# script land uncovered whenever its own PR happened to touch nothing else in scope.
# Pure AST parsing and in-process classification, so it costs about a second.
Write-Host "`n==> Self-test coverage" -ForegroundColor Cyan
$coverageViolations = @(Test-SelfTestCoverage -Fact (Get-ScriptSelfTestFact -Root $RepoRoot) `
        -Exemption $script:SelfTestCoverers)
if ($coverageViolations.Count -gt 0) {
    $coverageViolations | ForEach-Object { Write-Host "  FAIL  $_" -ForegroundColor Red }
    Write-Host ''
    Write-Host 'Pre-PR validation FAILED (self-test coverage).' -ForegroundColor Red
    Write-Host '      Build-tier scripts gate validation itself, so each must carry a' -ForegroundColor Yellow
    Write-Host '      -SelfTest. Add one, or add a deliberate exemption in this script' -ForegroundColor Yellow
    Write-Host '      naming the script that covers it.' -ForegroundColor Yellow
    exit 1
}
Write-Host '  PASS  every Build-tier script carries a -SelfTest' -ForegroundColor Green

if (-not $Force -and $change.Tier -eq 'Docs') {
    Write-Host 'Docs-only diff -- SKIPPING full build, extended tests, tactical suite and self-play.' -ForegroundColor Green
    Write-Host "The pre-commit hook's fast-test pass is sufficient for documentation changes." -ForegroundColor Green
    Write-Host 'Re-run with -Force to validate anyway.'
    Write-Host ''
    Write-Host 'Pre-PR validation PASSED (docs-only fast path).' -ForegroundColor Green
    exit 0
}

if (-not $Force -and $change.Tier -eq 'Tooling') {
    Write-Host 'Engine-inert tooling diff -- SKIPPING full build, extended tests, tactical suite and self-play.' -ForegroundColor Green
    Write-Host 'These scripts are never compiled and never invoked by the engine, so no' -ForegroundColor Green
    Write-Host 'build/test gate can observe the change. Syntax-checking them and' -ForegroundColor Green
    Write-Host 'running any -SelfTest they carry instead.' -ForegroundColor Green
    Write-Host 'Re-run with -Force to validate anyway.'
    Write-Host ''

    $scriptCheck = Invoke-ChangedScriptSelfTest -ChangedFile $change.ChangedFiles
    Write-Host ''
    if ($scriptCheck.Failed) {
        Write-Host 'Pre-PR validation FAILED (script syntax or self-test).' -ForegroundColor Red
        exit 1
    }
    Write-Host ("Pre-PR validation PASSED (tooling fast path; {0} self-test(s) run)." -f `
            $scriptCheck.SelfTestsRun) -ForegroundColor Green
    exit 0
}

Write-Host 'Running the full gate set.' -ForegroundColor Cyan

# --- Step 0a: build wrapper self-test ---
Write-Host "`n==> Build wrapper self-test" -ForegroundColor Cyan
$buildSelfTestFailed = $false
try   { & $buildScript -SelfTest }
catch { $buildSelfTestFailed = $true; Write-Host "Build self-test threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $buildSelfTestFailed = $true }
$checkResults['Build wrapper self-test'] = if ($buildSelfTestFailed) { 'FAIL' } else { 'PASS' }

# --- Step 0b: clang-format ---
# CI blocks a pull request whose sources are not formatted, so the same answer has
# to be reachable before pushing -- otherwise this is the only gate in the repo that
# can only be discovered after a push. It runs first because it is by far the
# cheapest: seconds against several minutes for the build.
Write-Host "`n==> clang-format (issue #175)" -ForegroundColor Cyan
$lintScript = Join-Path $PSScriptRoot 'Run-Lint.ps1'
$lintFailed = $false
try   { & $lintScript -Check Format -BaseRef $BaseRef }
catch { $lintFailed = $true; Write-Host "Lint threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $lintFailed = $true }
$checkResults['clang-format'] = if ($lintFailed) { 'FAIL' } else { 'PASS' }

# --- Step 0c: blame-ignore coverage ---
# A clang-format configuration change re-runs the formatter over the whole tree,
# so it lands as a commit that rewrites most files without altering a line of
# code. Unless it is recorded in .git-blame-ignore-revs it buries the real
# history of every file it touched -- and nothing else in the pipeline notices.
# Pure git, so it costs nothing.
Write-Host "`n==> Blame-ignore coverage (issue #175)" -ForegroundColor Cyan
$blameFailed = $false
try   { & $lintScript -Check BlameIgnore -BaseRef $BaseRef -AllowUnlistedReformat:$AllowUnlistedReformat }
catch { $blameFailed = $true; Write-Host "Blame-ignore check threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $blameFailed = $true }
$checkResults['Blame-ignore'] = if ($blameFailed) { 'FAIL' } else { 'PASS' }

# --- Step 0d: workflow job timeouts ---
# The classify job enforces this, so without it here a workflow edit that forgets
# timeout-minutes is only discoverable after a push -- the same asymmetry the
# clang-format step above exists to remove. Pure text, so it costs nothing.
Write-Host "`n==> Workflow job timeouts" -ForegroundColor Cyan
$timeoutScript = Join-Path $PSScriptRoot 'Test-WorkflowTimeouts.ps1'
$timeoutFailed = $false
try   { & $timeoutScript }
catch { $timeoutFailed = $true; Write-Host "Timeout guard threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $timeoutFailed = $true }
$checkResults['Workflow timeouts'] = if ($timeoutFailed) { 'FAIL' } else { 'PASS' }

# --- Step 0d2: script parameter binding ---
# Same reasoning as the timeout guard above, for the same reason it is cheap:
# pure text. A script that binds loosely discards an argument it does not know
# and runs its defaults, which for Run-EloMatch.ps1 meant a 500-game match.
Write-Host "`n==> Script parameter binding" -ForegroundColor Cyan
$bindingScript = Join-Path $PSScriptRoot 'Test-ScriptBinding.ps1'
$bindingFailed = $false
try   { & $bindingScript }
catch { $bindingFailed = $true; Write-Host "Binding guard threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $bindingFailed = $true }
$checkResults['Script binding'] = if ($bindingFailed) { 'FAIL' } else { 'PASS' }

# --- Step 0e: self-tests of any changed script ---
# Also run here, not only on the Tooling fast path: a Build- or Engine-tier diff can
# perfectly well change a script that carries tests, and skipping them because the
# diff also touched C++ is the wrong way round. Pure PowerShell, so it costs seconds.
Write-Host "`n==> Script self-tests" -ForegroundColor Cyan
$scriptCheck = Invoke-ChangedScriptSelfTest -ChangedFile $change.ChangedFiles
if ($scriptCheck.SelfTestsRun -eq 0) {
    Write-Host '  No changed script carries a -SelfTest switch.' -ForegroundColor DarkGray
}
$checkResults['Script self-tests'] = if ($scriptCheck.Failed) { 'FAIL' } else { 'PASS' }

# --- Step 1: Full parallel build ---
Write-Host "`n==> Full build (main + tests in parallel)" -ForegroundColor Cyan
# build.ps1 sets $ErrorActionPreference='Stop' internally and calls Write-Error on failure,
# which propagates a terminating error to this script via &. Wrap in try/catch so all three
# checks always run. Track failure via $buildFailed rather than $LASTEXITCODE — when the
# terminating error is caught, $LASTEXITCODE reflects the last native process (cmake/ninja), not
# build.ps1's exit code, so it can't be relied on for the PASS/FAIL decision.
$buildFailed = $false
try   { & $buildScript all }
catch { $buildFailed = $true; Write-Host "Build threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $buildFailed = $true }
$checkResults['Full build'] = if ($buildFailed) { 'FAIL' } else { 'PASS' }

# --- Step 1b: fast clang-tidy Gate ---
# Run after the build so a fresh worktree has the shipping clang-cl compilation
# database the shared local/CI runner requires.
Write-Host "`n==> clang-tidy Gate (issues #175/#284)" -ForegroundColor Cyan
$tidyFailed = $false
try   { & $lintScript -Check Tidy -Profile Gate -BaseRef $BaseRef }
catch { $tidyFailed = $true; Write-Host "clang-tidy Gate threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $tidyFailed = $true }
$checkResults['clang-tidy Gate'] = if ($tidyFailed) { 'FAIL' } else { 'PASS' }

# --- Step 2: Extended test suite ---
Write-Host "`n==> Extended test suite (including [slow])" -ForegroundColor Cyan
$extFailed = $false
try   { & $buildScript extended-tests }
catch { $extFailed = $true; Write-Host "Extended-tests threw: $_" -ForegroundColor DarkGray }
if ($LASTEXITCODE -ne 0) { $extFailed = $true }
$checkResults['Extended tests'] = if ($extFailed) { 'FAIL' } else { 'PASS' }

# --- Step 3: Exe tactical suite (stability mode) ---
# Guards the 90% pass threshold on Tests/tactical_test_cases.json. Issue #66
# (QFORK-001 silently regressed to 7/8) went unnoticed because no automated
# gate ran this suite. Stability mode (10 consecutive runs, no pass/fail
# flips allowed) additionally detects nondeterministic search results — the
# cheap intermittent-race-bug signal once Lazy SMP threads share the TT.
# ~11 s total; trivially deterministic (hence green) on single-threaded builds.
Write-Host "`n==> Tactical suite (StratChessEvolved.exe tactical stability 10)" -ForegroundColor Cyan
$testsDir = Join-Path $RepoRoot 'Tests'
Push-Location $testsDir
try {
    $tacticalFailed = $false
    try   { & $gameExe tactical stability 10 }
    catch { $tacticalFailed = $true; Write-Host "Tactical suite threw: $_" -ForegroundColor DarkGray }
    if ($LASTEXITCODE -ne 0) { $tacticalFailed = $true }
    $checkResults['Tactical suite'] = if ($tacticalFailed) { 'FAIL' } else { 'PASS' }
} finally {
    Pop-Location
}

# --- Step 4: Self-play ---
Write-Host "`n==> Self-play (AIPerplex vs AIPerplex, 60s timeout)" -ForegroundColor Cyan

# Ensure logs/ exists — spdlog silently fails without it
if (-not (Test-Path $logsDir)) {
    New-Item -ItemType Directory -Path $logsDir | Out-Null
    Write-Host "  Created missing logs/ directory." -ForegroundColor DarkGray
}

Push-Location $GameDir
try {
    if (Test-Path $outFile) { Remove-Item $outFile }
    # Truncate aiperplex.log before run so we only count moves from this game
    if (Test-Path $aiLogFile) { Clear-Content $aiLogFile }
    $proc   = Start-Process $gameExe -ArgumentList 'game' -PassThru -NoNewWindow -RedirectStandardOutput $outFile
    $exited = $proc.WaitForExit(60000)
    if (-not $exited) { $proc.Kill() }

    # GetMove complete: is written to logs/aiperplex.log by s_logger (file sink).
    # RedirectStandardOutput does not capture spdlog's stdout_color_sink on Windows
    # (spdlog uses WriteConsoleW which bypasses the C-runtime stdout redirect).
    $logOutput = (Test-Path $aiLogFile) ? [string](Get-Content $aiLogFile -Raw) : ''
    $moveCount = ([regex]::Matches($logOutput, 'GetMove complete:')).Count

    # Require at least 2 completed moves (one per side) — confirms the search engine
    # is functioning. With time_limit:15000ms per move, a 60s timeout window yields
    # ~3-4 moves; the game will not complete naturally in that window, so we do not
    # require game-termination (checkmate/stalemate/draw). Explicit parentheses guard
    # against PowerShell's left-to-right -and/-or precedence.
    if ($moveCount -ge 2) {
        Write-Host "PASS: $moveCount move(s) logged; engine is functional." -ForegroundColor Green
        $checkResults['Self-play'] = 'PASS'
    } else {
        Write-Host "FAIL: $moveCount move(s) logged. Expected at least 2." -ForegroundColor Red
        Write-Host "Log tail (aiperplex.log):" -ForegroundColor Yellow
        $logOutput -split "`n" | Select-Object -Last 10 | ForEach-Object { Write-Host "  $_" }
        $checkResults['Self-play'] = 'FAIL'
    }
} finally {
    Pop-Location
    if (Test-Path $outFile) { Remove-Item $outFile -ErrorAction SilentlyContinue }
}

# --- Summary table ---
Write-Host "`n--- Pre-PR Validation Summary ---" -ForegroundColor Cyan
Write-Host ("  {0,-22} {1}" -f 'Change tier', $change.Tier) -ForegroundColor DarkGray
$anyFailed = $false
foreach ($check in $checkResults.Keys) {
    $status = $checkResults[$check]
    $color  = ($status -eq 'PASS') ? 'Green' : 'Red'
    Write-Host ("  {0,-22} {1}" -f $check, $status) -ForegroundColor $color
    if ($status -ne 'PASS') { $anyFailed = $true }
}
Write-Host ""

if ($anyFailed) {
    Write-Host "Pre-PR validation FAILED." -ForegroundColor Red
    exit 1
} else {
    Write-Host "Pre-PR validation PASSED." -ForegroundColor Green
    exit 0
}
